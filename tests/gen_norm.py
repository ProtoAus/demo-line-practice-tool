#!/usr/bin/env python3
# gen_norm.py  --  writes tests/fixture_norm.h from the reference interpreter.
#
# NOT RUN BY CI, and not run by tests\build.bat. It needs the exact CPython the
# port is bit-compatible with, and the whole point of the table it writes is to
# be checked in and compared against -- regenerating it on the machine under
# test would make the comparison say nothing.
#
# WHY THE TABLE IS HALF-SELECTED AND NOT ALL RANDOM
#
# math.dist is CPython's compensated vector_norm, not sqrt(dx*dx+dy*dy+dz*dz),
# and on most inputs the two agree exactly -- 400 uniformly random world
# coordinates a tick apart produced ZERO disagreements on the first attempt.
# A table like that cannot tell a correct transcription from a naive one, which
# makes it worse than no table at all: it would pass either way.
#
# So half the rows are SEARCHED FOR: pairs where the two forms actually differ.
# They are not exotic -- 67 in 20000 at a 74-unit step, which is exactly the
# step length the fastest runs on disk produce at a 0.015 s tick. The other half
# are drawn at random, because a table of nothing but disagreements would not
# check that the two agree where they should.
#
#   python tests\gen_norm.py

import math
import random
import struct
import sys


def bits(x):
    return struct.unpack("<Q", struct.pack("<d", x))[0]


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def naive3(a, b):
    dx, dy, dz = a[0] - b[0], a[1] - b[1], a[2] - b[2]
    return math.sqrt(dx * dx + dy * dy + dz * dz)


def naive2(x, y):
    return math.sqrt(x * x + y * y)


random.seed(20260810)       # fixed, so re-running produces the same table

# (coordinate scale, step) pairs covering what the extractor actually meets: a
# stage near the world origin, an ordinary surf map, the far corner of
# surf_colin_blaster_69000, and a tick at the fastest speed on disk.
SHAPES = [(300.0, 3.0), (2000.0, 7.3), (2000.0, 74.0), (20000.0, 60.0),
          (65536.0, 199.0), (1.0, 1.0)]


def one_pair():
    scale, step = random.choice(SHAPES)
    a = tuple(f32(random.uniform(-scale, scale)) for _ in range(3))
    b = tuple(f32(a[i] + random.uniform(-step, step)) for i in range(3))
    return a, b


dist_rows = []
differs_dist = 0

# Searched: rows where the compensated form and the naive one disagree.
while differs_dist < 150:
    a, b = one_pair()
    if math.dist(a, b) != naive3(a, b):
        dist_rows.append((a, b, math.dist(a, b)))
        differs_dist += 1

# Drawn: whatever comes up.
for _ in range(250):
    a, b = one_pair()
    if math.dist(a, b) != naive3(a, b):
        differs_dist += 1
    dist_rows.append((a, b, math.dist(a, b)))

# By hand: the arms of vector_norm that random inputs will not reach. Counted
# like every other row -- two of these DO differ from naive sqrt, and a table
# whose advertised count did not include them would fail its own check.
for a, b in [((0.0, 0.0, 0.0), (0.0, 0.0, 0.0)),          # max == 0
             ((1.0, 2.0, 3.0), (1.0, 2.0, 3.0)),          # every component 0
             ((65536.0, 0.0, 0.0), (-65536.0, 0.0, 0.0)),  # one huge component
             ((5e-324, 0.0, 0.0), (0.0, 0.0, 0.0)),       # the subnormal branch
             ((1e-300, 1e-300, 1e-300), (0.0, 0.0, 0.0)),  # scaling, not overflow
             ((3.0, 4.0, 0.0), (0.0, 0.0, 0.0)),          # exact, famously
             ((1.0, 1.0, 1.0), (0.0, 0.0, 0.0))]:         # irrational, not exact
    if math.dist(a, b) != naive3(a, b):
        differs_dist += 1
    dist_rows.append((a, b, math.dist(a, b)))

hyp_rows = []
differs_hyp = 0
while differs_hyp < 80:
    x = f32(random.uniform(-4000, 4000))
    y = f32(random.uniform(-4000, 4000))
    if math.hypot(x, y) != naive2(x, y):
        hyp_rows.append((x, y, math.hypot(x, y)))
        differs_hyp += 1
for _ in range(150):
    x = f32(random.uniform(-4000, 4000))
    y = f32(random.uniform(-4000, 4000))
    if math.hypot(x, y) != naive2(x, y):
        differs_hyp += 1
    hyp_rows.append((x, y, math.hypot(x, y)))

# ---------------------------------------------------------------------------
# sum(), which is also not what it looks like
# ---------------------------------------------------------------------------
#
# Python's builtin sum() over floats has been Neumaier-compensated since 3.12.
# The reference uses it twice, and one of those decides whether a segment is
# kept -- so the port needs the same algorithm, and the same argument applies as
# for the norm: a table that never disagrees with a running total proves
# nothing.
#
# The values are not stored. They are GENERATED from an integer LCG on both
# sides, so the header carries one seed and one expected answer per row instead
# of ten thousand bit patterns. The conversion is exact in both languages:
# (s >> 11) is at most 53 bits, so it is representable, and the two multiplies
# are ordinary IEEE double operations that round the same way.
#
# The shapes are chosen to be what the two call sites actually feed it. A path
# length is thousands of same-signed terms of similar size, which is where a
# running total drifts steadily; a centroid coordinate can be tens of thousands
# of terms straddling zero, which is where catastrophic cancellation makes the
# compensation matter far more than the count suggests.

SUM_SHAPES = [
    # (count, scale, bias) -- bias 0 straddles zero, bias 1 is all one sign.
    (16, 74.0, 1.0),            # a very short path
    (300, 7.3, 1.0),            # an ordinary one
    (9819, 3.4, 1.0),           # the demo that found this bug, near enough
    (40000, 2.0, 1.0),          # the longest run on disk
    (2000, 65536.0, 0.0),       # centroid, far corner of the world, both signs
    (30000, 2000.0, 0.0),       # centroid, ordinary map, both signs
    (500, 1e12, 0.0),           # magnitudes that swamp each other
]


def lcg_values(seed, count, scale, bias):
    """The same doubles test_dp will regenerate. Integer state, exact
    conversion, then two rounding operations that both languages agree on."""
    s = seed & 0xFFFFFFFFFFFFFFFF
    out = []
    for _ in range(count):
        s = (s * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        u = (s >> 11) * (2.0 ** -53)            # [0, 1), exact
        out.append((u - 0.5 + 0.5 * bias) * scale)
    return out


def naive_sum(xs):
    t = 0.0
    for x in xs:
        t += x
    return t


sum_rows = []
differs_sum = 0
for i, (count, scale, bias) in enumerate(SUM_SHAPES):
    seed = 0x5741524C494E4553 + i
    xs = lcg_values(seed, count, scale, bias)
    want = sum(xs)
    if want != naive_sum(xs):
        differs_sum += 1
    sum_rows.append((seed, count, scale, bias, want))

# The signed-zero row, and the answer is the interesting part: sum() starts from
# the INTEGER 0, so 0 + (-0.0) is +0.0 and a sum of nothing but negative zeros
# comes out POSITIVE. CPython's guard against folding in a zero correction is
# what keeps it there. Recorded as whatever the interpreter says rather than as
# what the guard suggests, which is the only way round for a fixture.
NEG_ZERO_WANT = sum([-0.0, -0.0, -0.0])

out = [
    "// fixture_norm.h  --  GENERATED. Do not edit; see tests\\gen_norm.py.",
    "//",
    "// (input, expected) pairs for CPython's math.dist and math.hypot, produced",
    "// by the reference interpreter this port is bit-compatible with:",
    "//",
    "//     %s" % sys.version.replace("\n", " "),
    "//",
    "// That version number is not decoration. vector_norm has changed across",
    "// releases, and it is the whole of what _peak_horizontal_speed compares",
    "// against the run's own recorded max speed.",
    "//",
    "// Everything is a raw IEEE-754 bit pattern rather than a decimal literal,",
    "// because the point of the table is the last place and a decimal literal",
    "// is one more thing that could round.",
    "//",
    "// %d of the %d distance rows and %d of the %d hypot rows differ from naive" %
    (differs_dist, len(dist_rows), differs_hyp, len(hyp_rows)),
    "// sqrt(x*x + y*y + z*z). Half of those were searched for rather than",
    "// stumbled on -- see gen_norm.py -- and test_dp asserts the count is not",
    "// zero, because a table that never distinguishes the two forms would pass",
    "// whichever one was transcribed.",
    "",
    "#ifndef WR_FIXTURE_NORM_H",
    "#define WR_FIXTURE_NORM_H",
    "",
    "struct WrNormDist { unsigned long long a[3], b[3], want; };",
    "struct WrNormHypot { unsigned long long x, y, want; };",
    "",
    "static const WrNormDist kNormDist[] = {",
]
for a, b, d in dist_rows:
    out.append("    {{0x%016XULL,0x%016XULL,0x%016XULL},"
               "{0x%016XULL,0x%016XULL,0x%016XULL},0x%016XULL},"
               % (bits(a[0]), bits(a[1]), bits(a[2]),
                  bits(b[0]), bits(b[1]), bits(b[2]), bits(d)))
out += ["};", "", "static const WrNormHypot kNormHypot[] = {"]
for x, y, h in hyp_rows:
    out.append("    {0x%016XULL,0x%016XULL,0x%016XULL},"
               % (bits(x), bits(y), bits(h)))
out += ["};", "",
        "// Python's builtin sum() over the doubles the LCG in test_dp.cpp",
        "// regenerates from `seed`. %d of these %d rows differ from a plain"
        % (differs_sum, len(sum_rows)),
        "// running total.",
        "struct WrNormSum { unsigned long long seed; int count;",
        "                   double scale, bias; unsigned long long want; };",
        "",
        "static const WrNormSum kNormSum[] = {"]
for seed, count, scale, bias, want in sum_rows:
    out.append("    {0x%016XULL,%d,%.17g,%.1f,0x%016XULL},"
               % (seed, count, scale, bias, bits(want)))
out += ["};", "",
        "#define WR_NORM_DIST_DIFFERS %d" % differs_dist,
        "#define WR_NORM_HYPOT_DIFFERS %d" % differs_hyp,
        "#define WR_NORM_SUM_DIFFERS %d" % differs_sum,
        "#define WR_NORM_SUM_NEG_ZERO 0x%016XULL" % bits(NEG_ZERO_WANT),
        "", "#endif // WR_FIXTURE_NORM_H"]

open("tests/fixture_norm.h", "w", newline="\n").write("\n".join(out) + "\n")
print("dist %d rows (%d differ), hypot %d rows (%d differ), sum %d rows (%d differ)"
      % (len(dist_rows), differs_dist, len(hyp_rows), differs_hyp,
         len(sum_rows), differs_sum))
