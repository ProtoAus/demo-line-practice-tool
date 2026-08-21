// velprobe.cpp  --  does the demo stream carry the game's own velocity, and
//                   could we read it instead of differencing positions?
//
//   tests\velprobe.exe <game dir or one .mtv> [--limit N]
//
// THE QUESTION, AND WHY IT IS WORTH ASKING
//
// Every velocity in a .wrpath is a CENTRAL DIFFERENCE of two recovered
// positions -- wr_dp.cpp:1249 says so, and says why:
//
//     "Velocity by central difference. The stream does carry velocity floats
//      near the origin, but not at a reliable offset across runs, so we do not
//      use them."
//
// A central difference is an average across two ticks. It is excellent under
// constant acceleration -- free flight reads gravity back to 0.1% -- and it
// smears anything that happens inside one tick, which is exactly what a board
// is. So if the game's own per-tick velocity is sitting in the file, reading it
// would make the demo half tick-exact rather than tick-averaged, and
// FindEfficiency's +-4 point window exists only to reject the noise of
// differencing an already-differenced quantity.
//
// AND THE EXTRACTOR ALREADY FINDS PART OF IT. OriginScore (wr_dp.cpp:635) tells
// a position chain from a velocity chain by checking whether the chain's own
// VERTICAL velocity is sitting at a fixed relative bit offset beside it -- and
// on a real chain it matches on 74.6% of steps. That offset is recorded, as
// WrDpInfo::derivOffset. It is found, used to make a decision, and thrown away.
//
// THE HYPOTHESIS THIS EXISTS TO TEST
//
// wr_dp.cpp:628 rejects a three-component match on the grounds that it "scores
// 0.19 even on a chain known to be correct", and concludes only vz is reliably
// present. But that test compares the stream's floats against a CENTRAL
// DIFFERENCE, and a central difference is not equally good in all three
// components:
//
//     vz  under gravity alone the trajectory is a parabola, and a central
//         difference of a quadratic is EXACT at the midpoint. So vz agrees.
//     vx  air acceleration is horizontal and changes every tick, so the
//     vy  central difference is genuinely wrong there -- by the amount the
//         player accelerated.
//
// If that is what happened, 0.19 measured the ESTIMATOR and not the data, the
// floats are all three there, and reading them is available. If it is not, vx
// and vy really are absent or moving, and the comment is right.
//
// This program changes nothing. It reads demos, runs the real extractor, and
// prints match rates.

#include "wr_dp.h"
#include "wr_mtv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>

// The same tolerance OriginScore uses, so a match here means what a match means
// there: 25 u/s, or 5% of the figure, whichever is larger.
#define TOL_ABS 25.0
#define TOL_REL 0.05

static bool Close(double a, double b)
{
    double rel = TOL_REL * fabs(b);
    double tol = TOL_ABS > rel ? TOL_ABS : rel;
    return fabs(a - b) <= tol;
}

// ---------------------------------------------------------------------------

// THE OFFSET HAS TO BE FOUND HERE, and finding out why is half the result.
//
// WrDpInfo::derivOffset is filled in only when the speed oracle FAILS -- see
// wr_dp.cpp:975, `if (identN == 0)`. On a demo whose chain covers most of the
// run the oracle passes on the first attempt and the derivative test is never
// consulted at all. Measured: 0 of the first 150 demos in this library carried
// an offset. So the extractor's "not at a reliable offset across runs" is a
// claim about a code path most runs never enter, and this program has to sweep
// for the offset itself before it can ask anything about the floats.
//
// Sweeping SEPARATELY FOR EACH COMPONENT is what makes this decisive. If vx's
// own best offset lands exactly 64 bits before vz's, the velocity is a
// contiguous float triple and all three are present. If vz has a sharp peak and
// vx has none anywhere, the floats really are absent and the comment stands.
#define SWEEP_SAMPLES 200
#define SWEEP_MIN_MAG 60.0      // OriginScore's guard: near-zero matches anything

struct Tally
{
    long long steps;
    long long hitCentral[3];    // stream float vs the central difference
    long long hitForward[3];    // stream float vs a one-sided difference
    long long hitControl[3];    // ...vs ANOTHER step's velocity: the floor
    long long readable[3];      // the bit position was inside the body at all
};

static Tally g_all;
static int g_demos = 0, g_withDeriv = 0, g_extracted = 0, g_failed = 0;
static int g_layoutOk = 0, g_layoutBad = 0;
static int g_limit = 0;

// Per-demo best offsets, so "reliable across runs" can be answered rather than
// repeated.
static int g_contiguous = 0, g_notContiguous = 0;
static double g_bestRate[3] = { 0.0, 0.0, 0.0 };
static int g_bestRateN = 0;

// The z float of a candidate, read back from its bit position. Used once per
// demo to confirm the (x, y, z) triple really is three consecutive float32s at
// bit, bit+32, bit+64 -- everything below rests on that and it costs one read
// to check rather than assume.
static bool LayoutHolds(const unsigned char *body, size_t len,
                        unsigned int bit, double z)
{
    double got = 0.0;
    if (!WrDpFloatAt(body, len, (long long)bit + 64, &got))
        return false;
    return fabs(got - z) <= 1e-3;
}

static void ProbeOne(const char *path)
{
    g_demos++;

    size_t len = 0;
    char err[256];
    unsigned char *file = WrMtvReadFile(path, &len, err, sizeof(err));
    if (!file)
        return;

    WrMtvHeader h;
    if (!WrMtvParseHeader(file, len, &h, err, sizeof(err)))
    {
        free(file);
        return;
    }

    size_t blen = 0;
    unsigned char *body = WrMtvBody(file, len, &h, &blen, err, sizeof(err));
    free(file);
    if (!body)
        return;

    WrDpArgs a;
    memset(&a, 0, sizeof(a));
    a.body = body;
    a.bodyLen = blen;
    a.tickInterval = (double)h.tickInterval;
    a.ticks = h.ticks;
    a.keepDetail = true;        // segBits is the whole point of this program

    WrDpResult r;
    memset(&r, 0, sizeof(r));
    bool cancelled = false;
    if (!WrDpExtract(&a, &r, &cancelled, err, sizeof(err)))
    {
        g_failed++;
        free(body);
        return;
    }
    g_extracted++;

    if (!r.segBits || r.pointCount < 64 || r.segCount <= 0)
    {
        WrDpFree(&r);
        free(body);
        return;
    }

    const double dt = (double)h.tickInterval;

    // segBits is every segment's bits concatenated in the same order points[]
    // was filled, so index j indexes both.
    int total = 0;
    for (int s = 0; s < r.segCount; s++)
        total += r.segLen[s];
    if (total != r.pointCount)
    {
        // Should not happen; if it does, the index equivalence this rests on
        // does not hold and the demo is skipped rather than guessed at.
        WrDpFree(&r);
        free(body);
        return;
    }

    if (LayoutHolds(body, blen, r.segBits[0], r.points[0].z))
        g_layoutOk++;
    else
        g_layoutBad++;

    // Gather the sample steps: consecutive points inside one segment, whose bit
    // gap is one frame's worth, subsampled to keep the sweep affordable.
    static unsigned int sBit[SWEEP_SAMPLES];
    static double sCen[SWEEP_SAMPLES][3], sFwd[SWEEP_SAMPLES][3];
    int ns = 0;

    // A frame's worth of bits, as the median gap. The sweep's range comes from
    // this, exactly as OriginScore's does.
    double gapSum = 0.0;
    int gapN = 0;
    int base = 0;
    for (int s = 0; s < r.segCount; s++)
    {
        const int n = r.segLen[s];
        for (int k = 0; k + 1 < n; k++)
        {
            const long long g = (long long)r.segBits[base + k + 1]
                              - (long long)r.segBits[base + k];
            if (g > 0 && g < 100000) { gapSum += (double)g; gapN++; }
        }
        base += n;
    }
    if (gapN < 32)
    {
        WrDpFree(&r);
        free(body);
        return;
    }
    const long long frameBits = (long long)(gapSum / gapN + 0.5);
    const long long lim = (long long)(frameBits * 1.2);

    // Collect candidate steps, then thin them evenly.
    static int cand[65536];
    int nCand = 0;
    base = 0;
    for (int s = 0; s < r.segCount; s++)
    {
        const int n = r.segLen[s];
        for (int k = 1; k + 1 < n && nCand < 65536; k++)
        {
            const int j = base + k;
            const WrDpPoint *p1 = &r.points[j];

            // OriginScore's guard, applied PER COMPONENT and not to the
            // magnitude. Getting this wrong is what the first version of this
            // program did, and it read 82% for all three: with a tolerance of
            // max(25, 5%) a component near zero matches any small float, and a
            // stream is full of small floats. Requiring all three to be large
            // keeps one shared sample set, which is what makes the three
            // offsets comparable.
            if (fabs(p1->vx) < SWEEP_MIN_MAG) continue;
            if (fabs(p1->vy) < SWEEP_MIN_MAG) continue;
            if (fabs(p1->vz) < SWEEP_MIN_MAG) continue;
            const long long g = (long long)r.segBits[j + 1]
                              - (long long)r.segBits[j];
            if (g <= 0 || (double)g > (double)frameBits * 1.6) continue;
            cand[nCand++] = j;
        }
        base += n;
    }
    if (nCand < 32)
    {
        WrDpFree(&r);
        free(body);
        return;
    }

    const double stride = nCand > SWEEP_SAMPLES
                        ? (double)nCand / (double)SWEEP_SAMPLES : 1.0;
    for (int i = 0; i < SWEEP_SAMPLES && ns < SWEEP_SAMPLES; i++)
    {
        const int idx = (int)((double)i * stride);
        if (idx >= nCand) break;
        const int j = cand[idx];
        const WrDpPoint *p1 = &r.points[j];
        const WrDpPoint *p2 = &r.points[j + 1];
        sBit[ns] = r.segBits[j];
        sCen[ns][0] = p1->vx; sCen[ns][1] = p1->vy; sCen[ns][2] = p1->vz;
        sFwd[ns][0] = (p2->x - p1->x) / dt;
        sFwd[ns][1] = (p2->y - p1->y) / dt;
        sFwd[ns][2] = (p2->z - p1->z) / dt;
        ns++;
    }
    if (ns < 32)
    {
        WrDpFree(&r);
        free(body);
        return;
    }
    g_withDeriv++;
    g_all.steps += ns;

    // The sweep. Both directions, because nothing says the velocity follows the
    // origin in the send table rather than preceding it.
    long long bestOff[3] = { 0, 0, 0 };
    int bestHit[3] = { -1, -1, -1 };
    int bestFwd[3] = { 0, 0, 0 };
    int bestCtl[3] = { -1, -1, -1 };

    // THE CONTROL, and this program is worthless without it. A best-of sweep
    // over a few thousand offsets is maximising over a large space, so it can
    // never return zero and a high number proves nothing on its own. The
    // control asks the same sweep the same question with the CORRESPONDENCE
    // broken: each step is matched against another step's velocity, so the
    // values have the identical distribution and only the pairing is wrong.
    // Whatever rate that reaches is this sweep's own floor, measured rather
    // than assumed.
    const int shift = ns / 2;

    for (long long o = -lim; o <= lim; o++)
    {
        int hitC[3] = { 0, 0, 0 }, hitF[3] = { 0, 0, 0 }, hitX[3] = { 0, 0, 0 };
        for (int k = 0; k < ns; k++)
        {
            double w = 0.0;
            if (!WrDpFloatAt(body, blen, (long long)sBit[k] + o, &w))
                continue;
            const int q = (k + shift) % ns;
            for (int c = 0; c < 3; c++)
            {
                if (Close(w, sCen[k][c])) hitC[c]++;
                if (Close(w, sFwd[k][c])) hitF[c]++;
                if (Close(w, sCen[q][c])) hitX[c]++;
            }
        }
        for (int c = 0; c < 3; c++)
        {
            if (hitC[c] > bestHit[c])
            {
                bestHit[c] = hitC[c];
                bestOff[c] = o;
                bestFwd[c] = hitF[c];
            }
            if (hitX[c] > bestCtl[c])
                bestCtl[c] = hitX[c];
        }
    }

    for (int c = 0; c < 3; c++)
    {
        g_all.readable[c] += ns;
        g_all.hitCentral[c] += bestHit[c] > 0 ? bestHit[c] : 0;
        g_all.hitForward[c] += bestFwd[c];
        g_all.hitControl[c] += bestCtl[c] > 0 ? bestCtl[c] : 0;
        g_bestRate[c] += (double)(bestHit[c] > 0 ? bestHit[c] : 0) / ns;
    }
    g_bestRateN++;

    // The structural question: is the velocity a contiguous float triple, the
    // way the origin is? If so vx sits 64 bits before vz and vy 32.
    if (bestOff[0] == bestOff[2] - 64 && bestOff[1] == bestOff[2] - 32)
        g_contiguous++;
    else
        g_notContiguous++;

    WrDpFree(&r);
    free(body);
}

// ---------------------------------------------------------------------------

static void Walk(const char *dir)
{
    char pat[1024];
    _snprintf_s(pat, sizeof(pat), _TRUNCATE, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (g_limit && g_demos >= g_limit) break;
        if (fd.cFileName[0] == '.') continue;
        char full[1024];
        _snprintf_s(full, sizeof(full), _TRUNCATE, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { Walk(full); continue; }
        size_t L = strlen(fd.cFileName);
        if (L < 5 || _stricmp(fd.cFileName + L - 4, ".mtv") != 0) continue;
        ProbeOne(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

int main(int argc, char **argv)
{
    const char *root = NULL;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc)
            g_limit = atoi(argv[++i]);
        else
            root = argv[i];
    }
    if (!root)
    {
        printf("usage: velprobe.exe <dir with .mtv demos, or one .mtv> "
               "[--limit N]\n");
        return 2;
    }

    printf("\n=== does the stream carry the velocity? ===\n\n");

    DWORD at = GetFileAttributesA(root);
    if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY))
        Walk(root);
    else
        ProbeOne(root);

    printf("%d demos seen, %d extracted, %d failed, %d swept\n",
           g_demos, g_extracted, g_failed, g_withDeriv);
    printf("the ORIGIN was three consecutive float32s on %d demos and not "
           "on %d\n", g_layoutOk, g_layoutBad);
    if (g_withDeriv)
        printf("the best offsets formed a contiguous velocity triple on %d "
               "demos and not on %d\n", g_contiguous, g_notContiguous);

    if (!g_all.steps)
    {
        printf("\nnothing to report -- no demo got far enough.\n"
               "this needs real .mtv files; see the header.\n\n");
        return 1;
    }

    printf("\n%lld steps probed, at each component's OWN best offset\n\n",
           g_all.steps);
    printf("  %-4s  %-24s  %-22s  %s\n", "",
           "matches the central diff", "matches a forward diff",
           "CONTROL: wrong pairing");
    static const char *kName[3] = { "vx", "vy", "vz" };
    for (int c = 0; c < 3; c++)
    {
        const long long rd = g_all.readable[c];
        if (!rd) { printf("  %-4s  nothing probed\n", kName[c]); continue; }
        printf("  %-4s  %6.1f%%                   %6.1f%%                 "
               "  %6.1f%%\n",
               kName[c],
               100.0 * g_all.hitCentral[c] / rd,
               100.0 * g_all.hitForward[c] / rd,
               100.0 * g_all.hitControl[c] / rd);
    }
    if (g_bestRateN)
    {
        printf("\n  per-demo mean of each component's best rate: "
               "vx %.1f%%  vy %.1f%%  vz %.1f%%\n",
               100.0 * g_bestRate[0] / g_bestRateN,
               100.0 * g_bestRate[1] / g_bestRateN,
               100.0 * g_bestRate[2] / g_bestRateN);
    }

    // HOW TO READ THIS. Every figure is a BEST-OF sweep over a couple of
    // thousand offsets, so none of them can be zero and none of them is
    // evidence on its own -- a best-of-N over noise still finds a maximum.
    // What separates a real field from a coincidence is the same thing
    // OriginScore relies on: a real one matches on most steps at ONE offset,
    // and a coincidence matches on a handful.
    //
    // So the two things worth reading are the SIZE of the rate -- OriginScore
    // measured 74.6% on a chain known to carry the field, against 1.9-8.2% for
    // everything else in the same file -- and whether the three best offsets
    // line up into a contiguous triple. Both, or neither.
    printf("\n  a best-of sweep over a couple of thousand offsets cannot\n"
           "  return zero; the question is whether the rate is OriginScore's\n"
           "  74.6%% or its 1.9-8.2%% floor, and whether the offsets agree.\n\n");
    return 0;
}
