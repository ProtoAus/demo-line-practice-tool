#!/usr/bin/env python3
# wrpath_extract.py  --  Momentum Mod ".mtv" run demo  ->  ".wrpath" path cache.
#
# WrLines draws other players' run paths as lines in the world while you play.
# This script is the offline half: it reads the demos the game already downloaded
# and writes a small binary path file that wrlines.dll loads and draws. The DLL
# never parses .mtv itself.
#
# HOW IT WORKS
#   A .mtv is Momentum's own container ("MMTV"): a packed binary header, a JSON
#   run-stats blob, then the run body compressed with Valve-LZMA (or zstd on the
#   biggest files). The body is a Source entity-delta netstream, NOT a flat frame
#   array -- there is no public spec for it, and the send-table metadata you would
#   need to decode it properly lives in the closed-source game DLL.
#
#   We do not decode it properly. We don't have to. m_vecOrigin is networked as
#   three raw IEEE-754 float32, so we scan the decompressed body for every bit
#   position holding a plausible coordinate triple, and then find the longest
#   physically-smooth chain through those candidates.
#
#   "Longest smooth chain" is a dynamic program, not a greedy walk. That matters:
#   several other fields (velocity, wishVel) also form long smooth chains, the
#   stride between player frames is variable -- 216 and 400 bits are both common,
#   because Source only sends the props that actually changed -- and a greedy walk
#   that takes one wrong turn loses the rest of the run. The DP keeps the best
#   predecessor for every candidate, so a single bad edge costs nothing.
#
#   Picking the right chain out of the several the DP can find is exact and
#   self-validating: the JSON header records the run's own maxHorizontalSpeed, and
#   the real path is the chain whose step lengths reproduce it. In practice it
#   matches to ~0.01 u/s, which is not something a wrong chain does by accident.
#   If no chain matches, we say so instead of writing a plausible-looking lie.
#
# WHAT THIS DELIBERATELY DOESN'T DO
#   - It does not decode the netstream properly. No send tables, no prop flags.
#     It is a pattern-matcher over floats, and it reports when it is unsure.
#   - It does not reach 100% of ticks on every run and is not supposed to. Source
#     delta-compresses: a tick where the player did not move re-sends no origin,
#     and such ticks contribute no path.
#   - It does not use the velocity floats that sit near the origin in the stream.
#     They are not at a reliable offset across runs. Velocity here is a finite
#     difference of positions, which is always available and always consistent.
#   - It does not touch the game install. Reads only; writes only under --out.
#   - It has no zstd support unless the "zstandard" package is installed. That
#     affects ~142 of 4035 shipped demos (the very largest); they are reported as
#     skipped rather than guessed at.
#
# USAGE
#   python wrpath_extract.py --list
#   python wrpath_extract.py --map surf_demise
#   python wrpath_extract.py --map surf_demise --verify
#   python wrpath_extract.py --file "...\online\1\a7de....mtv"
#   python wrpath_extract.py --all --limit 50

import argparse
import json
import lzma
import bisect
import math
import os
import struct
import sys
import time
import zlib

try:
    import zstandard as _zstd
except ImportError:
    _zstd = None

# ---------------------------------------------------------------------------
# Locations
# ---------------------------------------------------------------------------

DEFAULT_GAME = r"C:\Program Files (x86)\Steam\steamapps\common\Momentum Mod Playtest"
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.join(SCRIPT_DIR, "wrlines_data", "paths")

# Bumped whenever anything that decides whether a demo can be extracted changes.
#
# Failures are recorded per map (see FAILURES_FILE) so that re-running a map
# costs seconds instead of minutes -- surf_colin_blaster_69000 has 66 demos that
# all fail, and re-deriving that takes four and a half minutes every time. The
# record carries this number, and a record written by a different revision is
# ignored, so improving the extractor automatically retries everything it
# previously gave up on. Nobody has to remember to delete a file.
#
# Keep in sync with WR_EXTRACTOR_REVISION in wr_extract.h, which reads the same
# file to report the count in-game. If they ever disagree, the in-game count
# simply falls back to treating those demos as unprocessed, which is the safe
# direction: it offers to do work that has already been done, rather than hiding
# work that has not.
EXTRACTOR_REVISION = 2
FAILURES_FILE = "_failed.txt"

# ---------------------------------------------------------------------------
# .mtv container
# ---------------------------------------------------------------------------

MMTV_MAGIC = b"MMTV"
ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"

# Fixed header fields. Everything up to ~0xBD is at a constant offset in both
# container versions; the JSON start is NOT (0xC6 on v1, 0xC7 on v2), so it is
# found by scanning and validated by the length prefix that precedes it.
OFF_VERSION = 0x04
OFF_DATE_MS = 0x08
OFF_MAPNAME = 0x10
OFF_MAPHASH = 0x50
OFF_GAMEMODE = 0x79
OFF_TICKRATE = 0x7B
OFF_STEAMID = 0x7F
OFF_PLAYER = 0x87
OFF_TRACKTYPE = 0xA7
OFF_TRACKNUM = 0xA8
OFF_RUNTIME = 0xA9
OFF_TICKS = 0xB1

GAMEMODE_NAMES = {
    1: "surf", 2: "bhop", 3: "bhop_hl1", 4: "climb_mom", 5: "climb_kzt",
    6: "climb_16", 7: "rj", 8: "sj", 9: "ahop", 10: "conc",
    11: "defrag_cpm", 12: "defrag_vq3", 13: "defrag_vtg",
}
TRACKTYPE_NAMES = {0: "main", 1: "stage", 2: "bonus"}


class MtvError(Exception):
    pass


def _cstr(buf, off, size):
    raw = buf[off:off + size]
    end = raw.find(b"\0")
    if end >= 0:
        raw = raw[:end]
    return raw.decode("utf-8", "replace")


def parse_mtv_header(data, path_hint=""):
    """Parse the fixed header + JSON blob. Returns a dict, raises MtvError."""
    if len(data) < 0x200 or data[:4] != MMTV_MAGIC:
        raise MtvError("not an MMTV file")

    h = {"path": path_hint}
    h["version"] = struct.unpack_from("<I", data, OFF_VERSION)[0]
    h["date_ms"] = struct.unpack_from("<q", data, OFF_DATE_MS)[0]
    h["map"] = _cstr(data, OFF_MAPNAME, 64)
    h["map_hash"] = _cstr(data, OFF_MAPHASH, 41)
    h["gamemode"] = data[OFF_GAMEMODE]
    h["tick_interval"] = struct.unpack_from("<f", data, OFF_TICKRATE)[0]
    h["steamid64"] = struct.unpack_from("<Q", data, OFF_STEAMID)[0]
    h["player"] = _cstr(data, OFF_PLAYER, 32)
    h["track_type"] = data[OFF_TRACKTYPE]
    h["track_num"] = data[OFF_TRACKNUM]
    h["run_time"] = struct.unpack_from("<d", data, OFF_RUNTIME)[0]
    h["ticks"] = struct.unpack_from("<I", data, OFF_TICKS)[0]

    # Sanity gates. If these fail, the offsets have moved and we should not
    # pretend any of the values mean anything.
    if not h["map"]:
        raise MtvError("empty map name field")
    if not (0.001 <= h["tick_interval"] <= 0.1):
        raise MtvError("tick interval %r out of range" % h["tick_interval"])
    if (h["steamid64"] >> 32) != 0x01100001:
        raise MtvError("steamid64 high word wrong (0x%X)" % (h["steamid64"] >> 32))

    # Locate the JSON: a '{' in a bounded window, with the u32 byte-length
    # immediately before it and the compressed-body magic immediately after it.
    # That triple is self-validating and holds for both container versions.
    #
    # Try every '{' in the window rather than only the first. The player name and
    # the padding around it are arbitrary bytes, so a stray '{' before the real
    # blob is perfectly possible -- and taking it on faith is what produced
    # "implausible JSON length 1076353433" on two files. The length and codec
    # checks below already tell us when we have the wrong one, so keep looking
    # instead of giving up.
    json_start = -1
    json_len = 0
    codec = None
    probe = data.find(b"{", 0xB0, 0x200)
    tried = []
    while probe >= 0:
        n = struct.unpack_from("<I", data, probe - 4)[0]
        tried.append(n)
        if 0 < n < (1 << 20) and probe + n + 4 <= len(data):
            tail = data[probe + n:probe + n + 4]
            if tail == b"LZMA":
                json_start, json_len, codec = probe, n, "lzma"
                break
            if tail == ZSTD_MAGIC:
                json_start, json_len, codec = probe, n, "zstd"
                break
        probe = data.find(b"{", probe + 1, 0x200)

    if json_start < 0:
        raise MtvError("no JSON blob in header window (lengths tried: %s)" %
                       ", ".join(str(t) for t in tried[:4]) if tried
                       else "no '{' in header window")

    h["codec"] = codec
    body_off = json_start + json_len

    h["body_off"] = body_off
    try:
        h["json"] = json.loads(data[json_start:json_start + json_len].rstrip(b"\0"))
    except ValueError as e:
        h["json"] = None
        h["json_error"] = str(e)
    return h


def decompress_body(data, h):
    """Return the decompressed run body, or raise MtvError."""
    off = h["body_off"]
    if h["codec"] == "lzma":
        # Valve's 17-byte container: id | actualSize | lzmaSize | 5 prop bytes,
        # then a raw LZMA1 stream. Not .xz and not .lzma: no end marker, and the
        # uncompressed size is known up front.
        actual, comp = struct.unpack_from("<II", data, off + 4)
        props = data[off + 12:off + 17]
        b = props[0]
        lc = b % 9
        rest = b // 9
        lp = rest % 5
        pb = rest // 5
        dict_size = struct.unpack_from("<I", props, 1)[0]
        if off + 17 + comp > len(data):
            raise MtvError("LZMA block runs past EOF")
        dec = lzma.LZMADecompressor(
            format=lzma.FORMAT_RAW,
            filters=[{"id": lzma.FILTER_LZMA1, "lc": lc, "lp": lp,
                      "pb": pb, "dict_size": dict_size}])
        out = dec.decompress(data[off + 17:off + 17 + comp], actual)
        if len(out) != actual:
            raise MtvError("LZMA short read: %d of %d" % (len(out), actual))
        return out

    if h["codec"] == "zstd":
        if _zstd is None:
            raise MtvError("zstd body but the 'zstandard' package is not installed "
                           "(pip install zstandard)")
        return _zstd.ZstdDecompressor().decompress(data[off:])

    raise MtvError("unknown codec %r" % h["codec"])


# ---------------------------------------------------------------------------
# Path extraction
# ---------------------------------------------------------------------------

# NOT the 16384 that every Source reference quotes. Strata's maps are bigger:
# measured from the demos themselves, surf_colin_blaster_69000 reaches -31295 on
# X. With 16384 this filter chopped that map's origin stream into fragments, and
# 66 of its 141 demos could not be extracted at all -- the DP could only chain
# 8-38% of ticks, and where it did find a long chain the implied speed was
# nonsense because it was stitching across the gaps.
#
# Widening it is free: re-running ten surf_demise demos at 65536 gives results
# byte-identical to 16384, because this only ever threw out floats that are
# nowhere near a coordinate. Keep in sync with WR_WORLD_LIMIT in wr_common.h.
WORLD_LIMIT = 65536.0

# Largest plausible movement between consecutive ticks, in world units. The
# fastest runs on disk peak near 4900 u/s, which is ~74 units at a 0.015 s tick;
# this leaves generous headroom for defrag/rocket-jump speeds.
MAX_STEP = 200.0

# Bit-gap window between consecutive player frames. A frame cannot begin before
# the 96 bits the origin itself occupies; the upper bound only has to cover the
# worst interleaving of other entities' deltas, and is also derived per-file from
# the average bits-per-tick.
GAP_MIN = 96
GAP_MAX_FLOOR = 3000

# Smoothness gate. Consecutive step vectors may differ by at most
# TOL_FLOOR + TOL_FRAC * (longer of the two steps). This is what stops the DP
# from stitching together candidates that merely happen to be nearby.
TOL_FLOOR = 10.0
TOL_FRAC = 0.80

# float32 exponent window, used as an integer prefilter. A magnitude in
# [~1e-3, 32768) has a biased exponent in [117, 141]. Testing raw bits throws
# away ~98% of positions before any float conversion happens.
EXP_LO, EXP_HI = 117, 141

# How closely a chain's step lengths must reproduce the run's own recorded
# maxHorizontalSpeed to be accepted as the player's path. A correct chain lands
# within ~0.01 u/s; this bound is loose enough for a missing tick here or there.
MATCH_ABS = 1.0
MATCH_REL = 0.005

MIN_CHAIN = 32
MAX_IDENTIFY_ROUNDS = 14

# Vertical-velocity test (see origin_stream_score). A position chain scores ~0.75
# on real data; velocity and projectile chains score under 0.09, so half is a
# comfortable line. DERIV_MIN_STEPS guards against the offset search finding a
# coincidence on a handful of samples.
DERIV_MIN_FRACTION = 0.5
DERIV_MIN_STEPS = 10
DERIV_TOL_ABS = 25.0
DERIV_TOL_REL = 0.05

# The offset search takes the best of ~500 offsets, so a small sample can throw
# up a coincidence. Rather than demand a large sample outright -- short stage
# runs simply do not have many airborne ticks -- scale the bar to the evidence:
# a marginal rate needs plenty of steps, a near-perfect one needs fewer. At 12
# steps and a 5% tolerance, reaching 70% by chance across the whole offset sweep
# is well under a percent.
DERIV_SMALL_SAMPLE = 25
DERIV_SMALL_FRACTION = 0.7

# Upper bound on how many steps the offset sweep looks at. Bounds a cost that is
# otherwise quadratic in run length; see origin_stream_score.
DERIV_MAX_SAMPLES = 400

# Knowing which stream is the origin does not guarantee enough of it survived to
# be worth drawing. Below this, report a failure instead of writing a stub.
DERIV_MIN_COVERAGE = 0.25


def _origin_confirmed(rate, steps):
    if steps < DERIV_MIN_STEPS:
        return False
    if steps >= DERIV_SMALL_SAMPLE:
        return rate >= DERIV_MIN_FRACTION
    return rate >= DERIV_SMALL_FRACTION

# Teleports. A staged map teleports the player between legs, and the smoothness
# gate above cannot step across that -- by design, since allowing a 10000-unit
# step is exactly how a chain wanders off into unrelated data. So on a staged map
# the DP returns whichever single leg is longest and drops the rest: two
# surf_tensor2 main-track runs came out at 8.8% and 3.9% of their ticks, which is
# one stage each.
#
# The fix is to run the DP again over the bit ranges the accepted chains do not
# cover, and stitch the results together in stream order. See harvest_segments.
MIN_SEGMENT = 24
MAX_SEGMENTS = 24

# A recovered segment must not imply a speed the run never reached. This is what
# stops a velocity or wishvel field being mistaken for a leg of the path -- read
# as positions, those produce accelerations-as-speeds, which are wildly too big.
SEGMENT_SPEED_SLACK = 0.25

# ...except while the player is idle, when the velocity field sits at nearly zero
# and forms a long, perfectly smooth, slow chain that the speed gate is happy
# with. It gives itself away by being a stationary cloud at the world origin: an
# 18-minute practice demo produced one 1245 samples long, spanning 90 units,
# centred 4 units from (0,0,0).
#
# Both halves of that are worth rejecting on their own terms. A leg that never
# travels this far is not a route worth drawing wherever it is, and a cluster
# this tight sitting on the world origin is not a place a player can be.
SEGMENT_MIN_EXTENT = 256.0
ORIGIN_CLUSTER_RADIUS = 512.0

# Coverage below this is reported as low-confidence even when the speed oracle
# passed. It passing only proves the leg we found is real, not that we found the
# whole run -- which is exactly how the two broken files above got written
# without a warning.
MIN_COVERAGE_CONFIDENT = 0.60

# When the speed check fails outright, a chain must still span at least this
# fraction of the run's ticks before we will keep it as a low-confidence path.
LOW_CONFIDENCE_MIN_COVERAGE = 0.50


def _plausible_bits(w):
    if w == 0 or w == 0x80000000:
        return True
    e = (w >> 23) & 0xFF
    return EXP_LO <= e <= EXP_HI


def scan_candidates(buf, start_byte=0):
    """Find every bit position holding a plausible (x, y, z) float triple.

    Returns {bitpos: (x, y, z)}.

    The buffer is shifted by each of the 8 bit phases so every float becomes
    byte-aligned, which lets struct decode them in bulk instead of one Python
    call per bit. Within a shifted stream, array index j is bit
    (phase + 8*q + 32*j), so a coordinate triple is simply three adjacent
    entries.
    """
    n = len(buf)
    if n < 64:
        return {}
    whole = int.from_bytes(buf, "little")
    out = {}
    skip = start_byte // 4

    for phase in range(8):
        sb = (whole >> phase).to_bytes(n, "little")
        for q in range(4):
            view = sb[q:]
            count = len(view) // 4
            if count < 8:
                continue
            words = struct.unpack_from("<%dI" % count, view, 0)
            base = phase + 8 * q
            for j in range(skip, count - 3):
                wx = words[j]
                if not _plausible_bits(wx):
                    continue
                wy = words[j + 1]
                if not _plausible_bits(wy):
                    continue
                wz = words[j + 2]
                if not _plausible_bits(wz):
                    continue
                x, y, z = struct.unpack("<3f", struct.pack("<3I", wx, wy, wz))
                if (-WORLD_LIMIT <= x <= WORLD_LIMIT
                        and -WORLD_LIMIT <= y <= WORLD_LIMIT
                        and -WORLD_LIMIT <= z <= WORLD_LIMIT):
                    out[base + 32 * j] = (x, y, z)
    return out


# A per-demo deadline.
#
# The dynamic program below is quadratic in the worst case, and some demos are
# genuinely slow -- 68 s each on surf_colin_blaster_69000 against 0.7 s on a
# normal map. That is not a hang, but it looks exactly like one: the extractor
# rattles through four demos and then sits on the fifth. So there is now a limit,
# it is reported as an ordinary failure with the reason given, and the failure
# record means it is not paid twice.
_deadline = [0.0, 0.0]      # [absolute give-up time, the limit that produced it]


def set_deadline(seconds):
    if seconds and seconds > 0:
        _deadline[0] = time.time() + seconds
        _deadline[1] = seconds
    else:
        _deadline[0] = 0.0
        _deadline[1] = 0.0


def check_deadline(where):
    if _deadline[0] and time.time() > _deadline[0]:
        raise MtvError("gave up after %.0f s in %s -- pass --timeout 0 for no "
                       "limit, or a larger value" % (_deadline[1], where))


def longest_smooth_chain(cands, keys, banned, gap_max):
    """Longest physically-smooth chain of candidates, by dynamic programming.

    dp[j] is the length of the best chain ending at candidate j, so a wrong edge
    anywhere costs only that edge -- unlike a greedy walk, which loses the entire
    remainder of the run.
    """
    ks = [k for k in keys if k not in banned] if banned else keys
    n = len(ks)
    if n < 2:
        return []
    pos = [cands[k] for k in ks]

    dp = [1] * n
    pred = [-1] * n
    step = [None] * n

    for i in range(n):
        if (i & 0xFFF) == 0:
            check_deadline("the chain search")
        bi = ks[i]
        xi, yi, zi = pos[i]
        si = step[i]
        di = dp[i]
        limit = bi + gap_max
        j = i + 1
        while j < n and ks[j] <= limit:
            if ks[j] - bi >= GAP_MIN:
                xj, yj, zj = pos[j]
                sx = xj - xi
                sy = yj - yi
                sz = zj - zi
                sl = math.sqrt(sx * sx + sy * sy + sz * sz)
                if sl <= MAX_STEP and di + 1 > dp[j]:
                    ok = True
                    if si is not None:
                        dvx = sx - si[0]
                        dvy = sy - si[1]
                        dvz = sz - si[2]
                        dv = math.sqrt(dvx * dvx + dvy * dvy + dvz * dvz)
                        pl = math.sqrt(si[0] ** 2 + si[1] ** 2 + si[2] ** 2)
                        if dv > TOL_FLOOR + TOL_FRAC * (sl if sl > pl else pl):
                            ok = False
                    if ok:
                        dp[j] = di + 1
                        pred[j] = i
                        step[j] = (sx, sy, sz)
            j += 1

    best = max(range(n), key=dp.__getitem__)
    out = []
    while best != -1:
        out.append(ks[best])
        best = pred[best]
    out.reverse()
    return out


def make_float_reader(body):
    """Read a float32 at any BIT position. Shares scan_candidates' trick of
    pre-shifting the whole buffer by each of the 8 phases so a read is a plain
    struct call rather than bit surgery."""
    whole = int.from_bytes(body, "little")
    shifted = [(whole >> ph).to_bytes(len(body), "little") for ph in range(8)]
    n = len(body)

    def f32(p):
        if p < 0 or (p >> 3) + 4 > n:
            return None
        return struct.unpack_from("<f", shifted[p & 7], p >> 3)[0]
    return f32


def origin_stream_score(f32, cands, chain, dt, frame_bits):
    """Does the stream carry this chain's own vertical velocity beside it?

    Returns (hit_rate, bit_offset, steps_tested).

    This is how a position stream is told from a velocity one WITHOUT relying on
    magnitude, and it is the only thing that works when a map's stage sits near
    the world origin. There the player's coordinates are a few hundred units --
    the same size as a velocity vector -- so "is this too big to be a position?"
    answers nothing, and the DP happily returns a velocity chain instead. Five of
    surf_colin_blaster_69000's tracks are laid out that way.

    Source networks position and velocity in the same entity delta, so if a chain
    is the origin then its own derivative is sitting a few dozen bits away. Only
    the VERTICAL component turns out to be reliably present in these demos -- a
    three-component match scores 0.19 even on a chain known to be correct -- so
    match on vz alone.

    One float matching once proves nothing. What makes this decisive is that it
    must match at the SAME relative bit offset on most steps, because the send
    table layout is fixed. Measured on a known-good chain: 74.6% at offset 128,
    against 1.9-8.2% for every velocity and projectile chain in the same file.

    Tolerance covers the real difference between the two quantities:
    (z[i+1] - z[i]) / dt is the AVERAGE velocity across the tick, while the
    networked value is instantaneous, and they differ by about half a tick of
    gravity -- 6 u/s.
    """
    if len(chain) < 12 or frame_bits <= 0:
        return 0.0, -1, 0

    steps = []
    for i in range(len(chain) - 1):
        gap = chain[i + 1] - chain[i]
        if gap > frame_bits * 1.6:
            continue                    # not consecutive frames; not one tick
        vz = (cands[chain[i + 1]][2] - cands[chain[i]][2]) / dt
        if abs(vz) < 60.0:
            continue                    # level flight: vz ~ 0 matches far too much
        steps.append((chain[i], vz))

    # Below this the best-of-N offset search finds coincidences, so refuse to
    # answer rather than answer badly.
    if len(steps) < DERIV_MIN_STEPS:
        return 0.0, -1, len(steps)

    # Cost is offsets x steps, and both grow with the run: a 30000-point chain
    # over ~500 offsets is 15 million float reads, per round, up to 14 rounds.
    # A 110000-tick demo would take hours of that. Subsample evenly instead --
    # the measured separation was 74.6% for a real chain against 1.9-8.2% for
    # everything else using 524 steps, so a few hundred is far more than enough
    # to tell those apart.
    total_steps = len(steps)
    if total_steps > DERIV_MAX_SAMPLES:
        stride = total_steps / DERIV_MAX_SAMPLES
        steps = [steps[int(i * stride)] for i in range(DERIV_MAX_SAMPLES)]

    best, bestoff = 0.0, -1
    for off in range(0, int(frame_bits * 1.2)):
        hit = 0
        for b, vz in steps:
            w = f32(b + off)
            if w is not None and abs(w - vz) <= max(DERIV_TOL_ABS,
                                                    DERIV_TOL_REL * abs(vz)):
                hit += 1
        if hit > best * len(steps):
            best, bestoff = hit / len(steps), off
    return best, bestoff, len(steps)


def _peak_horizontal_speed(pts, dt):
    """Max horizontal speed implied by the chain's own step lengths.

    Steps above the 99.5th percentile are dropped: those are the places where a
    tick is missing from the stream, and a two-tick step would read as double the
    real speed.
    """
    if len(pts) < 8:
        return 0.0
    lens = sorted(math.dist(pts[i], pts[i + 1]) for i in range(len(pts) - 1))
    cap = lens[int(0.995 * (len(lens) - 1))]
    peak = 0.0
    for i in range(len(pts) - 1):
        ax, ay, az = pts[i]
        bx, by, bz = pts[i + 1]
        if math.dist(pts[i], pts[i + 1]) > cap:
            continue
        v = math.hypot(bx - ax, by - ay) / dt
        if v > peak:
            peak = v
    return peak


def _segment_plausible(pts, dt, ref, deriv=None):
    """Could this chain be a leg of the player's path?

    `deriv` is the derivative-support score when it could be measured. Near the
    world origin a velocity chain is the same size as a position one, so the
    magnitude tests below cannot separate them and this is the only thing that
    can. A clear negative overrides everything else.
    """
    if len(pts) < MIN_SEGMENT:
        return False
    if deriv is not None and deriv < DERIV_MIN_FRACTION:
        return False

    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    zs = [p[2] for p in pts]
    extent = math.dist((min(xs), min(ys), min(zs)),
                       (max(xs), max(ys), max(zs)))
    if extent < SEGMENT_MIN_EXTENT:
        return False

    n = len(pts)
    centroid = (sum(xs) / n, sum(ys) / n, sum(zs) / n)
    if math.dist(centroid, (0.0, 0.0, 0.0)) < ORIGIN_CLUSTER_RADIUS:
        return False

    peak = _peak_horizontal_speed(pts, dt)
    if ref is not None and ref > 0:
        return peak <= ref * (1.0 + SEGMENT_SPEED_SLACK) + 50.0
    return peak <= 6000.0


def harvest_segments(cands, keys, first, gap_max, dt, ref, frame_bits=0, f32=None):
    """Recover the legs of a run that teleports, as a list of chains.

    The first chain has already been identified as the real path by the speed
    oracle, which tells us *which* data in the stream is the origin. What it does
    not do is span a teleport. So we look again -- but only at bit ranges no
    accepted chain covers.

    That restriction is what keeps this honest. The velocity and wishvel fields
    that also form long smooth chains are networked in the same frames as the
    origin, so they sit inside an already-accepted bit range and cannot be picked
    up by mistake. Anything found outside those ranges comes from a stretch of
    the run we have no samples for at all, which is precisely what is missing
    after a teleport.

    Chains that survive the range test but imply an impossible speed are banned
    individually rather than by range, so rejecting one cannot hide a real leg
    that overlaps it.
    """
    segments = [first]
    spans = [(min(first), max(first))]
    banned = set()

    for _ in range(MAX_SEGMENTS * 2):
        outside = [k for k in keys
                   if k not in banned
                   and not any(lo <= k <= hi for lo, hi in spans)]
        if len(outside) < MIN_SEGMENT:
            break
        chain = longest_smooth_chain(cands, outside, None, gap_max)
        if len(chain) < MIN_SEGMENT:
            break
        deriv = None
        if f32 is not None and frame_bits:
            rate, _, ns = origin_stream_score(f32, cands, chain, dt, frame_bits)
            # Only treat the score as evidence when it had enough samples to
            # mean something; otherwise leave the decision to the other tests.
            if ns >= DERIV_MIN_STEPS:
                deriv = 1.0 if _origin_confirmed(rate, ns) else 0.0
        if _segment_plausible([cands[b] for b in chain], dt, ref, deriv):
            segments.append(chain)
            spans.append((min(chain), max(chain)))
            if len(segments) >= MAX_SEGMENTS:
                break
        else:
            banned.update(chain)

    # Stream order is time order.
    segments.sort(key=lambda c: c[0])
    return segments


def extract_path(body, h):
    """Return (points, info). points = [(x, y, z, vx, vy, vz)] in tick order."""
    dt = h["tick_interval"]
    info = {}
    t0 = time.time()

    cands = scan_candidates(body, start_byte=0x300)
    keys = sorted(cands)
    info["candidates"] = len(cands)
    if len(keys) < MIN_CHAIN:
        raise MtvError("only %d coordinate-triple candidates; this body does not "
                       "look like a player delta stream" % len(keys))

    # Frames are roughly evenly spread through the body, so the average
    # bits-per-tick bounds how far apart two consecutive ones can sensibly be.
    avg_bits = (len(body) * 8 / h["ticks"]) if h["ticks"] else 0
    gap_max = max(GAP_MAX_FLOOR, int(avg_bits * 8))

    ref = None
    ts = (h.get("json") or {}).get("trackStats") or {}
    if isinstance(ts.get("maxHorizontalSpeed"), (int, float)):
        ref = float(ts["maxHorizontalSpeed"])
    info["ref_max_horiz"] = ref

    # Typical bits per networked frame, used to tell "the next tick" from "some
    # tick later" when looking for a chain's derivative.
    frame_bits = max(96, int(avg_bits)) if avg_bits else 400
    f32 = make_float_reader(body)

    banned = set()
    chosen = None
    longest = None
    identified = None       # structurally confirmed as the origin stream
    attempts = []
    for _ in range(MAX_IDENTIFY_ROUNDS):
        check_deadline("chain identification")
        chain = longest_smooth_chain(cands, keys, banned, gap_max)
        if len(chain) < MIN_CHAIN:
            break
        pts = [cands[b] for b in chain]
        peak = _peak_horizontal_speed(pts, dt)
        err = abs(peak - ref) if ref is not None else 0.0
        attempts.append((len(chain), peak, err, chain))
        if longest is None or len(chain) > len(longest):
            longest = chain
        if ref is None:
            chosen = chain          # nothing to check against; take the longest
            break
        if err <= max(MATCH_ABS, MATCH_REL * ref):
            chosen = chain
            break
        # The speed check compares one chain's peak against the WHOLE run's max,
        # so it can only pass when that chain covers most of the run. On a map
        # that launches the player around, the path survives as short fragments
        # and no single one of them can ever satisfy it. Remember the first
        # fragment the derivative test confirms is the origin, and fall back to
        # it below -- harvest_segments can rebuild the rest of the run from it.
        if identified is None:
            rate, off, ns = origin_stream_score(f32, cands, chain, dt, frame_bits)
            if _origin_confirmed(rate, ns):
                identified = chain
                info["deriv_rate"] = rate
                info["deriv_offset"] = off
        banned.update(chain)

    info["rounds"] = len(attempts)
    if not attempts:
        raise MtvError("no chain of >=%d linked samples found" % MIN_CHAIN)

    info["confident"] = chosen is not None
    info["identified_by"] = "speed" if chosen is not None else None

    if chosen is None and identified is not None:
        # Not proven by speed, but proven to BE the origin stream: the run
        # carries this chain's own derivative next to it. Take it and let
        # harvesting rebuild the rest; the combined path is re-checked against
        # the recorded max speed below, which can still promote it to confident.
        chosen = identified
        info["identified_by"] = "derivative"

    if chosen is None:
        # The speed check is the only exact test we have, so failing it means we
        # cannot prove which chain is the path. But a single smooth chain that
        # spans most of the run's ticks is not something the other fields in the
        # stream produce, so take the longest, mark it low-confidence, and let
        # the caller decide. Better a flagged path than a silently discarded run.
        best = max(attempts, key=lambda a: a[0])
        cover = (best[0] / h["ticks"]) if h["ticks"] else 0.0
        if cover < LOW_CONFIDENCE_MIN_COVERAGE:
            raise MtvError("no chain reproduced the recorded max speed "
                           "(best %.1f vs %.1f), none was confirmed as the "
                           "origin stream, and the longest covers only %.0f%% "
                           "of ticks" % (best[1], ref, 100 * cover))
        chosen = best[3]
        info["identified_by"] = "coverage"

    # The identified chain proves which data is the origin; it does not span a
    # teleport. Go back for the rest of the run.
    segments = harvest_segments(cands, keys, chosen, gap_max, dt, ref, frame_bits, f32)
    seg_pts = [[cands[b] for b in seg] for seg in segments]

    pts = [p for seg in seg_pts for p in seg]
    info["segments"] = len(segments)
    info["first_segment"] = len(chosen)

    # Per segment, so a teleport step never counts as movement.
    peak = max(_peak_horizontal_speed(s, dt) for s in seg_pts)
    info["chain_max_horiz"] = peak
    info["match_error"] = abs(peak - ref) if ref is not None else 0.0

    # A fragment identified structurally could not be checked against the run's
    # max speed on its own -- it only covers part of the run. The reassembled
    # path can be, and if the fastest moment of the run is now in it, that is
    # the same exact confirmation the clean maps get.
    if (not info["confident"] and ref is not None
            and info["match_error"] <= max(MATCH_ABS, MATCH_REL * ref)):
        info["confident"] = True
        info["identified_by"] = (info.get("identified_by") or "") + "+speed"

    info["samples"] = len(pts)
    info["ticks"] = h["ticks"]
    info["coverage"] = (len(pts) / h["ticks"]) if h["ticks"] else 0.0
    info["path_length"] = sum(
        math.dist(s[i], s[i + 1]) for s in seg_pts for i in range(len(s) - 1))
    info["scan_seconds"] = time.time() - t0

    # A structurally-identified fragment is only worth keeping if reassembly
    # actually produced a route. Writing a 59-point stub covering 11% of a run
    # would put a meaningless stub of a line in the world, which is worse than
    # admitting we could not extract this one.
    if (not info["confident"] and info["identified_by"] == "derivative"
            and info["coverage"] < DERIV_MIN_COVERAGE):
        raise MtvError("origin stream identified but only %.0f%% of ticks could "
                       "be recovered (%d points) -- too fragmented to be a route"
                       % (100 * info["coverage"], len(pts)))

    # Velocity by central difference. The stream does carry velocity floats near
    # the origin, but not at a reliable offset across runs, so we do not use them.
    #
    # Differenced within a segment only: across a teleport the difference is the
    # length of the teleport, which would read as a speed of tens of thousands of
    # units per second and wreck colour-by-speed for the whole run.
    out = []
    for seg in seg_pts:
        n = len(seg)
        for i in range(n):
            a = seg[i - 1] if i > 0 else seg[i]
            b = seg[i + 1] if i + 1 < n else seg[i]
            span = ((1 if i > 0 else 0) + (1 if i + 1 < n else 0)) * dt
            if span <= 0.0:
                vx = vy = vz = 0.0
            else:
                vx = (b[0] - a[0]) / span
                vy = (b[1] - a[1]) / span
                vz = (b[2] - a[2]) / span
            out.append((seg[i][0], seg[i][1], seg[i][2], vx, vy, vz))
    return out, info


# ---------------------------------------------------------------------------
# Split markers
# ---------------------------------------------------------------------------

MARKER_TOL = 0.08


def anchor_markers(points, h, segment_count=1):
    """Place the JSON's split points onto the extracted path.

    The JSON records, for each subsegment, the time it was reached and the
    player's exact velocity at that moment -- but no position. Velocity is a
    3-vector of full-precision floats, which is a distinctive fingerprint, so we
    match on velocity, seeded by time, and require the matched indices to come
    out in order. One shared index offset absorbs the pre-roll before the run
    timer starts (tick count is always larger than run_time / tick_interval).

    Returns (markers, anchored_ok). Wrong markers are worse than no markers, so
    a failed match reports False rather than placing them anyway.
    """
    js = h.get("json") or {}
    segs = js.get("segments") or []
    n = len(points)
    if n < 8 or not segs:
        return [], False

    run_time = h.get("run_time") or 0.0
    wanted = []
    for si, seg in enumerate(segs):
        for sub in (seg.get("subsegments") or []):
            v = sub.get("velocityWhenReached")
            t = sub.get("timeReached")
            if not isinstance(v, list) or len(v) != 3:
                continue
            if not isinstance(t, (int, float)):
                continue
            if v[0] == 0.0 and v[1] == 0.0 and v[2] == 0.0:
                continue        # the start subsegment carries no velocity
            wanted.append((si, int(sub.get("minorNum") or 0), float(t),
                           (float(v[0]), float(v[1]), float(v[2])),
                           float((sub.get("stats") or {}).get("maxOverallSpeed") or 0.0)))
    if not wanted or run_time <= 0.0:
        return [], False

    def err_at(i, tv):
        p = points[i]
        return math.dist((p[3], p[4], p[5]), tv)

    # A stitched path is a concatenation of legs with time gaps between them, so
    # point index is no longer proportional to elapsed time and the time-seeded
    # window below would look in the wrong place. The velocity fingerprint does
    # not depend on that assumption, so search on it alone.
    #
    # This fails safe either way -- a bad window makes the velocity match miss,
    # which reports not-anchored and drops the markers -- but dropping them is a
    # silent loss of the split times, so it is worth searching properly.
    if segment_count > 1:
        markers = []
        last = -1
        ok = True
        for si, minor, t, tv, mspd in wanted:
            speed = math.dist((0.0, 0.0, 0.0), tv) or 1.0
            lo = last + 1
            if lo >= n:
                ok = False
                break
            # Searching forward from the previous match enforces the ordering
            # requirement by construction rather than checking it afterwards.
            idx = min(range(lo, n), key=lambda i: err_at(i, tv))
            if err_at(idx, tv) / speed > MARKER_TOL:
                ok = False
            last = idx
            markers.append((idx, si, minor, t, tv[0], tv[1], tv[2], mspd))
        return markers, ok

    window = max(4, n // 20)
    span = max(8, n // 4)
    stride = max(1, span // 128)

    best_off, best_cost = 0, None
    for off in range(-span, span + 1, stride):
        cost = 0.0
        for _, _, t, tv, _ in wanted:
            g = min(n - 1, max(0, int(n * (t / run_time)) + off))
            lo = max(0, g - window)
            hi = min(n, g + window + 1)
            cost += min(err_at(i, tv) for i in range(lo, hi))
        if best_cost is None or cost < best_cost:
            best_cost, best_off = cost, off

    markers = []
    last = -1
    ok = True
    for si, minor, t, tv, mspd in wanted:
        g = min(n - 1, max(0, int(n * (t / run_time)) + best_off))
        lo = max(0, g - window)
        hi = min(n, g + window + 1)
        idx = min(range(lo, hi), key=lambda i: err_at(i, tv))
        speed = math.dist((0.0, 0.0, 0.0), tv) or 1.0
        if err_at(idx, tv) / speed > MARKER_TOL or idx <= last:
            ok = False
        last = idx
        markers.append((idx, si, minor, t, tv[0], tv[1], tv[2], mspd))
    return markers, ok


# ---------------------------------------------------------------------------
# .wrpath writer
# ---------------------------------------------------------------------------

WRPATH_MAGIC = b"WRPATH\0\0"
WRPATH_VERSION = 1
WRPATH_HEADER = 0x100

FLAG_HAS_VELOCITY = 1 << 0
FLAG_HAS_ANGLES = 1 << 1
FLAG_IS_EYE_PATH = 1 << 2
FLAG_MARKERS_OK = 1 << 3
FLAG_SELF_RECORDED = 1 << 4
FLAG_FROM_EXTRACTOR = 1 << 5
FLAG_LOW_CONFIDENCE = 1 << 6

POINT_FMT = "<7f"           # x y z vx vy vz t                 -> 28 bytes
MARKER_FMT = "<IHHdffffI"   # idx seg minor time vx vy vz spd pad -> 36 bytes
assert struct.calcsize(POINT_FMT) == 28
assert struct.calcsize(MARKER_FMT) == 36


def _fixed(s, size):
    raw = s.encode("utf-8", "replace")[:size - 1]
    return raw + b"\0" * (size - len(raw))


def write_wrpath(out_path, h, points, markers, src_sha1, flags):
    dt = h["tick_interval"]
    head = bytearray(WRPATH_HEADER)
    head[0:8] = WRPATH_MAGIC
    struct.pack_into("<I", head, 0x08, WRPATH_VERSION)
    struct.pack_into("<I", head, 0x0C, flags)
    struct.pack_into("<I", head, 0x10, len(points))
    struct.pack_into("<I", head, 0x14, len(markers))
    struct.pack_into("<f", head, 0x18, dt)
    struct.pack_into("<d", head, 0x1C, h["run_time"])
    struct.pack_into("<Q", head, 0x24, h["steamid64"])
    struct.pack_into("<q", head, 0x2C, h["date_ms"])
    head[0x34:0x74] = _fixed(h["map"], 64)
    head[0x74:0x9C] = _fixed(h["map_hash"], 40)
    head[0x9C:0xC4] = _fixed(src_sha1, 40)
    head[0xC4:0xE4] = _fixed(h["player"], 32)
    struct.pack_into("<f", head, 0xE4, 1.0)     # captureTimescale (n/a offline)
    struct.pack_into("<f", head, 0xE8, 0.0)     # minSampleDist
    struct.pack_into("<f", head, 0xEC, 0.0)     # minSampleAngleDeg
    struct.pack_into("<f", head, 0xF0, 0.0)     # eyeHeightOffset: true origin
    struct.pack_into("<I", head, 0xF4, int(time.time()))
    struct.pack_into("<B", head, 0xF8, h.get("gamemode", 0))
    struct.pack_into("<B", head, 0xF9, h.get("track_type", 0))
    struct.pack_into("<B", head, 0xFA, h.get("track_num", 0))
    # Which extractor wrote this. Bumping EXTRACTOR_REVISION therefore does not
    # only retry the recorded failures -- it also marks everything already
    # written as out of date, so a fix that changes what gets extracted actually
    # reaches the files that were extracted wrongly. Without this the 75
    # surf_colin_blaster_69000 runs produced under the old +-16384 world limit
    # would have been skipped forever as "already done", which is exactly what
    # they looked like: present, and mostly wrong.
    struct.pack_into("<I", head, 0xFC, EXTRACTOR_REVISION)

    body = bytearray()
    for i, (x, y, z, vx, vy, vz) in enumerate(points):
        body += struct.pack(POINT_FMT, x, y, z, vx, vy, vz, i * dt)
    for idx, seg, minor, t, vx, vy, vz, spd in markers:
        body += struct.pack(MARKER_FMT, idx, seg, minor, t, vx, vy, vz, spd, 0)

    blob = bytes(head) + bytes(body)
    blob += struct.pack("<I", zlib.crc32(blob) & 0xFFFFFFFF)

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    tmp = out_path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(blob)
    os.replace(tmp, out_path)
    return len(blob)


# ---------------------------------------------------------------------------
# Demo discovery
# ---------------------------------------------------------------------------

def iter_demos(game_dir):
    root = os.path.join(game_dir, "momentum", "momtv")
    for tree in ("online", "local"):
        base = os.path.join(root, tree)
        if not os.path.isdir(base):
            continue
        for dirpath, _dirs, files in os.walk(base):
            for fn in files:
                if fn.lower().endswith(".mtv"):
                    yield tree, os.path.join(dirpath, fn)


def peek_map(path):
    """Read just enough to get the map name, for cheap filtering."""
    try:
        with open(path, "rb") as f:
            head = f.read(0x50)
        if head[:4] != MMTV_MAGIC:
            return None
        return _cstr(head, OFF_MAPNAME, 64)
    except OSError:
        return None


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def fmt_time(t):
    if t is None:
        return "-"
    m = int(t // 60)
    return ("%d:%06.3f" % (m, t - m * 60)) if m else ("%.3f" % t)


def cmd_list(args):
    by_map = {}
    bad = 0
    for tree, path in iter_demos(args.game):
        try:
            with open(path, "rb") as f:
                h = parse_mtv_header(f.read(8192), path)
        except (MtvError, OSError):
            bad += 1
            continue
        h["tree"] = tree
        by_map.setdefault(h["map"], []).append(h)

    names = sorted(by_map, key=lambda m: -len(by_map[m]))
    if args.map:
        names = [m for m in names if args.map.lower() in m.lower()]
    total = sum(len(v) for v in by_map.values())
    print("%d demos across %d maps (%d unreadable)" % (total, len(by_map), bad))
    for m in names[:(args.limit or 40)]:
        runs = sorted(by_map[m], key=lambda r: r["run_time"])
        print("\n%-34s %d runs   best %s by %s"
              % (m, len(runs), fmt_time(runs[0]["run_time"]), runs[0]["player"]))
        for h in runs[:8]:
            print("    %-9s %-20s %-6s %-6s %s"
                  % (fmt_time(h["run_time"]), h["player"],
                     GAMEMODE_NAMES.get(h["gamemode"], "?"),
                     TRACKTYPE_NAMES.get(h["track_type"], "?"), h["tree"]))
    return 0


def process_one(path, args):
    name = os.path.basename(path)
    sha1 = os.path.splitext(name)[0]
    set_deadline(getattr(args, "timeout", 0))
    try:
        data = open(path, "rb").read()
        h = parse_mtv_header(data, path)
    except (MtvError, OSError) as e:
        return ("error", name, str(e), None)

    if h["codec"] == "zstd" and _zstd is None:
        return ("skip", name, "zstd body (pip install zstandard)", h)

    try:
        body = decompress_body(data, h)
        pts, info = extract_path(body, h)
    except (MtvError, OSError, lzma.LZMAError) as e:
        return ("error", name, str(e), h)

    markers, mok = anchor_markers(pts, h, info.get("segments", 1))
    flags = FLAG_HAS_VELOCITY | FLAG_FROM_EXTRACTOR
    if mok:
        flags |= FLAG_MARKERS_OK
    if not info.get("confident"):
        flags |= FLAG_LOW_CONFIDENCE
        info["why_flagged"] = "speed check"
    # Passing the speed oracle only proves the leg we identified is real. It says
    # nothing about whether we found the whole run, and that is exactly how two
    # main-track runs got written at 8.8% and 3.9% coverage with no warning.
    if info.get("coverage", 0.0) < MIN_COVERAGE_CONFIDENT:
        flags |= FLAG_LOW_CONFIDENCE
        info.setdefault("why_flagged", "coverage")
    info["flagged"] = bool(flags & FLAG_LOW_CONFIDENCE)

    info["markers"] = len(markers)
    info["markers_ok"] = mok
    if not args.verify:
        out = os.path.join(args.out, h["map"], sha1 + ".wrpath")
        info["bytes"] = write_wrpath(out, h, pts, markers if mok else [], sha1, flags)
    return ("ok", name, "", (h, info))


def wrpath_for(out_dir, demo_path, map_name):
    """Where process_one would write this demo's output.

    Cheap on purpose: process_one names the file after the source basename (the
    variable is called sha1 but it is os.path.splitext(name)[0]), so the answer
    needs only the map name, which peek_map reads from the first 0x50 bytes.
    That is what makes --skip-existing worth having -- it skips before the full
    read and the decompression, not after.
    """
    base = os.path.splitext(os.path.basename(demo_path))[0]
    return os.path.join(out_dir, map_name, base + ".wrpath")


def failures_path(out_dir, map_name):
    return os.path.join(out_dir, map_name, FAILURES_FILE)


def load_failures(out_dir, map_name):
    """basename -> (size, reason), for demos that already failed once.

    Records written by a different EXTRACTOR_REVISION are ignored rather than
    deleted: they cost nothing to leave in place and the next write rewrites the
    file anyway.
    """
    out = {}
    try:
        with open(failures_path(out_dir, map_name), "r",
                  encoding="utf-8", errors="replace") as f:
            for line in f:
                if not line.strip() or line.startswith("#"):
                    continue
                parts = line.rstrip("\n").split("\t")
                if len(parts) < 4:
                    continue
                try:
                    rev, size = int(parts[0]), int(parts[1])
                except ValueError:
                    continue
                if rev == EXTRACTOR_REVISION:
                    out[parts[2]] = (size, parts[3])
    except OSError:
        pass
    return out


def save_failures(out_dir, map_name, records):
    path = failures_path(out_dir, map_name)
    if not records:
        try:
            os.remove(path)
        except OSError:
            pass
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write("# WrLines: demos on this map that could not be extracted.\n")
        f.write("# Re-running the extractor skips these, because deriving the\n")
        f.write("# same failure again costs the same minutes it cost the first\n")
        f.write("# time. Pass --retry-failed to try them anyway; delete this\n")
        f.write("# file to forget them entirely.\n")
        f.write("# extractor-revision <TAB> bytes <TAB> demo <TAB> why\n")
        for base in sorted(records):
            size, why = records[base]
            why = why.replace("\t", " ").replace("\n", " ")
            f.write("%d\t%d\t%s\t%s\n" % (EXTRACTOR_REVISION, size, base, why))
    os.replace(tmp, path)


def _size_of(path):
    try:
        return os.path.getsize(path)
    except OSError:
        return -1


def _job_count(requested):
    """How many worker processes to use. 0 means "decide for me"."""
    if requested and requested > 0:
        return int(requested)
    cpus = os.cpu_count() or 2
    # Two cores held back on purpose. The in-game button starts this at
    # below-normal priority precisely so it does not fight the game for CPU, and
    # child processes inherit that priority class -- but priority does not help
    # if every core is occupied.
    return max(1, cpus - 2)


def _process_one_job(item):
    """Worker entry point. Must be module level so it can be pickled."""
    path, demo_map, args = item
    kind, name, msg, extra = process_one(path, args)
    return (path, demo_map, kind, name, msg, extra)


def _run_all(targets, args):
    """Yield a result per target, in whatever order they finish.

    Each demo is completely independent -- separate file in, separate file out --
    so this is about as parallel as work gets. It matters because the cost per
    demo is wildly uneven: 0.7 s on a normal map, up to a minute on a bad one,
    which serially reads as "it did four quickly and then stopped".
    """
    jobs = _job_count(getattr(args, "jobs", 0))
    if jobs <= 1 or len(targets) < 2:
        for path, demo_map in targets:
            kind, name, msg, extra = process_one(path, args)
            yield (path, demo_map, kind, name, msg, extra)
        return

    try:
        from concurrent.futures import ProcessPoolExecutor
    except ImportError:
        jobs = 1

    if jobs <= 1:
        for path, demo_map in targets:
            kind, name, msg, extra = process_one(path, args)
            yield (path, demo_map, kind, name, msg, extra)
        return

    print("%d worker%s" % (jobs, "" if jobs == 1 else "s"))
    items = [(path, demo_map, args) for path, demo_map in targets]
    with ProcessPoolExecutor(max_workers=jobs) as pool:
        for result in pool.map(_process_one_job, items, chunksize=1):
            yield result


def wrpath_revision(path):
    """Which extractor revision wrote this .wrpath, or -1 if unreadable.

    Files written before this field existed read as 0, which is never equal to a
    real revision, so they are correctly treated as out of date.
    """
    try:
        with open(path, "rb") as f:
            head = f.read(WRPATH_HEADER)
        if len(head) < 0x100:
            return -1
        return struct.unpack_from("<I", head, 0xFC)[0]
    except OSError:
        return -1


def cmd_extract(args):
    already = 0
    known_bad = 0
    stale = 0
    if args.file:
        targets = [(args.file, peek_map(args.file) or "")]
    else:
        targets = []
        failures_by_map = {}
        for _tree, path in iter_demos(args.game):
            m = peek_map(path) or ""
            if args.map and m.lower() != args.map.lower():
                continue
            stale_out = False
            if args.skip_existing and m:
                out = wrpath_for(args.out, path, m)
                if os.path.exists(out):
                    if wrpath_revision(out) == EXTRACTOR_REVISION:
                        already += 1
                        continue
                    stale += 1      # written by an older extractor; redo it
                    stale_out = True
            # Demos that failed before, at this revision, with this exact file
            # size. A re-download that changed the file is not the same demo and
            # gets another go.
            #
            # Not applied when there is an out-of-date output on disk. That
            # combination is real -- a demo can have succeeded under the old
            # extractor and fail under the new one -- and skipping it would leave
            # the old file in place forever, still loaded, still drawn, derived
            # from an assumption we have since established was wrong.
            if args.skip_existing and m and not args.retry_failed and not stale_out:
                if m not in failures_by_map:
                    failures_by_map[m] = load_failures(args.out, m)
                rec = failures_by_map[m].get(
                    os.path.splitext(os.path.basename(path))[0])
                if rec and rec[0] == _size_of(path):
                    known_bad += 1
                    continue
            targets.append((path, m))
    if already:
        print("%d already extracted, skipping them" % already)
    if stale:
        print("%d were extracted by an older version and are being redone" % stale)
    if known_bad:
        print("%d failed before and are being skipped (--retry-failed to try "
              "them again)" % known_bad)

    # What we learn this run, per map, so the record can be updated in place:
    # newly failed demos are added, and any that succeed this time are dropped.
    seen_maps = set()
    now_failed = {}
    now_ok = {}

    if args.limit:
        targets = targets[:args.limit]

    done = ok = skipped = failed = lowconf = 0
    removed = []
    cov = []
    t0 = time.time()
    total = len(targets)
    for path, demo_map, kind, name, msg, extra in _run_all(targets, args):
        done += 1
        base = os.path.splitext(name)[0]
        if demo_map:
            seen_maps.add(demo_map)
            if kind == "error":
                now_failed.setdefault(demo_map, {})[base] = (_size_of(path), msg)
            elif kind == "ok":
                now_ok.setdefault(demo_map, set()).add(base)
        # A running count, because the panel shows this live and "4 of 66" is the
        # difference between "working" and "stuck".
        pre = "[%d/%d]" % (done, total)
        if kind == "error":
            failed += 1
            # An older extractor may have left an output for this demo. We have
            # just established the current one cannot produce it, so that file is
            # a path derived from an assumption we no longer trust -- remove it
            # rather than go on drawing it. Current-revision files are never
            # touched.
            if not args.verify and demo_map:
                out = wrpath_for(args.out, path, demo_map)
                try:
                    if (os.path.exists(out)
                            and wrpath_revision(out) != EXTRACTOR_REVISION):
                        os.remove(out)
                        removed.append(base)
                except OSError:
                    pass
            print("%s FAIL %-44s %s" % (pre, name[:44], msg))
        elif kind == "skip":
            skipped += 1
            print("%s SKIP %-44s %s" % (pre, name[:44], msg))
        else:
            h, info = extra
            ok += 1
            cov.append(info["coverage"])
            # Count what actually got written to the file, not just the speed
            # oracle's verdict -- low coverage flags a run too, and reporting
            # those as clean is how two 4%-coverage runs went unnoticed.
            if info.get("flagged"):
                lowconf += 1
            mk = ("%d%s" % (info["markers"], "" if info["markers_ok"] else "!")) \
                if info["markers"] else "-"
            print("%s %-4s %-44s %-9s %5d pts %5.1f%%  err %7.4f  mk %-4s %.1fs"
                  % (pre, "OK" if not info.get("flagged") else "OK?",
                     name[:44], fmt_time(h["run_time"]), info["samples"],
                     100 * info["coverage"], info["match_error"], mk,
                     info["scan_seconds"]))

    print("\n%d processed in %.1fs: %d ok (%d low-confidence), %d skipped, %d failed"
          % (done, time.time() - t0, ok, lowconf, skipped, failed))
    if cov:
        cov.sort()
        print("coverage: median %.1f%%  min %.1f%%  max %.1f%%"
              % (100 * cov[len(cov) // 2], 100 * cov[0], 100 * cov[-1]))
    if not args.verify and ok:
        print("wrote %d .wrpath files under %s" % (ok, args.out))
    if removed:
        print("removed %d out-of-date .wrpath file%s whose demo can no longer be "
              "extracted" % (len(removed), "" if len(removed) == 1 else "s"))

    # --verify writes nothing, and that has to include this.
    if not args.verify:
        recorded = 0
        for m in seen_maps:
            recs = load_failures(args.out, m)
            recs.update(now_failed.get(m, {}))
            for base in now_ok.get(m, ()):        # rescued: forget the failure
                recs.pop(base, None)
            save_failures(args.out, m, recs)
            recorded += len(now_failed.get(m, {}))
        if recorded:
            print("recorded %d failure%s so re-running this map skips them"
                  % (recorded, "" if recorded == 1 else "s"))
    return 0 if failed == 0 else 1


def main(argv):
    ap = argparse.ArgumentParser(
        description="Extract run paths from Momentum Mod .mtv demos.")
    ap.add_argument("--game", default=DEFAULT_GAME, help="game install directory")
    ap.add_argument("--out", default=DEFAULT_OUT, help="output dir for .wrpath files")
    ap.add_argument("--map", help="only this map")
    ap.add_argument("--file", help="a single .mtv file")
    ap.add_argument("--all", action="store_true", help="process every demo")
    ap.add_argument("--list", action="store_true", help="list indexed demos and exit")
    ap.add_argument("--verify", action="store_true",
                    help="extract and report, but write nothing")
    ap.add_argument("--limit", type=int, default=0, help="stop after N demos")
    ap.add_argument("--skip-existing", action="store_true",
                    help="skip demos that already have a .wrpath, and demos "
                         "recorded as having failed before; makes re-running a "
                         "map cost seconds instead of minutes")
    ap.add_argument("--retry-failed", action="store_true",
                    help="with --skip-existing, try the recorded failures "
                         "again anyway")
    ap.add_argument("--jobs", type=int, default=0,
                    help="worker processes; 0 (default) uses all cores but two, "
                         "1 runs serially")
    ap.add_argument("--timeout", type=float, default=180.0,
                    help="give up on a single demo after this many seconds "
                         "(0 for no limit). It is recorded as an ordinary "
                         "failure, so it is not paid for twice")
    args = ap.parse_args(argv)

    if not os.path.isdir(args.game):
        print("[!] game directory not found: %s" % args.game)
        return 1
    if args.list:
        return cmd_list(args)
    if not (args.map or args.file or args.all):
        ap.print_help()
        print("\n[!] pick --map NAME, --file PATH, or --all")
        return 1
    return cmd_extract(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
