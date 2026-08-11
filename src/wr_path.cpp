// wr_path.cpp  --  see wr_path.h.

#include "wr_path.h"
#include "wr_dp.h"
#include "wr_extract.h"
#include "wr_profile.h"
#include "wr_stress.h"
#include "wr_energy.h"
#include "wr_engine.h"
#include "wr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// The format, in three numbers. WrPathWrite below and LoadOne further down are
// the only two things that know them, and they are the only two statements of
// this format that exist.
#define WRPATH_HEADER_BYTES 0x100
#define WRPATH_POINT_BYTES 28
#define WRPATH_MARKER_BYTES 36

static WrRun g_runs[WR_MAX_RUNS];
static int g_runCount = 0;
static char g_loadedMap[72] = {0};

static WrPoint g_live[WR_LIVE_POINTS];
static int g_liveCount = 0;
static bool g_liveOn = true;

// Recording paused with the buffer intact, so a failed attempt survives to be
// looked at. Set and cleared from wr_timer.cpp, which is the one place that
// knows about start zones -- this file is deliberately linked without
// wr_start.cpp by the rank harness.
static bool g_liveHold = false;

// Set when a load finishes, cleared by the first WrUpdateNearest with a live
// camera, which is where the default run selection is actually made.
static bool g_autoEnablePending = false;

// See WrRunStoreGeneration in the header. Bumped on a map change and again
// when the last file of a load is in, which are the two moments the store
// stops meaning what it meant.
static unsigned int g_generation = 1;

// What counts as "on the leg I am standing in" for that default. Matches the
// panel's own default radius: enough to cover one stage of a surf map without
// reaching into the next one.
#define AUTO_ENABLE_RADIUS 4096.0f

// A distinct, readable palette. Gold first so the best run reads as the best
// run without needing a legend.
static const unsigned int kPalette[] = {
    0xFF33CCFF,   // gold      (ABGR)
    0xFFFFCC44,   // cyan
    0xFF66FF66,   // green
    0xFF6666FF,   // red
    0xFFFF66FF,   // magenta
    0xFF44DDFF,   // orange
    0xFFDDDD66,   // teal
    0xFFCCCCCC,   // grey
};

static unsigned int PaletteColour(int i)
{
    return kPalette[i % (int)(sizeof(kPalette) / sizeof(kPalette[0]))];
}

// ---------------------------------------------------------------------------
// CRC32 (zlib polynomial), to match what the Python writer stamps on.
// ---------------------------------------------------------------------------

static unsigned int Crc32(const unsigned char *data, size_t len)
{
    static unsigned int table[256];
    static bool built = false;
    if (!built)
    {
        for (unsigned int i = 0; i < 256; i++)
        {
            unsigned int c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    unsigned int c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// Writing one
// ---------------------------------------------------------------------------
//
// The reader is LoadOne, a couple of hundred lines below, and the offsets both
// of them use are the three defines at the top of this file. That adjacency is
// the whole reason the writer is here rather than in the extractor: the
// reference implementation used to be the second statement of this format and
// it has stopped shipping, so these two functions are now the only two, and two
// halves of a format in two files drift.

// The reference's _fixed(): encode to UTF-8, cut to size-1 bytes, NUL-pad.
//
// NOT strncpy, and the difference is not academic. The reference reads these
// fields out of the demo with str.decode("utf-8", "replace") and writes them
// back with str.encode("utf-8", "replace"), so any byte that is not valid UTF-8
// has become U+FFFD -- three bytes, EF BF BD -- by the time it reaches the
// file. That is reachable on real data: the player-name field in a .mtv is 32
// bytes and the game truncates into it, so a name whose last character is a
// multi-byte one arrives here cut in half. Copying the raw bytes would produce
// a file that differs from the reference's in exactly those demos and nowhere
// else, which is the worst possible shape for a difference to have.
//
// The truncation is applied to the ENCODED bytes and can therefore cut a
// sequence in half itself. That is what the reference's slice does, so it is
// what this does; the field is a label, not a string anybody parses.
//
// The error handling is CPython's, not "one U+FFFD per bad byte": an invalid
// start byte consumes one, a bad continuation consumes as many as were valid,
// and a sequence truncated by the end of the field consumes the whole
// remainder. Emitting per byte would differ on precisely the truncated-emoji
// case this exists for.

static inline bool Utf8Cont(unsigned char c) { return (c & 0xC0) == 0x80; }

// The length of the valid sequence at s, or minus the number of bytes CPython's
// decoder would consume before emitting one replacement character.
static int Utf8SeqLen(const unsigned char *s, size_t avail)
{
    const unsigned char c = s[0];
    if (c < 0x80)
        return 1;
    if (c < 0xC2)
        return -1;                      // continuation byte, or a fake 0000-007F
    if (c < 0xE0)
    {
        if (avail < 2)
            return -(int)avail;         // unexpected end of data
        if (!Utf8Cont(s[1]))
            return -1;
        return 2;
    }
    if (c < 0xF0)
    {
        if (avail < 3)
        {
            if (avail < 2)
                return -1;
            if (!Utf8Cont(s[1]) || (s[1] < 0xA0 ? c == 0xE0 : c == 0xED))
                return -1;
            return -2;
        }
        if (!Utf8Cont(s[1]))
            return -1;
        if (c == 0xE0 && s[1] < 0xA0)
            return -1;                  // overlong
        if (c == 0xED && s[1] >= 0xA0)
            return -1;                  // a surrogate, which is not valid UTF-8
        if (!Utf8Cont(s[2]))
            return -2;
        return 3;
    }
    if (c < 0xF5)
    {
        if (avail < 4)
        {
            if (avail < 2)
                return -1;
            if (!Utf8Cont(s[1]) || (s[1] < 0x90 ? c == 0xF0 : c == 0xF4))
                return -1;
            if (avail < 3)
                return -2;
            if (!Utf8Cont(s[2]))
                return -2;
            return -3;
        }
        if (!Utf8Cont(s[1]))
            return -1;
        if (c == 0xF0 && s[1] < 0x90)
            return -1;                  // overlong
        if (c == 0xF4 && s[1] >= 0x90)
            return -1;                  // past U+10FFFF
        if (!Utf8Cont(s[2]))
            return -2;
        if (!Utf8Cont(s[3]))
            return -3;
        return 4;
    }
    return -1;                          // F5-FF: no valid sequence starts here
}

void WrPathFixedField(unsigned char *dst, int size, const char *src)
{
    memset(dst, 0, (size_t)size);
    if (!src || size < 1)
        return;

    static const unsigned char kReplacement[3] = {0xEF, 0xBF, 0xBD};
    const unsigned char *s = (const unsigned char *)src;
    const size_t n = strlen(src);
    const int cap = size - 1;           // always at least one NUL
    size_t i = 0;
    int w = 0;

    // Stopping at `cap` rather than encoding it all and slicing gives the same
    // bytes: the encoding of a prefix is a prefix of the encoding.
    while (i < n && w < cap)
    {
        int seq = Utf8SeqLen(s + i, n - i);
        if (seq > 0)
        {
            for (int k = 0; k < seq && w < cap; k++)
                dst[w++] = s[i + (size_t)k];
            i += (size_t)seq;
        }
        else
        {
            for (int k = 0; k < 3 && w < cap; k++)
                dst[w++] = kReplacement[k];
            i += (size_t)(-seq);
        }
    }
}

static void PutU32(unsigned char *p, unsigned int v) { memcpy(p, &v, 4); }
static void PutF32(unsigned char *p, float v) { memcpy(p, &v, 4); }
static void PutF64(unsigned char *p, double v) { memcpy(p, &v, 8); }

long long WrPathWrite(const WrPathWriteArgs *a, char *err, int errCap)
{
    const size_t bodyBytes = (size_t)a->pointCount * WRPATH_POINT_BYTES
                           + (size_t)a->markerCount * WRPATH_MARKER_BYTES;
    const size_t total = (size_t)WRPATH_HEADER_BYTES + bodyBytes + 4;

    // calloc, because the header is mostly gaps -- 0x20..0x23 between the run
    // time and the SteamID, and 0xFB -- and those gaps are covered by the CRC.
    unsigned char *blob = (unsigned char *)calloc(total, 1);
    if (!blob)
    {
        _snprintf_s(err, (size_t)errCap, _TRUNCATE,
                    "out of memory writing %d points", a->pointCount);
        return -1;
    }

    memcpy(blob, "WRPATH\0\0", 8);
    PutU32(blob + 0x08, 1);                     // WRPATH_VERSION
    PutU32(blob + 0x0C, a->flags);
    PutU32(blob + 0x10, (unsigned int)a->pointCount);
    PutU32(blob + 0x14, (unsigned int)a->markerCount);
    PutF32(blob + 0x18, a->tickInterval);
    PutF64(blob + 0x1C, a->runTime);
    memcpy(blob + 0x24, &a->steamid64, 8);
    memcpy(blob + 0x2C, &a->dateMs, 8);
    WrPathFixedField(blob + 0x34, 64, a->map);
    WrPathFixedField(blob + 0x74, 40, a->mapHash);
    WrPathFixedField(blob + 0x9C, 40, a->srcSha1);
    WrPathFixedField(blob + 0xC4, 32, a->player);
    PutF32(blob + 0xE4, 1.0f);                  // captureTimescale (n/a offline)

    // 0xE8 and 0xEC were minSampleDist and minSampleAngleDeg: written as 0.0
    // since the format existed and never read by anything. Zero across all 1735
    // .wrpath files on the development machine, which is what makes claiming
    // them free -- an older file reads 0 here, and 0 already means "no start
    // recorded". So this needs no version bump and no branch in LoadOne, whose
    // version check is a hard reject with no migration path.
    PutU32(blob + 0xE8, a->startOk ? a->startIndex : 0u);
    PutU32(blob + 0xEC, a->startOk ? 1u : 0u);  // START_FLAG_FOUND
    PutF32(blob + 0xF0, 0.0f);                  // eyeHeightOffset: true origin
    PutU32(blob + 0xF4, (unsigned int)WrNowEpoch());
    blob[0xF8] = a->gamemode;
    blob[0xF9] = a->trackType;
    blob[0xFA] = a->trackNum;
    PutU32(blob + 0xFC, WR_EXTRACTOR_REVISION);

    unsigned char *p = blob + WRPATH_HEADER_BYTES;
    const double dt = (double)a->tickInterval;
    for (int i = 0; i < a->pointCount; i++)
    {
        const WrDpPoint *pt = &a->points[i];
        // t is index * tick_interval in DOUBLE and then rounded once, on the
        // way into the file. Accumulating a float would drift.
        float v[7] = {
            (float)pt->x, (float)pt->y, (float)pt->z,
            (float)pt->vx, (float)pt->vy, (float)pt->vz,
            (float)((double)i * dt)
        };
        memcpy(p, v, sizeof(v));
        p += WRPATH_POINT_BYTES;
    }
    for (int i = 0; i < a->markerCount; i++)
    {
        const WrPathWriteMarker *m = &a->markers[i];
        memcpy(p + 0, &m->pointIndex, 4);
        memcpy(p + 4, &m->segment, 2);
        memcpy(p + 6, &m->minorNum, 2);
        memcpy(p + 8, &m->timeReached, 8);
        float mv[4] = {m->vx, m->vy, m->vz, m->maxSpeed};
        memcpy(p + 16, mv, sizeof(mv));
        // p + 32 is the four pad bytes, already zero from the calloc.
        p += WRPATH_MARKER_BYTES;
    }

    PutU32(blob + total - 4, Crc32(blob, total - 4));

    char tmp[MAX_PATH];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s.tmp", a->outPath);

    char dir[MAX_PATH];
    strcpy_s(dir, sizeof(dir), a->outPath);
    char *slash = strrchr(dir, '\\');
    if (slash)
    {
        *slash = '\0';
        WrMakeTree(dir);
    }

    FILE *f = NULL;
    if (fopen_s(&f, tmp, "wb") != 0 || !f)
    {
        _snprintf_s(err, (size_t)errCap, _TRUNCATE, "cannot write %s", tmp);
        free(blob);
        return -1;
    }
    const size_t put = fwrite(blob, 1, total, f);
    fclose(f);
    free(blob);
    if (put != total)
    {
        DeleteFileA(tmp);
        _snprintf_s(err, (size_t)errCap, _TRUNCATE,
                    "short write to %s (%zu of %zu bytes)", tmp, put, total);
        return -1;
    }

    // os.replace: atomic, so a reader never sees half a file and a Stop never
    // leaves one behind.
    if (!MoveFileExA(tmp, a->outPath, MOVEFILE_REPLACE_EXISTING))
    {
        DWORD e = GetLastError();
        DeleteFileA(tmp);
        _snprintf_s(err, (size_t)errCap, _TRUNCATE,
                    "cannot replace %s (error %lu)", a->outPath, e);
        return -1;
    }
    return (long long)total;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

static void FreeRuns(void)
{
    for (int i = 0; i < g_runCount; i++)
    {
        if (g_runs[i].points)
            free(g_runs[i].points);
        if (g_runs[i].breaks)
            free(g_runs[i].breaks);
        if (g_runs[i].dips)
            free(g_runs[i].dips);
        if (g_runs[i].peaks)
            free(g_runs[i].peaks);
        if (g_runs[i].eff)
            free(g_runs[i].eff);
        if (g_runs[i].chunks)
            free(g_runs[i].chunks);
        WrProfileFree(&g_runs[i]);
    }
    memset(g_runs, 0, sizeof(g_runs));
    g_runCount = 0;
}

static void ReadFixed(char *dst, int dstLen, const unsigned char *src, int srcLen)
{
    int n = srcLen < dstLen - 1 ? srcLen : dstLen - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    // Fixed-width fields are NUL-padded; make sure we stop at the first one.
    for (int i = 0; i < n; i++)
        if (dst[i] == '\0') { dst[i] = '\0'; break; }
}

// Bottoms of ramps: where the path stops falling and starts climbing.
//
// Detected on the stored velocity's z sign change rather than on position, so a
// single noisy point cannot invent one. The hysteresis is what keeps this
// useful: a surf line spends a lot of time near level, and without requiring a
// real descent followed by a real climb every one of those wobbles would get a
// label until the line vanished under text.
static void FindDips(WrRun *run)
{
    run->dips = NULL;
    run->dipCount = 0;
    if (run->pointCount < 8)
        return;

    // Two passes: count, then fill. Dips are rare (tens per run) so allocating
    // for the worst case would waste far more than the second walk costs.
    for (int pass = 0; pass < 2; pass++)
    {
        int n = 0;
        int last = -WR_DIP_MIN_GAP;
        float peakZ = run->points[0].pos.z;      // highest point since climbing
        float lowZ = run->points[0].pos.z;       // lowest since we started down
        bool falling = false;

        for (int i = 1; i < run->pointCount; i++)
        {
            float z = run->points[i].pos.z;
            float vz = run->points[i].vel.z;

            if (vz < 0.0f)
            {
                if (!falling)
                {
                    falling = true;
                    lowZ = z;
                }
                if (z < lowZ)
                    lowZ = z;
                if (z > peakZ)
                    peakZ = z;
            }
            else if (falling)
            {
                // Turned upward. Was the descent deep enough, and are we far
                // enough from the previous dip to be a separate feature?
                if ((peakZ - lowZ) >= WR_DIP_MIN_DROP && (i - last) >= WR_DIP_MIN_GAP)
                {
                    if (pass == 1 && run->dips)
                        run->dips[n] = i;
                    n++;
                    last = i;
                }
                falling = false;
                peakZ = z;
            }
            else if (z > peakZ)
            {
                peakZ = z;
            }
        }

        if (pass == 0)
        {
            if (n == 0)
                return;
            run->dips = (int *)malloc(sizeof(int) * n);
            if (!run->dips)
                return;
        }
        else
        {
            run->dipCount = n;
        }
    }
}

// Tops: where the path stops climbing and starts falling. An exact mirror of
// FindDips, down to the hysteresis, so a top and a bottom are found by the same
// rule and a run cannot have plausible bottoms and implausible tops.
//
// Note which climb the threshold applies to. A dip needs a real DESCENT before
// it; a peak needs a real CLIMB. Reusing WR_DIP_MIN_DROP for both is deliberate
// -- it is a "this is a feature, not a wobble" height and the number that keeps
// a flat section from vanishing under labels does not care about direction.
static void FindPeaks(WrRun *run)
{
    run->peaks = NULL;
    run->peakCount = 0;
    if (run->pointCount < 8)
        return;

    for (int pass = 0; pass < 2; pass++)
    {
        int n = 0;
        int last = -WR_DIP_MIN_GAP;
        float lowZ = run->points[0].pos.z;       // lowest point since falling
        float peakZ = run->points[0].pos.z;      // highest since we started up
        bool rising = false;

        for (int i = 1; i < run->pointCount; i++)
        {
            float z = run->points[i].pos.z;
            float vz = run->points[i].vel.z;

            if (vz > 0.0f)
            {
                if (!rising)
                {
                    rising = true;
                    peakZ = z;
                }
                if (z > peakZ)
                    peakZ = z;
                if (z < lowZ)
                    lowZ = z;
            }
            else if (rising)
            {
                if ((peakZ - lowZ) >= WR_DIP_MIN_DROP && (i - last) >= WR_DIP_MIN_GAP)
                {
                    if (pass == 1 && run->peaks)
                        run->peaks[n] = i;
                    n++;
                    last = i;
                }
                rising = false;
                lowZ = z;
            }
            else if (z < lowZ)
            {
                lowZ = z;
            }
        }

        if (pass == 0)
        {
            if (n == 0)
                return;
            run->peaks = (int *)malloc(sizeof(int) * n);
            if (!run->peaks)
                return;
        }
        else
        {
            run->peakCount = n;
        }
    }
}

// Bounding spheres, for aiming at a line. See WrChunk in the header for why the
// path is bounded in blocks rather than walked.
//
// Centre-of-AABB rather than a true minimal sphere: a real Welzl solve on 38 751
// points would be a lot of code to shave a few per cent off a radius that is
// only ever used to say "not this one". Over-large is the safe direction --
// it costs a chunk that gets rejected at the next stage, never a missed line.
static void BuildBounds(WrRun *run)
{
    run->chunks = NULL;
    run->chunkCount = 0;
    run->boundRadius = 0.0f;
    if (run->pointCount < 2)
        return;

    Vec3 lo = run->points[0].pos, hi = lo;
    for (int i = 1; i < run->pointCount; i++)
    {
        const Vec3 &p = run->points[i].pos;
        if (p.x < lo.x) lo.x = p.x;  if (p.x > hi.x) hi.x = p.x;
        if (p.y < lo.y) lo.y = p.y;  if (p.y > hi.y) hi.y = p.y;
        if (p.z < lo.z) lo.z = p.z;  if (p.z > hi.z) hi.z = p.z;
    }
    run->boundCentre = WrScale(WrAdd(lo, hi), 0.5f);
    float r = 0.0f;
    for (int i = 0; i < run->pointCount; i++)
    {
        float d = WrDistSqr(run->points[i].pos, run->boundCentre);
        if (d > r) r = d;
    }
    run->boundRadius = sqrtf(r);

    int n = (run->pointCount + WR_PICK_CHUNK - 1) / WR_PICK_CHUNK;
    run->chunks = (WrChunk *)malloc(sizeof(WrChunk) * (size_t)n);
    if (!run->chunks)
        return;                 // the whole-run sphere still works without them
    run->chunkCount = n;

    for (int k = 0; k < n; k++)
    {
        int a = k * WR_PICK_CHUNK;
        int b = a + WR_PICK_CHUNK;
        if (b > run->pointCount)
            b = run->pointCount;
        Vec3 clo = run->points[a].pos, chi = clo;
        for (int i = a + 1; i < b; i++)
        {
            const Vec3 &p = run->points[i].pos;
            if (p.x < clo.x) clo.x = p.x;  if (p.x > chi.x) chi.x = p.x;
            if (p.y < clo.y) clo.y = p.y;  if (p.y > chi.y) chi.y = p.y;
            if (p.z < clo.z) clo.z = p.z;  if (p.z > chi.z) chi.z = p.z;
        }
        run->chunks[k].c = WrScale(WrAdd(clo, chi), 0.5f);
        float cr = 0.0f;
        for (int i = a; i < b; i++)
        {
            float d = WrDistSqr(run->points[i].pos, run->chunks[k].c);
            if (d > cr) cr = d;
        }
        run->chunks[k].r = sqrtf(cr);
    }
}

// Defined below, next to the rest of the per-run analysis.
static void FindStart(WrRun *run, int stored, bool storedUsable);
static void CheckTimes(WrRun *run);
static void FindEfficiency(WrRun *run);

static bool LoadOne(const char *path, WrRun *run)
{
    FILE *f = NULL;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < WRPATH_HEADER_BYTES + 4)
    {
        fclose(f);
        return false;
    }

    unsigned char *buf = (unsigned char *)malloc((size_t)size);
    if (!buf)
    {
        fclose(f);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size)
    {
        free(buf);
        return false;
    }

    if (memcmp(buf, "WRPATH\0\0", 8) != 0)
    {
        WrLogf("[!] %s: bad magic", path);
        free(buf);
        return false;
    }

    unsigned int stored;
    memcpy(&stored, buf + size - 4, 4);
    if (Crc32(buf, (size_t)size - 4) != stored)
    {
        WrLogf("[!] %s: CRC mismatch -- truncated or corrupt, skipping", path);
        free(buf);
        return false;
    }

    unsigned int version, flags, nPts, nMks;
    memcpy(&version, buf + 0x08, 4);
    memcpy(&flags, buf + 0x0C, 4);
    memcpy(&nPts, buf + 0x10, 4);
    memcpy(&nMks, buf + 0x14, 4);
    if (version != 1)
    {
        WrLogf("[!] %s: unsupported version %u", path, version);
        free(buf);
        return false;
    }

    size_t need = (size_t)WRPATH_HEADER_BYTES + (size_t)nPts * WRPATH_POINT_BYTES
                + (size_t)nMks * WRPATH_MARKER_BYTES + 4;
    if (nPts < 2 || need > (size_t)size)
    {
        WrLogf("[!] %s: point/marker counts do not fit the file", path);
        free(buf);
        return false;
    }

    memset(run, 0, sizeof(*run));
    run->flags = flags;
    memcpy(&run->tickInterval, buf + 0x18, 4);
    memcpy(&run->runTime, buf + 0x1C, 8);
    memcpy(&run->steamId, buf + 0x24, 8);
    memcpy(&run->dateMs, buf + 0x2C, 8);
    ReadFixed(run->map, sizeof(run->map), buf + 0x34, 64);
    ReadFixed(run->srcSha1, sizeof(run->srcSha1), buf + 0x9C, 40);
    ReadFixed(run->player, sizeof(run->player), buf + 0xC4, 32);
    run->gamemode = buf[0xF8];
    run->trackType = buf[0xF9];
    run->trackNum = buf[0xFA];
    strcpy_s(run->file, sizeof(run->file), path);

    // The extractor's own answer for where the run starts, when it has one.
    //
    // 0xE8 and 0xEC held minSampleDist and minSampleAngleDeg, both written as
    // 0.0 since the format existed and never read by anything. Checked across
    // all 1735 .wrpath files on this machine: bytes 0xE8..0xF3 are zero in every
    // single one. So reading them as two integers costs no version bump and no
    // reader branch -- an old file reads 0, and 0 already means "unknown", which
    // is what an old file genuinely is.
    unsigned int storedStart = 0, storedStartFlags = 0;
    memcpy(&storedStart, buf + 0xE8, 4);
    memcpy(&storedStartFlags, buf + 0xEC, 4);

    run->points = (WrPoint *)malloc(sizeof(WrPoint) * nPts);
    if (!run->points)
    {
        free(buf);
        return false;
    }

    const unsigned char *p = buf + WRPATH_HEADER_BYTES;
    int kept = 0;
    for (unsigned int i = 0; i < nPts; i++, p += WRPATH_POINT_BYTES)
    {
        float v[7];
        memcpy(v, p, sizeof(v));
        Vec3 pos = WrVec(v[0], v[1], v[2]);
        if (!WrSaneVec(pos))
            continue;       // never let a bad float reach the renderer
        run->points[kept].pos = pos;
        run->points[kept].vel = WrVec(v[3], v[4], v[5]);
        run->points[kept].t = v[6];
        kept++;
    }
    run->pointCount = kept;

    const unsigned char *m = buf + WRPATH_HEADER_BYTES
                           + (size_t)nPts * WRPATH_POINT_BYTES;
    run->markerCount = 0;
    for (unsigned int i = 0; i < nMks && run->markerCount < WR_MAX_MARKERS;
         i++, m += WRPATH_MARKER_BYTES)
    {
        WrMarker mk;
        memset(&mk, 0, sizeof(mk));
        memcpy(&mk.pointIndex, m + 0, 4);
        memcpy(&mk.segment, m + 4, 2);
        memcpy(&mk.minorNum, m + 6, 2);
        memcpy(&mk.timeReached, m + 8, 8);
        float mv[4];
        memcpy(mv, m + 16, sizeof(mv));
        mk.vel = WrVec(mv[0], mv[1], mv[2]);
        mk.maxSpeed = mv[3];
        if (mk.pointIndex >= (unsigned int)run->pointCount)
            continue;
        run->markers[run->markerCount++] = mk;
    }

    // Precompute what the renderer needs so the hot path stays arithmetic-free.
    run->speedMin = 1e9f;
    run->speedMax = 0.0f;
    run->pathLength = 0.0f;
    for (int i = 0; i < run->pointCount; i++)
    {
        float s = WrLength(run->points[i].vel);
        if (s < run->speedMin) run->speedMin = s;
        if (s > run->speedMax) run->speedMax = s;
        if (i > 0)
            run->pathLength += WrDist(run->points[i - 1].pos, run->points[i].pos);
    }
    if (run->speedMin > run->speedMax)
    {
        run->speedMin = 0.0f;
        run->speedMax = 1.0f;
    }
    if (run->pointCount > 0)
        run->startPos = run->points[0].pos;
    run->nearestDist = -1.0f;
    run->nearestIndex = -1;
    run->tagIndex = -1;

    // Find the teleports. wrpath_extract.py's DP caps the distance between
    // consecutive samples within a leg at MAX_STEP = 200 units, so anything
    // larger than that in a written file is a join between two legs that
    // harvest_segments() stitched together -- i.e. exactly a teleport.
    run->breaks = NULL;
    run->breakCount = 0;
    int nBreaks = 0;
    for (int i = 0; i + 1 < run->pointCount; i++)
        if (WrDistSqr(run->points[i].pos, run->points[i + 1].pos) >
            WR_TELEPORT_UNITS * WR_TELEPORT_UNITS)
            nBreaks++;
    if (nBreaks > 0)
    {
        run->breaks = (int *)malloc(sizeof(int) * nBreaks);
        if (run->breaks)
        {
            for (int i = 0; i + 1 < run->pointCount; i++)
                if (WrDistSqr(run->points[i].pos, run->points[i + 1].pos) >
                    WR_TELEPORT_UNITS * WR_TELEPORT_UNITS)
                    run->breaks[run->breakCount++] = i;
        }
    }

    // Points that failed WrSaneVec were dropped above, which shifts every index
    // after the first bad one. The extractor's stored start is a file index, so
    // it only means anything when nothing was dropped.
    FindStart(run, (int)storedStart, kept == (int)nPts && storedStartFlags != 0);

    BuildBounds(run);
    FindDips(run);
    FindPeaks(run);
    CheckTimes(run);        // after breaks and FindStart: a break makes the clock
                            // untrustworthy, and the pre-roll used to dilute it
    FindEfficiency(run);    // after breaks: never differences across a teleport

    // No energy array. It would only ever be read one element at a time -- the
    // point nearest the camera -- and caching it here would go silently stale
    // the moment the gravity setting moved. WrEnergyOf() on the stored velocity
    // costs nothing and is always current.

    free(buf);
    return run->pointCount >= 2;
}

// Where this run places among the loaded runs OF ITS OWN LEG.
//
// Within the track, and that is not a detail. The store is sorted by runTime
// across every track at once, so its first entry is the numerically smallest
// time in the file -- which on a staged map is usually a stage run, not the
// main one. Locally, bhop_futile holds twenty main runs between 52.85 and
// 54.34 s and one bonus at 33.97 s: rank naively and the bonus takes gold from
// a main track it was never racing.
//
// Returns 1 for the fastest. `outOf` gets how many runs share the leg, which is
// what a colour ramp needs to spread itself over.
//
// A lookup, not a search. ComputeRanks below fills these in when the store
// settles; see the note on WrRun::rank for why the scan that used to live here
// could not stay.
int WrRunRankInTrack(const WrRun *run, int *outOf)
{
    if (outOf)
        *outOf = run ? run->rankOutOf : 0;
    return run ? run->rank : 0;
}

// Place every run on its own leg, once.
//
// Quadratic, and deliberately so: it is the same comparison the per-query scan
// made, hoisted to run one time per store rather than several times per drawn
// run per frame. A thousand runs is a million trivial compares -- about a
// millisecond, once, next to a qsort and a directory full of file reads.
//
// Written to make no assumption about the store's order, so it is equally right
// when called on a store that has just been sorted and on one a test filled by
// hand.
static void ComputeRanks(void)
{
    for (int i = 0; i < g_runCount; i++)
    {
        WrRun *run = &g_runs[i];
        run->rank = 0;
        run->rankOutOf = 0;
        if (run->pointCount < 2)
            continue;

        int rank = 1, total = 0;
        for (int j = 0; j < g_runCount; j++)
        {
            const WrRun *c = &g_runs[j];
            if (c->pointCount < 2)
                continue;
            if (c->trackType != run->trackType || c->trackNum != run->trackNum)
                continue;
            total++;
            // Strictly faster, so equal times share a place rather than one of
            // them silently losing a place to array order.
            if (c != run && c->runTime < run->runTime)
                rank++;
        }
        run->rank = rank;
        run->rankOutOf = total;
    }
}

const char *WrTrackNameOf(int trackType, int trackNum)
{
    static char buf[32];
    switch (trackType)
    {
    case 0:  return "main";
    case 1:  _snprintf_s(buf, sizeof(buf), _TRUNCATE, "stage %d", trackNum);
             return buf;
    case 2:  _snprintf_s(buf, sizeof(buf), _TRUNCATE, "bonus %d", trackNum);
             return buf;
    default: _snprintf_s(buf, sizeof(buf), _TRUNCATE, "t%d/%d",
                         trackType, trackNum);
             return buf;
    }
}

const char *WrTrackName(const WrRun *run)
{
    if (!run)
        return "?";
    return WrTrackNameOf(run->trackType, run->trackNum);
}

// The stored per-point time is not a clock, and it has to be made into one.
//
// wrpath_extract.py writes t = index * tick_interval. That is only elapsed time
// if every tick was recovered, and extraction never recovers every tick -- so
// the stored clock runs at the wrong rate, by a different amount in every run.
// Measured against each run's own recorded duration across a real library:
// surf_demise 0.96-1.00x, surf_tensor2 up to 1.87x, surf_colin_blaster_69000
// from 0.36x to 10.32x. A time comparison built on the raw value would have
// been silently wrong by a factor of ten on the map being practised.
//
// Rescaling so the last point lands on the recorded duration makes both ends
// exact. The middle is only as good as the assumption that the missing ticks
// are spread evenly, which is why anything far from 1.0 is marked untrusted
// rather than quietly used.
#define TIME_SCALE_TRUST 0.10f      // how far from 1.0 is still believable

// How much pre-roll is believable, in seconds.
//
// Measured over the 1735 .wrpath files on this machine. Extrapolating back from
// the first split marker -- whose timeReached the GAME measured -- puts the
// pre-roll at 0.19 s to 1.11 s across the deciles, 1.74 s at worst, and never
// below zero on any of the 660 files that carry markers.
//
// The range is doing more work than it looks. The back-solve below is only
// valid while the extracted point stream is COMPLETE, and on a fragmented
// extraction it does not go slightly wrong, it goes to -1089 seconds. So this
// is a completeness test wearing a plausibility test's clothes, and it is why
// the 39% of files with incomplete streams fall back to "unknown" rather than
// being handed a confident wrong number.
#define START_PREROLL_MIN (-0.15f)
#define START_PREROLL_MAX 5.0f

// How far the two independent estimates may disagree and still confirm one
// another. The back-solve agrees with the marker figure to within 0.05 s on
// 99.4% of the files that have both, and to within 0.15 s on all of them.
#define START_AGREE 0.15f

// Find the first point of the RUN, as opposed to the first point of the
// recording. See the comment on startIndex in wr_path.h for why they differ.
//
// Three routes, in decreasing order of authority:
//
//   the extractor's       matched against the JSON's effectiveStartVelocity,
//   stored index          a full-precision fingerprint. Does not care whether
//                         the stream is complete, so it is the only one that
//                         works on a fragmented extraction.
//
//   the markers           extrapolated back from the first split, whose time
//                         the game measured. A measurement, not an inference.
//
//   the back-solve        (pointCount-1) - runTime/tick. The extracted stream
//                         ends AT the finish -- implied post-roll is a median
//                         0.00 s over every file here that can be checked --
//                         so every surplus tick is at the front.
//
// Agreement between the last two is what earns "trusted". Neither available
// leaves startIndex at 0, which is what every run did before this existed.
static void FindStart(WrRun *run, int stored, bool storedUsable)
{
    run->startIndex = 0;
    run->startTrusted = false;
    if (run->pointCount < 2)
        return;

    float dt = run->tickInterval;
    if (!(dt > 1e-6f) || !(run->runTime > 0.0))
    {
        run->startPos = run->points[0].pos;
        return;
    }

    // >= 0, not > 0: storedUsable already carries the extractor's "I found it"
    // bit, so an index of genuinely zero -- a demo with no pre-roll at all -- is
    // an answer and not a missing one.
    if (storedUsable && stored >= 0 && stored < run->pointCount)
    {
        run->startIndex = stored;
        run->startTrusted = true;
        run->startPos = run->points[run->startIndex].pos;
        return;
    }

    float back = (float)(run->pointCount - 1) * dt - (float)run->runTime;
    int backIdx = (int)(back / dt + 0.5f);
    bool backOk = back >= START_PREROLL_MIN && back <= START_PREROLL_MAX;
    if (backIdx < 0)
        backIdx = 0;                    // a fraction of a tick negative, rounded
    if (backIdx >= run->pointCount)
        backOk = false;

    int markIdx = -1;
    if ((run->flags & WRPATH_FLAG_MARKERS_OK) && run->markerCount > 0 &&
        run->markers[0].timeReached > 0.0)
    {
        markIdx = (int)run->markers[0].pointIndex
                - (int)(run->markers[0].timeReached / (double)dt + 0.5);
        float mark = (float)markIdx * dt;
        if (mark < START_PREROLL_MIN || mark > START_PREROLL_MAX ||
            markIdx >= run->pointCount)
            markIdx = -1;
        else if (markIdx < 0)
            markIdx = 0;
    }

    if (markIdx >= 0 && backOk)
    {
        float gap = (float)(markIdx - backIdx) * dt;
        if (gap < 0.0f)
            gap = -gap;
        run->startIndex = markIdx;      // measured beats inferred on a tie
        run->startTrusted = (gap <= START_AGREE);
    }
    else if (markIdx >= 0)
    {
        run->startIndex = markIdx;
        run->startTrusted = true;
    }
    else if (backOk)
    {
        run->startIndex = backIdx;
        run->startTrusted = true;
    }

    run->startPos = run->points[run->startIndex].pos;
}

// Decide whether this run's stored times can be used as a clock. A TEST, not a
// correction -- see the comment on timeScale in wr_path.h. Rescaling would
// stretch a clock whose rate is already right.
static void CheckTimes(WrRun *run)
{
    run->timeScale = 1.0f;
    run->timingTrusted = false;
    if (run->pointCount < 2 || run->runTime <= 0.0f)
        return;

    // Over the run, not over the recording. This used to divide runTime by the
    // last point's time with the pre-roll still in it, which conflates two
    // different faults: ticks the extractor missed, and ticks that were never
    // part of the run. A 60 s run with 0.72 s of pre-roll scored 0.988 for a
    // stream with nothing wrong with it. Measuring from startIndex removes the
    // pre-roll term entirely, so what is left is genuinely the missing ticks.
    float first = run->points[run->startIndex].t;
    float span = run->points[run->pointCount - 1].t - first;
    if (!(span > 1e-3f))
        return;

    run->timeScale = (float)run->runTime / span;
    float off = run->timeScale - 1.0f;
    if (off < 0.0f) off = -off;

    // A teleport join skips an unknown duration, so any run with a break has a
    // clock with an unknown gap in it whatever the endpoints say.
    run->timingTrusted = (off <= TIME_SCALE_TRUST) &&
                         run->breakCount == 0 &&
                         !(run->flags & WRPATH_FLAG_LOW_CONFIDENCE);
}

// Per-point air-strafing efficiency: how much of the energy air acceleration
// could physically have added was actually added. See wr_stress.h, especially
// for why this is not a turn-rate metric.
#define EFF_WINDOW 4                // points either side; ~120 ms at 66 tick

// The window actually used, so it can be widened without an edit and a rebuild.
//
// Same trade as every other window in the tool: narrow follows a ramp entry and
// carries the noise of a short difference, wide is steadier and smears the
// moment a line lost its energy across the points either side of it. Four points
// either side is about an eighth of a second at 66 tick.
int g_wrEffWindow = EFF_WINDOW;

static void FindEfficiency(WrRun *run)
{
    int win = WrClampI(g_wrEffWindow, 1, 64);
    free(run->eff);
    run->eff = NULL;
    run->effWindow = win;
    if (run->pointCount < win * 2 + 1)
        return;
    run->eff = (signed char *)malloc((size_t)run->pointCount);
    if (!run->eff)
        return;

    // NO DATA, not neutral. This was calloc'd, so the window's points at each
    // end and every window spanning a teleport read as "eta 0" -- the same value
    // as free flight. A gap in the measurement and a player coasting are not the
    // same thing and must not draw the same.
    memset(run->eff, WR_ETA_NO_DATA, (size_t)run->pointCount);

    float ceiling = WrAirPowerCeiling(g_energy.gravity, run->tickInterval);
    float dt = run->tickInterval * (float)(win * 2);
    if (!(dt > 1e-5f))
        return;

    // A centred difference, so the figure belongs to the point it is drawn at
    // rather than trailing it by half a window.
    for (int i = win; i + win < run->pointCount; i++)
    {
        int a = i - win, b = i + win;

        // Never across a teleport: the join skips an unknown duration, and the
        // height either side of it is unrelated.
        bool spans = false;
        for (int k = 0; k < run->breakCount; k++)
            if (run->breaks[k] >= a && run->breaks[k] < b)
            {
                spans = true;
                break;
            }
        if (spans)
            continue;

        float ea = WrEnergyOf(run->points[a].pos, run->points[a].vel);
        float eb = WrEnergyOf(run->points[b].pos, run->points[b].vel);
        float power = (eb - ea) / dt;

        // A booster stays no-data. Only the GAIN side is rejected: losing faster
        // than the ceiling is a ramp entry or a wall, which is 18.8% of all
        // samples and 94.6% of all energy lost, and is the thing worth seeing.
        if (WrEtaIsNoData(power, ceiling))
            continue;
        run->eff[i] = WrEtaToByte(WrEfficiency(power, ceiling));
    }
}

// Distance from the camera to the nearest point of each run.
//
// Sampled, not exhaustive: 64 evenly spaced points is more than enough to answer
// "is this run anywhere near me", and it keeps this at a few thousand distance
// tests per frame regardless of how many runs are loaded.
// How many points the refine pass may examine, whatever the run's length.
//
// The coarse pass samples 64 points and then refined the whole bracket around
// the winner -- which is pointCount/64 wide, so on a 38 751-point run that was
// 1211 extra distance tests. Sixty-four of them find the minimum of a smooth
// path to well inside a unit; the rest were spent proving it.
#define REFINE_BUDGET 64

// How many disabled runs to re-measure per frame.
//
// This used to run over every loaded run, every frame, enabled or not, and the
// comment claimed 64 samples per run. With the refine bracket it was closer to
// 1275 for a long run, so 256 loaded runs cost roughly 326 000 distance tests
// per frame to keep a column up to date that nobody is reading mid-surf.
//
// Enabled runs still update every frame -- the energy and time comparisons read
// nearestIndex and must be exact. The rest take turns.
#define NEAREST_PER_FRAME 4

static void MeasureNearest(WrRun *r, const Vec3 &cam)
{
    if (r->pointCount < 2)
    {
        r->nearestDist = -1.0f;
        r->nearestIndex = -1;
        return;
    }
    int step = r->pointCount / 64;
    if (step < 1)
        step = 1;
    float best = 1e18f;
    int bestIdx = 0;
    for (int p = 0; p < r->pointCount; p += step)
    {
        float d = WrDistSqr(r->points[p].pos, cam);
        if (d < best)
        {
            best = d;
            bestIdx = p;
        }
    }
    // Refine within the sampled bracket, so the energy read off this index is
    // the run's energy where you actually are rather than up to 64 points away.
    int rstep = (step * 2 + REFINE_BUDGET - 1) / REFINE_BUDGET;
    if (rstep < 1)
        rstep = 1;
    int lo = bestIdx - step, hi = bestIdx + step;
    if (lo < 0) lo = 0;
    if (hi > r->pointCount - 1) hi = r->pointCount - 1;
    for (int p = lo; p <= hi; p += rstep)
    {
        float d = WrDistSqr(r->points[p].pos, cam);
        if (d < best)
        {
            best = d;
            bestIdx = p;
        }
    }
    r->nearestDist = sqrtf(best);
    r->nearestIndex = bestIdx;
}

void WrUpdateNearest(const Vec3 &cam)
{
    static int cursor = 0;

    for (int i = 0; i < g_runCount; i++)
        if (g_runs[i].enabled)
            MeasureNearest(&g_runs[i], cam);

    for (int n = 0; n < NEAREST_PER_FRAME && g_runCount > 0; n++)
    {
        cursor = (cursor + 1) % g_runCount;
        WrRun *r = &g_runs[cursor];
        if (!r->enabled)
            MeasureNearest(r, cam);
    }

    // The first time distances are known after a load, turn on the fastest run
    // that actually comes near you rather than the fastest run in the file list.
    //
    // Those are usually not the same run. Momentum records a separate demo per
    // stage, so on a staged map the fastest recorded time is generally a single
    // short stage somewhere else entirely -- and enabling it puts a line in the
    // map that you cannot see from where you spawned, which reads exactly like
    // "I changed map and no lines showed up".
    //
    // Deferred to here rather than done at load time because it needs a live
    // camera, and during a level load the last known camera still belongs to the
    // previous map.
    if (g_autoEnablePending && g_runCount > 0)
    {
        g_autoEnablePending = false;
        // This one frame measures everything: picking the best run near you is
        // exactly the decision that needs every run's distance at once, and the
        // round-robin above has only touched a handful of them so far.
        for (int i = 0; i < g_runCount; i++)
            MeasureNearest(&g_runs[i], cam);
        WrEnableBestNearby(1, AUTO_ENABLE_RADIUS);
        int on = 0;
        for (int i = 0; i < g_runCount; i++)
            if (g_runs[i].enabled)
                on++;
        if (on == 0)
        {
            g_runs[0].enabled = true;   // nothing nearby: the overall best it is
            WrLogf("no run passes within %.0f units of you; enabled the fastest "
                   "one instead (\"%s\", %s)", AUTO_ENABLE_RADIUS,
                   g_runs[0].player, WrTrackName(&g_runs[0]));
        }
    }
}

// Rebuild every run's efficiency array. Runs already at the wanted window are
// skipped, so calling this every frame would be free -- but it is not called
// every frame, because on a full store a real rebuild is a pass over millions of
// points and that belongs on a slider release rather than inside Present.
float WrRunTimeAt(const WrRun *run, int index)
{
    if (!run || index < 0 || index >= run->pointCount)
        return 0.0f;
    int from = run->startIndex;
    if (from < 0 || from >= run->pointCount)
        from = 0;
    return (run->points[index].t - run->points[from].t) * run->timeScale;
}

void WrPathRefreshEfficiency(void)
{
    int want = WrClampI(g_wrEffWindow, 1, 64);
    for (int i = 0; i < g_runCount; i++)
        if (g_runs[i].effWindow != want)
            FindEfficiency(&g_runs[i]);
}

void WrEnableBestNearby(int count, float radius)
{
    // Runs are already sorted fastest-first, so the first `count` within range
    // are the fastest ones covering where we are standing.
    int on = 0;
    for (int i = 0; i < g_runCount; i++)
    {
        WrRun *r = &g_runs[i];
        // Not `near`: windows.h still defines that as a macro.
        bool inRange = (r->nearestDist >= 0.0f && r->nearestDist <= radius);
        r->enabled = (inRange && on < count);
        if (r->enabled)
            on++;
    }
}

static int CompareByTime(const void *a, const void *b)
{
    const WrRun *ra = (const WrRun *)a;
    const WrRun *rb = (const WrRun *)b;
    if (ra->runTime < rb->runTime) return -1;
    if (ra->runTime > rb->runTime) return 1;
    return 0;
}

// Loading is spread over frames rather than done in one go.
//
// WrPathLoadMap runs inside Present, and a .wrpath costs a few milliseconds to
// read, CRC, and scan for teleports and dips. With 64 runs that was a barely
// noticeable hitch; the busiest map on disk has 125 demos, and at 256 the
// one-shot version would stall the render thread for the better part of a
// second every time you loaded a map.
//
// So the file list is collected up front -- cheap, one directory walk -- and the
// files themselves are loaded a few per frame. The run store is usable the whole
// time; the list fills in behind you.
//
// At the raised cap that fill is no longer instant: a thousand runs at four a
// frame is 250 frames, so roughly four seconds at 60 Hz. Left at four
// deliberately -- the number that matters here is the size of the worst hitch,
// not the total, and the panel says "loading runs n / total" throughout. Runs
// stay unranked until the last one is in (see ComputeRanks), so during those
// seconds the lines wear their palette colours and then settle.
#define LOAD_PER_FRAME 4

static char (*g_pending)[MAX_PATH] = NULL;
static int g_pendingCount = 0;
static int g_pendingNext = 0;
static int g_pendingFailed = 0;

static void FinishLoad(void)
{
    qsort(g_runs, (size_t)g_runCount, sizeof(WrRun), CompareByTime);
    for (int i = 0; i < g_runCount; i++)
    {
        g_runs[i].colour = PaletteColour(i);
        g_runs[i].enabled = (i == 0);       // provisional; see WrUpdateNearest
    }
    ComputeRanks();     // after the sort, and after the last run is in
    g_generation++;     // the store is now what it is going to be
    g_autoEnablePending = true;

    WrLogf("loaded %d run%s for \"%s\"%s", g_runCount, g_runCount == 1 ? "" : "s",
           g_loadedMap, g_pendingFailed ? " (some files rejected)" : "");
    if (g_pendingFailed)
        WrLogf("[!] %d .wrpath file%s rejected -- see lines above", g_pendingFailed,
               g_pendingFailed == 1 ? "" : "s");

    free(g_pending);
    g_pending = NULL;
    g_pendingCount = g_pendingNext = g_pendingFailed = 0;
}

void WrPathLoadTick(void)
{
    if (!g_pending)
        return;
    for (int n = 0; n < LOAD_PER_FRAME && g_pendingNext < g_pendingCount; n++)
    {
        // Unreachable, and kept: WrPathLoadMap caps g_pendingCount at
        // WR_MAX_RUNS and warns there, so g_runCount cannot reach the cap here.
        // This is the bounds check on the &g_runs[g_runCount] write below, and
        // unreachable is the correct state for one of those.
        if (g_runCount >= WR_MAX_RUNS)
        {
            g_pendingNext = g_pendingCount;
            break;
        }
        if (LoadOne(g_pending[g_pendingNext], &g_runs[g_runCount]))
            g_runCount++;
        else
            g_pendingFailed++;
        g_pendingNext++;
    }
    if (g_pendingNext >= g_pendingCount)
        FinishLoad();
}

bool WrPathLoading(int *done, int *total)
{
    if (done) *done = g_pendingNext;
    if (total) *total = g_pendingCount;
    return g_pending != NULL;
}

void WrPathLoadMap(const char *map)
{
    FreeRuns();
    g_generation++;         // whatever was derived from the old store is void
    free(g_pending);
    g_pending = NULL;
    g_pendingCount = g_pendingNext = g_pendingFailed = 0;

    g_loadedMap[0] = '\0';
    if (!map || !*map)
        return;
    strcpy_s(g_loadedMap, sizeof(g_loadedMap), map);

    char pattern[MAX_PATH];
    char dir[MAX_PATH];
    _snprintf_s(dir, sizeof(dir), _TRUNCATE, "paths\\%s", map);
    const char *base = WrDataPath(dir);
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*.wrpath", base);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        WrLogf("no cached paths for \"%s\" (looked in %s)", map, base);
        return;
    }

    g_pending = (char (*)[MAX_PATH])malloc(sizeof(*g_pending) * WR_MAX_RUNS);
    if (!g_pending)
    {
        FindClose(h);
        return;
    }

    // This break is where truncation actually happens -- the cap in
    // WrPathLoadTick is a bounds guard that cannot fire, because this runs
    // first. It used to be silent, and it was firing: surf_demise has 273
    // .wrpath files on this machine against the old cap of 256, so seventeen
    // runs were being dropped with nothing said anywhere. The map picker shows
    // the count on disk and the log says how many loaded, and nobody is
    // expected to subtract two numbers in different parts of the UI.
    int found = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        found++;
        if (g_pendingCount >= WR_MAX_RUNS)
            continue;           // keep counting, so the message can be honest
        _snprintf_s(g_pending[g_pendingCount], MAX_PATH, _TRUNCATE, "%s\\%s",
                    base, fd.cFileName);
        g_pendingCount++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if (found > g_pendingCount)
        WrLogf("[!] \"%s\" has %d paths on disk and the store holds %d; "
               "%d were not loaded", map, found, WR_MAX_RUNS,
               found - g_pendingCount);

    if (g_pendingCount == 0)
        FinishLoad();
}

int WrRunCount(void) { return g_runCount; }
unsigned int WrRunStoreGeneration(void) { return g_generation; }
WrRun *WrRunAt(int i) { return (i >= 0 && i < g_runCount) ? &g_runs[i] : NULL; }

// Fill the store directly, for tests\test_rank.exe.
//
// Here rather than in the harness so the ranking is tested against the REAL
// store, the way test_energy drives the real sampler. Ranking is a property of
// what is loaded, and a test that ranked its own private array would pass while
// the shipped function looked at something else. Nothing in the DLL calls this;
// it allocates nothing, so the ordinary FreeRuns path is untouched.
void WrPathTestLoad(const WrRun *runs, int count)
{
    if (count > WR_MAX_RUNS)
        count = WR_MAX_RUNS;
    memset(g_runs, 0, sizeof(g_runs));
    g_runCount = 0;
    if (!runs || count <= 0)
        return;
    memcpy(g_runs, runs, sizeof(WrRun) * (size_t)count);
    g_runCount = count;
    ComputeRanks();     // the loader's job, done here because there is no loader
    g_generation++;     // so anything derived from the store rebuilds
}

// Run the real start recovery over one run, for tests\test_start.exe.
//
// The same reasoning as WrPathTestLoad: FindStart is where the pre-roll
// arithmetic lives, and a harness that reimplemented that arithmetic would agree
// with itself rather than with the shipped code.
void WrPathTestFindStart(WrRun *run, int stored, bool storedUsable)
{
    FindStart(run, stored, storedUsable);
}
const char *WrPathLoadedMap(void) { return g_loadedMap; }

void WrPathCancelAutoEnable(void) { g_autoEnablePending = false; }

bool WrRunIsFrom(const WrRun *run, const char *stem)
{
    if (!run || !stem || !*stem || !run->srcSha1[0])
        return false;

    const size_t stored = strlen(run->srcSha1);
    const size_t want = strlen(stem);

    // The stored field is the prefix, never the other way round: it is what got
    // cut. A stem SHORTER than what we hold cannot be the same demo.
    if (stored > want)
        return false;

    // Eight characters, which is the same floor WrIntoGame's hash guard uses.
    // Below that a prefix match is not evidence of anything.
    if (stored < 8)
        return _stricmp(run->srcSha1, stem) == 0;

    // One character is all that is ever lost, so anything shorter than that is
    // a different name rather than a truncation of this one.
    if (want - stored > 1)
        return false;

    return _strnicmp(run->srcSha1, stem, stored) == 0;
}

int WrRunEnabledCount(void)
{
    int n = 0;
    for (int i = 0; i < g_runCount; i++)
        if (g_runs[i].enabled)
            n++;
    return n;
}

void WrPathShutdown(void) { FreeRuns(); WrProfileShutdown(); }

// ---------------------------------------------------------------------------
// Available maps (whatever the extractor has produced)
// ---------------------------------------------------------------------------

#define WR_MAX_AVAIL_MAPS 256

static char g_availMap[WR_MAX_AVAIL_MAPS][72];
static int g_availRuns[WR_MAX_AVAIL_MAPS];
static int g_availCount = 0;

void WrScanAvailableMaps(void)
{
    g_availCount = 0;

    char pattern[MAX_PATH];
    const char *base = WrDataPath("paths");
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*", base);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (fd.cFileName[0] == '.')
            continue;
        if (g_availCount >= WR_MAX_AVAIL_MAPS)
            break;

        char sub[MAX_PATH];
        _snprintf_s(sub, sizeof(sub), _TRUNCATE, "%s\\%s\\*.wrpath", base,
                    fd.cFileName);
        int n = 0;
        WIN32_FIND_DATAA f2;
        HANDLE h2 = FindFirstFileA(sub, &f2);
        if (h2 != INVALID_HANDLE_VALUE)
        {
            do { n++; } while (FindNextFileA(h2, &f2));
            FindClose(h2);
        }
        if (n == 0)
            continue;

        strcpy_s(g_availMap[g_availCount], sizeof(g_availMap[0]), fd.cFileName);
        g_availRuns[g_availCount] = n;
        g_availCount++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    WrLogf("%d map%s have extracted paths", g_availCount,
           g_availCount == 1 ? "" : "s");
}

int WrAvailableMapCount(void) { return g_availCount; }

const char *WrAvailableMapAt(int i)
{
    return (i >= 0 && i < g_availCount) ? g_availMap[i] : "";
}

int WrAvailableMapRuns(int i)
{
    return (i >= 0 && i < g_availCount) ? g_availRuns[i] : 0;
}

// ---------------------------------------------------------------------------
// Live self-recording
// ---------------------------------------------------------------------------

bool WrLiveEnabled(void) { return g_liveOn; }
void WrLiveSetEnabled(bool on) { g_liveOn = on; }

void WrLiveClear(void)
{
    g_liveCount = 0;
    g_liveHold = false;     // clearing is also how the hold is let go of
}

bool WrLiveHeld(void) { return g_liveHold; }

// Stop recording without throwing away what has been recorded.
//
// The buffer used to be wiped by the teleport branch in WrLiveRecord below, so
// failing a run erased the attempt you had just made -- and since the graph is
// something you look at AFTER failing, it always looked as though opening the
// graph had reset it. Holding keeps the attempt on screen until the next one
// actually starts, which is when leaving the start zone clears it.
//
// A hold rather than "keep appending", and that is not a preference: a live
// point's t is the run clock, which is zeroed on a restart and again at the
// start line, so appending across one would send the graph's time axis
// backwards -- and wr_profile.cpp binary-searches that axis.
void WrLiveHold(bool on)
{
    g_liveHold = on;
}

const WrPoint *WrLivePoints(int *count)
{
    if (count)
        *count = g_liveCount;
    return g_live;
}

// Linear, but strided coarse-then-refine like MeasureNearest, because this is
// asked once per drawn label rather than once per frame and the buffer holds
// 32768 points.
const WrPoint *WrLiveNearest(const Vec3 &pos, float radius)
{
    if (g_liveCount < 1)
        return NULL;

    int step = g_liveCount / 256;
    if (step < 1) step = 1;

    int best = -1;
    float bestSqr = radius * radius;
    for (int i = 0; i < g_liveCount; i += step)
    {
        float d = WrDistSqr(g_live[i].pos, pos);
        if (d < bestSqr) { bestSqr = d; best = i; }
    }
    if (best < 0)
        return NULL;

    int lo = best - step, hi = best + step;
    if (lo < 0) lo = 0;
    if (hi >= g_liveCount) hi = g_liveCount - 1;
    for (int i = lo; i <= hi; i++)
    {
        float d = WrDistSqr(g_live[i].pos, pos);
        if (d < bestSqr) { bestSqr = d; best = i; }
    }
    return &g_live[best];
}

// The velocity and the clock are passed in rather than derived here.
//
// This used to store the raw position DELTA in `vel` and cumulative DISTANCE in
// `t`, neither of which is what the field names say, and that made your own line
// the only one that could not be asked "how fast was I here, and when?". The
// energy sampler has already computed a smoothed velocity from the same camera
// this is being fed, and the timer already has the elapsed run time, so both are
// free -- and it is what lets a label on somebody else's line say how you
// compare at that point.
void WrLiveRecord(const Vec3 &pos, const Vec3 &vel, float elapsed)
{
    if (!g_liveOn || g_liveHold || !WrSaneVec(pos))
        return;

    Vec3 v = WrSaneVec(vel) ? vel : WrVec(0.0f, 0.0f, 0.0f);

    if (g_liveCount == 0)
    {
        g_live[0].pos = pos;
        g_live[0].vel = v;
        g_live[0].t = elapsed;
        g_liveCount = 1;
        return;
    }

    WrPoint *last = &g_live[g_liveCount - 1];
    float moved = WrDist(last->pos, pos);

    // A teleport (savestate load, stage restart) should break the line rather
    // than draw a straight bar across the map.
    //
    // 400 rather than the 512 this used to be, to agree with the one teleport
    // threshold the rest of the tool uses (TELEPORT_UNITS, wr_energy.cpp). They
    // disagreed, so a jump between 400 and 512 units was a teleport everywhere
    // else and ordinary movement here -- drawn as the straight bar the comment
    // above says cannot happen.
    //
    // Reached far less often now: a restart holds the buffer instead of
    // arriving here, so what is left for this branch is the teleports nobody
    // called a restart, which is what it was always meant to be for.
    if (moved > WR_LIVE_TELEPORT_UNITS)
    {
        g_liveCount = 0;
        g_live[0].pos = pos;
        g_live[0].vel = v;
        g_live[0].t = elapsed;
        g_liveCount = 1;
        return;
    }

    if (moved < 2.0f)
        return;

    if (g_liveCount >= WR_LIVE_POINTS)
    {
        // Drop the oldest half rather than stopping dead, so a long session
        // keeps showing the recent path.
        int keep = WR_LIVE_POINTS / 2;
        memmove(g_live, g_live + (WR_LIVE_POINTS - keep), sizeof(WrPoint) * keep);
        g_liveCount = keep;
        last = &g_live[g_liveCount - 1];
    }

    g_live[g_liveCount].pos = pos;
    g_live[g_liveCount].vel = v;
    g_live[g_liveCount].t = elapsed;
    g_liveCount++;
}
