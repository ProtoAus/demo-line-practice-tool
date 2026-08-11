// wr_dp.h  --  a decompressed .mtv run body -> the player's path.
//
// This is the half of the extractor that is not exact. Everything below it --
// the container, the LZMA, the JSON -- is a byte being the right byte or not.
// Everything here is a statistical argument about somebody else's netstream,
// and the whole file exists to make that argument checkable.
//
// The explanation below is transplanted, nearly verbatim, from the head of
// wrpath_extract.py. It is the best account of this algorithm that exists and
// the port would be the poorer for leaving it in a file that no longer ships.
//
// ---------------------------------------------------------------------------
// HOW IT WORKS
// ---------------------------------------------------------------------------
//
// A .mtv is Momentum's own container ("MMTV"): a packed binary header, a JSON
// run-stats blob, then the run body compressed with Valve-LZMA (or zstd on the
// biggest files). The body is a Source entity-delta netstream, NOT a flat frame
// array -- there is no public spec for it, and the send-table metadata you would
// need to decode it properly lives in the closed-source game DLL.
//
// We do not decode it properly. We don't have to. m_vecOrigin is networked as
// three raw IEEE-754 float32, so we scan the decompressed body for every bit
// position holding a plausible coordinate triple, and then find the longest
// physically-smooth chain through those candidates.
//
// "Longest smooth chain" is a dynamic program, not a greedy walk. That matters:
// several other fields (velocity, wishVel) also form long smooth chains, the
// stride between player frames is variable -- 216 and 400 bits are both common,
// because Source only sends the props that actually changed -- and a greedy walk
// that takes one wrong turn loses the rest of the run. The DP keeps the best
// predecessor for every candidate, so a single bad edge costs nothing.
//
// Picking the right chain out of the several the DP can find is exact and
// self-validating: the JSON header records the run's own maxHorizontalSpeed, and
// the real path is the chain whose step lengths reproduce it. In practice it
// matches to ~0.01 u/s, which is not something a wrong chain does by accident.
// If no chain matches, we say so instead of writing a plausible-looking lie.
//
// WHAT THIS DELIBERATELY DOESN'T DO
//   - It does not decode the netstream properly. No send tables, no prop flags.
//     It is a pattern-matcher over floats, and it reports when it is unsure.
//   - It does not reach 100% of ticks on every run and is not supposed to. Source
//     delta-compresses: a tick where the player did not move re-sends no origin,
//     and such ticks contribute no path.
//   - It does not use the velocity floats that sit near the origin in the stream.
//     They are not at a reliable offset across runs. Velocity here is a finite
//     difference of positions, which is always available and always consistent.
//
// ---------------------------------------------------------------------------
// NO WINDOWS HEADERS, ON PURPOSE
// ---------------------------------------------------------------------------
//
// This file includes <stddef.h> and nothing else, and the .cpp adds <math.h>,
// <stdlib.h> and <string.h>. It cannot allocate a thread, open a file, read a
// clock or log a line, and that is the point: it is the one part of the DLL
// where "does this produce the same numbers as the reference" is the only
// question, so it is kept small enough that the question has an answer.
//
// Time and cancellation reach it through one function pointer. Files reach it
// not at all -- the caller hands it a decompressed body and takes back an array
// of points.
//
// ---------------------------------------------------------------------------
// THE BIT <-> (phase, q, j) BIJECTION, AND THE SCAN BOUND
// ---------------------------------------------------------------------------
//
// The reference cannot read a float at an arbitrary bit position cheaply --
// that would be one Python call per bit -- so it shifts the WHOLE body right by
// each of the eight bit phases, and then reads the shifted copy as an array of
// byte-aligned u32 at four byte alignments. Inside one such pass, array index j
// is bit position
//
//     b = phase + 8*q + 32*j          phase in [0,8), q in [0,4)
//
// and that map is a bijection: phase = b & 7, q = (b >> 3) & 3, j = b >> 5. So
// the port does not need the eight shifted copies at all -- reading a u32 at
// bit b is one unaligned 8-byte load and a shift, exact because 4 bytes plus 7
// bits is never more than 8. That deletes about nine times the body in
// allocations, which is the single biggest reason this can run inside the game
// where the reference needed its own process.
//
// WHAT IT DOES NOT DELETE IS THE BOUND, AND THE BOUND IS THE SUBTLEST THING IN
// THIS PROJECT. The reference walks j over
//
//     range(skip, (n - q)//4 - 3)      skip = 0x300//4 = 192
//
// so the last admissible j DEPENDS ON q, and skip is applied to j identically
// for every q, which means the first admissible BYTE differs per q. A flat
// bound of the obvious kind -- "any b with b + 96 <= n*8" -- admits candidates
// the reference rejects, and a single extra candidate changes the candidate
// count, the sorted key list, and from there every edge in the dynamic program.
// The failure is not a crash and not an obviously wrong path; it is a slightly
// different one. So the loops below are written in the reference's own shape,
// phase then q then j, and tests\test_dp.exe enumerates the exact admitted set
// for every residue of n and compares it against the range expression.
//
// ---------------------------------------------------------------------------
// WHY THE NORM IS FORTY LINES OF SOMEBODY ELSE'S ARITHMETIC
// ---------------------------------------------------------------------------
//
// The reference calls math.dist and math.hypot. Those are NOT
// sqrt(x*x + y*y + z*z): since 3.8 CPython computes a vector norm with
// lossless power-of-two scaling, exact squaring via fma, Neumaier compensated
// summation and a differential correction of the square root, and it is
// accurate to well under an ulp where the naive form is out by several.
//
// That difference decides things. _peak_horizontal_speed is compared against
// the run's own recorded maxHorizontalSpeed with a tolerance of
// max(1.0, 0.005 * ref), which sounds enormous until you notice what it gates:
// a chain that misses by a hair is BANNED and the search moves on to a
// different chain, so a last-place-digit disagreement does not shift a
// coordinate slightly, it selects a different path through the demo. So
// WrDpVectorNorm below is a transcription of CPython's vector_norm, and
// tests\test_dp.exe checks it against a table of pairs generated by the
// reference interpreter -- and asserts that some of them differ from the naive
// square root, because a table that never distinguishes the two is a test that
// cannot fail.
//
// The reference interpreter is CPython 3.13.9 (tags/v3.13.9:8183fa5, Oct 14
// 2025) [MSC v.1944 64 bit (AMD64)]. That version number is not decoration:
// vector_norm has changed across releases and this is bit-compatible with
// exactly one of them.
//
// Note what is NOT compensated. The dynamic program's own lengths are plain
// sqrt in the reference -- the step at py:559, and the deviation and previous
// step at py:566-567 -- and plain sqrt here. Transcribing those as compensated
// norms would be a "fix" that changed which edges the DP accepts. Where the
// reference is naive, so is this.
//
// py:567 is naive in a second way, and the port deliberately does not copy it.
// It writes `si[0] ** 2` where its neighbours write `sx * sx`, and float ** 2
// is not a multiply: CPython's float_pow has no integer-exponent case, so it
// calls the platform pow(), and UCRT's pow(x, 2.0) is not always the correctly
// rounded square. So that one length really can land one ulp from ours. It was
// measured rather than argued about: over 62.8 million gated edges on real
// demos the two thresholds differed 7,530 times, ALWAYS by exactly one ulp,
// and the gate `dv > threshold` flipped zero times -- it cannot, because no
// double lies strictly between two adjacent doubles, so a flip would need dv
// to be bit-identical to the larger threshold. The closest dv ever came was
// 1.4e-5, nine orders of magnitude clear. Reproducing pow() here would buy
// nothing and would need a second reason to exist on every non-UCRT libm.
//
// AND NOTE WHERE THE COMPENSATION IS NOT IN A NORM AT ALL. Python's builtin
// sum() over floats has used Neumaier compensated summation since 3.12 -- not
// math.fsum, which is exact, but not a plain running total either. It reads
// like the most ordinary line in the file:
//
//     centroid = (sum(xs) / n, sum(ys) / n, sum(zs) / n)      py:721
//     info["path_length"] = sum(math.dist(...) for ...)       py:912
//
// A plain += over nine thousand step lengths landed 3.6e-11 away from that
// one, which is the sort of difference that shows up in a report as a wrong
// digit and nowhere else -- until the centroid version of it moves a segment
// across the ORIGIN_CLUSTER_RADIUS test and a whole leg of somebody's route
// stops being written. WrDpSum below is that algorithm, and the "since 3.12"
// is the point: this code would have been correct against 3.11.
//
// ---------------------------------------------------------------------------
// THE ABORT CALLBACK
// ---------------------------------------------------------------------------
//
// The reference is a throwaway process, so its deadline is a module global and
// its Stop button is TerminateJobObject. Neither survives the move in-process:
// the deadline has to be per-worker (a static would silently give N workers one
// shared clock) and there is no way to kill a thread of the game's own process
// without leaking the CRT heap lock. So every long loop in here polls one
// predicate, supplied by the caller, and unwinds.
//
// Four sites, matching the reference one for one, with the same `where` strings
// because those strings end up in paths\<map>\_failed.txt and a record with a
// different reason reads as a different failure:
//
//     "the candidate scan"       "the chain search"
//     "the derivative sweep"     "chain identification"
//
// One deliberate difference: the scan polls per (phase, q) rather than per
// phase -- 32 checks instead of 8 -- because that loop is 32 passes over a body
// that can be megabytes and Stop should not have to wait out an eighth of it.
// This cannot affect a comparison. An abort can only make a run fail EARLIER
// than the reference's would; it can never change a value that gets computed.

#ifndef WR_DP_H
#define WR_DP_H

#include <stddef.h>

// ---------------------------------------------------------------------------
// The tunables, and every one of them was measured
// ---------------------------------------------------------------------------

// How far from the origin a real world coordinate can be. NOT the 16384 every
// Source reference quotes -- Strata's maps are bigger, and surf_colin_blaster_
// 69000 reaches -31295 on X. At 16384 this filter chopped that map's origin
// stream into fragments and 66 of its 141 demos could not be extracted at all.
// Keep in sync with WR_WORLD_LIMIT in wr_common.h and WORLD_LIMIT in the
// reference.
#define WR_DP_WORLD_LIMIT 65536.0

// Largest plausible movement between consecutive ticks, in world units. The
// fastest runs on disk peak near 4900 u/s, which is ~74 units at a 0.015 s
// tick; this leaves generous headroom for defrag and rocket-jump speeds.
#define WR_DP_MAX_STEP 200.0

// Bit-gap window between consecutive player frames. A frame cannot begin before
// the 96 bits the origin itself occupies; the upper bound only has to cover the
// worst interleaving of other entities' deltas, and is also derived per-file
// from the average bits-per-tick.
#define WR_DP_GAP_MIN 96
#define WR_DP_GAP_MAX_FLOOR 3000

// Smoothness gate. Consecutive step vectors may differ by at most
// TOL_FLOOR + TOL_FRAC * (longer of the two steps). This is what stops the DP
// stitching together candidates that merely happen to be nearby.
#define WR_DP_TOL_FLOOR 10.0
#define WR_DP_TOL_FRAC 0.80

// float32 exponent window, used as an integer prefilter. A magnitude in
// [~1e-3, 32768) has a biased exponent in [117, 141]. Testing raw bits throws
// away ~98% of positions before any float conversion happens.
#define WR_DP_EXP_LO 117
#define WR_DP_EXP_HI 141

// How closely a chain's step lengths must reproduce the run's own recorded
// maxHorizontalSpeed to be accepted as the player's path. A correct chain lands
// within ~0.01 u/s; this is loose enough for a missing tick here or there.
#define WR_DP_MATCH_ABS 1.0
#define WR_DP_MATCH_REL 0.005

#define WR_DP_MIN_CHAIN 32
#define WR_DP_MAX_IDENTIFY_ROUNDS 14

// Vertical-velocity test (see WrDpOriginScore in the .cpp). A position chain
// scores ~0.75 on real data; velocity and projectile chains score under 0.09,
// so half is a comfortable line. MIN_STEPS guards against the offset search
// finding a coincidence on a handful of samples.
#define WR_DP_DERIV_MIN_FRACTION 0.5
#define WR_DP_DERIV_MIN_STEPS 10
#define WR_DP_DERIV_TOL_ABS 25.0
#define WR_DP_DERIV_TOL_REL 0.05

// The offset search takes the best of ~500 offsets, so a small sample can throw
// up a coincidence. Rather than demand a large sample outright -- short stage
// runs simply do not have many airborne ticks -- scale the bar to the evidence.
#define WR_DP_DERIV_SMALL_SAMPLE 25
#define WR_DP_DERIV_SMALL_FRACTION 0.7

// Upper bound on how many steps the offset sweep looks at. Bounds a cost that
// is otherwise quadratic in run length.
#define WR_DP_DERIV_MAX_SAMPLES 400

// Knowing which stream is the origin does not guarantee enough of it survived
// to be worth drawing. Below this, report a failure instead of writing a stub.
#define WR_DP_DERIV_MIN_COVERAGE 0.25

// Coverage below this is reported as low-confidence even when the speed oracle
// passed. It passing only proves the leg we found is real, not that we found
// the whole run -- which is exactly how two main-track runs got written at 8.8%
// and 3.9% coverage with no warning at all. Read by wr_demo.cpp, which is where
// the flags are decided.
#define WR_DP_MIN_COVERAGE_CONFIDENT 0.60

// When the speed check fails outright, a chain must still span at least this
// fraction of the run's ticks before it is kept as a low-confidence path.
#define WR_DP_LOW_CONFIDENCE_MIN_COVERAGE 0.50

// Teleports. A staged map teleports the player between legs and the smoothness
// gate cannot step across that -- by design, since allowing a 10000-unit step
// is exactly how a chain wanders off into unrelated data. So the DP is run
// again over the bit ranges the accepted chains do not cover.
#define WR_DP_MIN_SEGMENT 24
#define WR_DP_MAX_SEGMENTS 24

// A recovered segment must not imply a speed the run never reached. This is
// what stops a velocity or wishvel field being mistaken for a leg of the path.
#define WR_DP_SEGMENT_SPEED_SLACK 0.25

// ...except while the player is idle, when the velocity field sits at nearly
// zero and forms a long, perfectly smooth, slow chain the speed gate is happy
// with. It gives itself away by being a stationary cloud at the world origin:
// an 18-minute practice demo produced one 1245 samples long, spanning 90 units,
// centred 4 units from (0,0,0).
#define WR_DP_SEGMENT_MIN_EXTENT 256.0
#define WR_DP_ORIGIN_CLUSTER_RADIUS 512.0

// Where the scan starts. The first 0x300 bytes are the netstream's own preamble
// and hold no player frames.
#define WR_DP_SCAN_START_BYTE 0x300

// ---------------------------------------------------------------------------
// Cancellation
// ---------------------------------------------------------------------------

// Returned by the caller's predicate. Anything non-zero stops the run; the two
// values are distinguished because only one of them is a failure worth
// recording. A demo the user stopped has not been shown to be unextractable.
#define WR_DP_GO 0
#define WR_DP_STOP_TIMEOUT 1
#define WR_DP_STOP_CANCEL 2

typedef int (*WrDpAbort)(void *user);

// ---------------------------------------------------------------------------
// Candidates
// ---------------------------------------------------------------------------

// 16 bytes, and the u32 bit position is what caps a body at 512 MB -- which is
// why wr_mtv.h refuses one that size rather than letting this wrap.
//
// The coordinates are stored as float because they ARE float32: they came out
// of the stream as four bytes each and every arithmetic step widens them to
// double at use, exactly as struct.unpack does. Storing doubles would be eight
// bytes of zeros per component on a million-entry array.
struct WrDpCand
{
    unsigned int bit;
    float x, y, z;
};

// Every bit position holding a plausible (x, y, z) float triple, SORTED BY BIT.
// The reference builds a dict and sorts its keys; the bijection above means no
// two (phase, q, j) can collide, so this is the same set.
//
// *out is malloc'd and the caller free()s it.
//
// False means either the caller's predicate fired -- *stop then holds its code
// and `err` is untouched, because only the caller knows how long the deadline
// was and therefore what the message should say -- or an allocation failed, in
// which case *stop is 0 and `err` says so.
bool WrDpScan(const unsigned char *body, size_t len, unsigned int startByte,
              WrDpCand **out, int *countOut, int *stop,
              WrDpAbort abort, void *user, char *err, int errCap);

// ---------------------------------------------------------------------------
// The result
// ---------------------------------------------------------------------------

// Doubles, because they are doubles in the reference and two of the six are
// arithmetic rather than data: vx/vy/vz are a central difference that only
// becomes float32 when it is written to the file. Rounding them early would
// change what anchor_markers matches on.
struct WrDpPoint
{
    double x, y, z;
    double vx, vy, vz;
};

// Everything the run decided, which is also exactly the --dump-info row. Kept
// as one struct so that the file's flags, the panel's progress line and the
// dump cannot disagree about what happened -- the reference split finish_info
// out of process_one for the same reason.
struct WrDpInfo
{
    int candidates;
    unsigned int ticks;
    int rounds;
    bool confident;
    char identifiedBy[32];      // "" is the reference's None

    bool haveDeriv;             // was the derivative test ever consulted
    double derivRate;
    int derivOffset;

    int segments;
    int firstSegment;

    bool haveRef;               // the JSON carried a maxHorizontalSpeed
    double refMaxHoriz;
    double chainMaxHoriz;
    double matchError;

    int samples;
    double coverage;
    double pathLength;
    double scanSeconds;         // filled in by the caller; see wr_demo.cpp
};

struct WrDpResult
{
    WrDpPoint *points;
    int pointCount;
    WrDpInfo info;

    // --dump-chain only, and NULL unless WrDpArgs::keepDetail asked for them.
    // A 30000-point chain is a quarter of a megabyte of bit positions that
    // nothing in the ordinary path would ever read.
    unsigned int *chain;
    int chainCount;
    unsigned int *segBits;      // every segment's bits, concatenated
    int *segOff;                // segment i is segBits[segOff[i] .. +segLen[i])
    int *segLen;
    int segCount;
};

struct WrDpArgs
{
    const unsigned char *body;
    size_t bodyLen;

    double tickInterval;
    unsigned int ticks;

    // The run's own recorded maxHorizontalSpeed, when the JSON had one. This is
    // the entire oracle: `haveRef` false is the reference's `ref is None`, and
    // it takes the longest chain without arguing.
    bool haveRef;
    double refMaxHoriz;

    WrDpAbort abort;
    void *abortUser;

    // Only used to word the timeout message, which has to match the reference's
    // because it is written into the failure record. This file does not read a
    // clock; the caller's predicate is what knows the time.
    double timeoutSeconds;

    bool keepDetail;
};

// The whole of extract_path. False having written `err`, which is the string
// that reaches _failed.txt.
//
// `cancelled` is set instead when the caller's predicate returned
// WR_DP_STOP_CANCEL: the user pressed Stop, nothing was learned about this
// demo, and recording a failure for it would skip it forever.
bool WrDpExtract(const WrDpArgs *args, WrDpResult *out, bool *cancelled,
                 char *err, int errCap);

void WrDpFree(WrDpResult *r);

// ---------------------------------------------------------------------------
// The arithmetic, exposed so tests\test_dp.exe can drive it directly
// ---------------------------------------------------------------------------

// CPython's math.dist and math.hypot. See the essay above for why these are not
// one line each.
double WrDpDist3(const double a[3], const double b[3]);
double WrDpHypot2(double dx, double dy);

// CPython's vector_norm. `vec` holds the ABSOLUTE values, `max` the largest of
// them, and this MUTATES vec on the subnormal path exactly as the original
// does -- transcribed rather than tidied, so that a future reader can diff it
// against Modules/mathmodule.c and see nothing.
double WrDpVectorNorm(double *vec, int n, double max);

// Python's builtin sum() over floats, which since 3.12 is Neumaier compensated
// summation and not a running total. An accumulator rather than a function over
// an array, because both call sites in the reference are generators over
// something that is expensive to materialise:
//
//     WrDpSum s;  WrDpSumInit(&s);
//     for (...) WrDpSumAdd(&s, x);
//     double total = WrDpSumEnd(&s);
//
// WrDpSumEnd folds the compensation in only when it is non-zero and finite,
// which is CPython's own guard: adding a zero would turn a -0.0 result into
// +0.0, and adding an infinity would turn an overflowed total into a NaN.
struct WrDpSum { double total, c; };
void WrDpSumInit(WrDpSum *s);
void WrDpSumAdd(WrDpSum *s, double x);
double WrDpSumEnd(const WrDpSum *s);

// A float32 read at an arbitrary BIT position, or false when the reference's
// bound `(p >> 3) + 4 > n` refuses it. Exposed because pinning this against the
// shifted-buffer original at every bit position of a fixture -- including the
// last eight bytes, where the reference's to_bytes() zero fill has to be
// reproduced -- is the first section of test_dp and the foundation of the rest.
bool WrDpFloatAt(const unsigned char *body, size_t len, long long bitPos,
                 double *out);

#endif // WR_DP_H
