// test_dp.cpp  --  the part of the port that a .wrpath cannot show you.
//
// Everything below wr_dp.cpp has a byte oracle: --dump-body over six thousand
// demos either matches or it does not. Everything at this level is arithmetic
// on floats, where a difference has to be argued about -- so the argument is
// made here, against inputs whose right answer is known independently of the
// code under test.
//
// Four things, in descending order of how much trouble they can cause.
//
// 1. THE SCAN BOUND. The reference walks j over range(skip, (n - q)//4 - 3),
//    so the last admissible index depends on q, and skip is applied to j
//    identically for every q, which means the first admissible BYTE differs
//    per q. A flat bound of the obvious kind admits candidates the reference
//    rejects, and one extra candidate changes the candidate count, the sorted
//    key list, and from there every edge in the dynamic program. The failure
//    is not a crash and not an obviously wrong path -- it is a slightly
//    different one.
//
//    A body of nothing but zero bytes makes this exhaustively checkable: a
//    zero word is plausible by the reference's own test and decodes to 0.0,
//    which is inside the world limit, so EVERY admitted bit position becomes a
//    candidate and the candidate set IS the admitted set. Compare it against
//    the range expression, for every residue of n.
//
// 2. THE COMPENSATED NORM. math.dist and math.hypot are CPython's vector_norm
//    -- lossless scaling, exact squaring, Neumaier summation, a differential
//    correction of the root -- and not sqrt(x*x + y*y + z*z). Checked against
//    a table of bit patterns the reference interpreter produced, half of them
//    searched for because they distinguish the two forms. See fixture_norm.h.
//
// 3. THE BIT READER. The reference cannot read a float at an arbitrary bit
//    position cheaply, so it shifts the whole body by each of eight phases and
//    reads the copies byte-aligned. This does one unaligned load and a shift.
//    The two agree everywhere, INCLUDING the last few bytes, where the
//    reference's to_bytes() zero-fills above the end of the buffer and this
//    has to reproduce that rather than read whatever follows.
//
// 4. END TO END, on a netstream built here with an answer known by
//    construction: a helix planted at a chosen bit phase in a body of filler
//    that cannot be mistaken for coordinates. That covers the dynamic program,
//    the origin scoring, the reassembly and the central difference in one
//    piece, and it is also where the 99.5th-percentile step cap is pinned --
//    by planting one deliberately doubled step and requiring that the reported
//    max speed does not see it.
//
// WHAT THIS DOES NOT PIN, so nobody has to wonder: the exact chain the DP picks
// on real data. That is what tests\parity.ps1 is for, and there is no
// substitute for it -- six thousand demos against the reference, byte for byte.
//
// Build:  tests\build.bat
// Run:    tests\test_dp.exe

#include "wr_dp.h"
#include "fixture_norm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

static double AsDouble(unsigned long long b)
{
    double d;
    memcpy(&d, &b, 8);
    return d;
}

static unsigned long long AsBits(double d)
{
    unsigned long long b;
    memcpy(&b, &d, 8);
    return b;
}

// The reference's own reader, written the reference's own way: shift the whole
// buffer right by `ph` bits, keeping n bytes, then read four bytes at p >> 3.
// Bits above the end of the buffer are zero, which is what to_bytes(n) does.
static bool ShiftedRead(const unsigned char *buf, size_t n, long long p,
                        double *out)
{
    if (p < 0 || (size_t)((unsigned long long)p >> 3) + 4 > n)
        return false;

    const int ph = (int)(p & 7);
    const size_t at = (size_t)((unsigned long long)p >> 3);

    unsigned int w = 0;
    for (int k = 0; k < 4; k++)
    {
        // Byte `at + k` of the shifted copy.
        const size_t i = at + (size_t)k;
        unsigned int lo = i < n ? buf[i] : 0u;
        unsigned int hi = (i + 1) < n ? buf[i + 1] : 0u;
        unsigned int byte = ph ? ((lo >> ph) | ((hi << (8 - ph)) & 0xFFu))
                               : lo;
        w |= (byte & 0xFFu) << (8 * k);
    }
    float f;
    memcpy(&f, &w, 4);
    *out = (double)f;
    return true;
}

// A candidate's bit position is phase + 8q + 32j, and nothing else can produce
// it: phase = b & 7, q = (b >> 3) & 3, j = b >> 5.
static bool BitIsAdmitted(size_t n, unsigned int b, long long skip)
{
    const long long phase = b & 7u;
    const long long q = (b >> 3) & 3u;
    const long long j = b >> 5;
    (void)phase;
    const long long count = (long long)((n - (size_t)q) / 4);
    if (count < 8)
        return false;
    return j >= skip && j < count - 3;
}

int main(void)
{
    printf("\n=== wrlines scan, norm and dynamic program ===\n");

    // -----------------------------------------------------------------------
    printf("\nthe scan admits exactly the bit positions the reference does\n");
    {
        // Zero bytes: every word is plausible and decodes to 0.0, so the
        // candidate set is the admitted set and nothing else.
        const long long skip = WR_DP_SCAN_START_BYTE / 4;
        int checkedSizes = 0;
        bool allMatch = true;
        long long totalCands = 0;

        for (size_t n = 1000; n <= 1007; n++)     // every residue mod 4
        {
            unsigned char *body = (unsigned char *)calloc(n, 1);
            WrDpCand *c = NULL;
            int got = 0, stop = 0;
            char err[128] = "";
            if (!WrDpScan(body, n, WR_DP_SCAN_START_BYTE, &c, &got, &stop,
                          NULL, NULL, err, sizeof(err)))
            {
                allMatch = false;
                free(body);
                continue;
            }

            // Every candidate is admitted, and they arrive sorted.
            for (int i = 0; i < got; i++)
            {
                if (!BitIsAdmitted(n, c[i].bit, skip))
                    allMatch = false;
                if (i > 0 && c[i].bit <= c[i - 1].bit)
                    allMatch = false;
                if (c[i].x != 0.0f || c[i].y != 0.0f || c[i].z != 0.0f)
                    allMatch = false;
            }

            // And every admitted position is a candidate. Counted rather than
            // searched: the two sets are the same size and one contains the
            // other, so they are equal.
            long long want = 0;
            for (int phase = 0; phase < 8; phase++)
                for (int q = 0; q < 4; q++)
                {
                    const long long count = (long long)((n - (size_t)q) / 4);
                    if (count < 8)
                        continue;
                    for (long long j = skip; j < count - 3; j++)
                        want++;
                }
            if (want != got)
                allMatch = false;

            totalCands += got;
            checkedSizes++;
            free(c);
            free(body);
        }
        printf("     %d body sizes, %lld candidate positions enumerated\n",
               checkedSizes, totalCands);
        Check(checkedSizes == 8 && allMatch,
              "candidates == { phase + 8q + 32j : skip <= j < (n-q)/4 - 3 }");

        // The bound really does depend on q, which is the whole trap: if it
        // did not, all four would admit the same number of j.
        const size_t n = 1001;
        int per[4] = {0, 0, 0, 0};
        for (int q = 0; q < 4; q++)
        {
            const long long count = (long long)((n - (size_t)q) / 4);
            per[q] = (int)(count - 3 - (WR_DP_SCAN_START_BYTE / 4));
        }
        Check(!(per[0] == per[1] && per[1] == per[2] && per[2] == per[3]),
              "the last admissible j is not the same for every q");

        // A body too short for the reference to look at at all.
        WrDpCand *c = NULL;
        int got = -1, stop = 0;
        char err[128] = "";
        unsigned char tiny[32] = {0};
        Check(WrDpScan(tiny, sizeof(tiny), WR_DP_SCAN_START_BYTE, &c, &got,
                       &stop, NULL, NULL, err, sizeof(err)) && got == 0,
              "a body under 64 bytes yields nothing, and is not an error");
        free(c);
    }

    // -----------------------------------------------------------------------
    printf("\nthe compensated norm, against the reference interpreter\n");
    {
        const int nd = (int)(sizeof(kNormDist) / sizeof(kNormDist[0]));
        int bad = 0, naiveDisagrees = 0;
        for (int i = 0; i < nd; i++)
        {
            double a[3], b[3];
            for (int k = 0; k < 3; k++)
            {
                a[k] = AsDouble(kNormDist[i].a[k]);
                b[k] = AsDouble(kNormDist[i].b[k]);
            }
            const double got = WrDpDist3(a, b);
            if (AsBits(got) != kNormDist[i].want)
                bad++;

            const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
            const double naive = sqrt(dx * dx + dy * dy + dz * dz);
            if (AsBits(naive) != kNormDist[i].want)
                naiveDisagrees++;
        }
        printf("     %d rows, %d of which naive sqrt gets wrong\n",
               nd, naiveDisagrees);
        Check(bad == 0, "math.dist agrees to the last bit on every row");
        Check(naiveDisagrees == WR_NORM_DIST_DIFFERS && naiveDisagrees > 0,
              "and the table can tell the two forms apart at all");

        const int nh = (int)(sizeof(kNormHypot) / sizeof(kNormHypot[0]));
        bad = 0;
        naiveDisagrees = 0;
        for (int i = 0; i < nh; i++)
        {
            const double x = AsDouble(kNormHypot[i].x);
            const double y = AsDouble(kNormHypot[i].y);
            if (AsBits(WrDpHypot2(x, y)) != kNormHypot[i].want)
                bad++;
            if (AsBits(sqrt(x * x + y * y)) != kNormHypot[i].want)
                naiveDisagrees++;
        }
        printf("     %d hypot rows, %d of which naive sqrt gets wrong\n",
               nh, naiveDisagrees);
        Check(bad == 0, "math.hypot agrees to the last bit on every row");
        Check(naiveDisagrees == WR_NORM_HYPOT_DIFFERS && naiveDisagrees > 0,
              "and that table distinguishes them too");

        // The arms random inputs will not reach.
        double zero[3] = {0.0, 0.0, 0.0};
        Check(WrDpDist3(zero, zero) == 0.0, "two identical points are 0.0 apart");
        double v[3] = {3.0, 4.0, 0.0};
        Check(WrDpDist3(v, zero) == 5.0, "3-4-5 is exact, as it must be");
        Check(WrDpHypot2(-3.0, -4.0) == 5.0, "and hypot takes the absolute value");
    }

    // -----------------------------------------------------------------------
    // Python's builtin sum() over floats is Neumaier-compensated from 3.12 on,
    // and the reference uses it for the path length and for the centroid of an
    // origin-cluster test that decides whether a segment is kept. A running
    // total is not the same function.
    printf("\nbuiltin sum(), which has been compensated since 3.12\n");
    {
        const int ns = (int)(sizeof(kNormSum) / sizeof(kNormSum[0]));
        int bad = 0, naiveDisagrees = 0;
        long long terms = 0;
        for (int i = 0; i < ns; i++)
        {
            // The same integer LCG gen_norm.py used, and the same exact
            // conversion: (s >> 11) is at most 53 bits, so it is a double
            // without rounding, and the two multiplies round identically.
            unsigned long long s = kNormSum[i].seed;
            WrDpSum acc;
            WrDpSumInit(&acc);
            double naive = 0.0;
            for (int k = 0; k < kNormSum[i].count; k++)
            {
                s = s * 6364136223846793005ULL + 1442695040888963407ULL;
                const double u = ldexp((double)(s >> 11), -53);
                const double x = (u - 0.5 + 0.5 * kNormSum[i].bias)
                               * kNormSum[i].scale;
                WrDpSumAdd(&acc, x);
                naive += x;
            }
            terms += kNormSum[i].count;
            if (AsBits(WrDpSumEnd(&acc)) != kNormSum[i].want)
                bad++;
            if (AsBits(naive) != kNormSum[i].want)
                naiveDisagrees++;
        }
        printf("     %d rows, %lld terms, %d of which a running total gets wrong\n",
               ns, terms, naiveDisagrees);
        Check(bad == 0, "sum() agrees to the last bit on every row");
        Check(naiveDisagrees == WR_NORM_SUM_DIFFERS && naiveDisagrees > 0,
              "and the table can tell it from a running total");

        // sum() starts at the integer 0, so this comes out POSITIVE zero.
        WrDpSum z;
        WrDpSumInit(&z);
        for (int i = 0; i < 3; i++)
            WrDpSumAdd(&z, -0.0);
        Check(AsBits(WrDpSumEnd(&z)) == WR_NORM_SUM_NEG_ZERO,
              "and a sum of negative zeros keeps the sign sum() gives it");

        WrDpSum e;
        WrDpSumInit(&e);
        Check(WrDpSumEnd(&e) == 0.0, "an empty sum is 0.0, as sum(()) is");
    }

    // -----------------------------------------------------------------------
    printf("\na float at a bit position, against the shifted-buffer original\n");
    {
        // Pseudo-random but fixed, so a failure is reproducible.
        unsigned char body[257];
        unsigned int s = 0x1234567u;
        for (int i = 0; i < (int)sizeof(body); i++)
        {
            s = s * 1103515245u + 12345u;
            body[i] = (unsigned char)(s >> 16);
        }

        const size_t n = sizeof(body);
        long long tested = 0, mismatched = 0, refusedBoth = 0;
        for (long long p = -3; p <= (long long)(n * 8) + 8; p++)
        {
            double want = 0.0, got = 0.0;
            const bool okRef = ShiftedRead(body, n, p, &want);
            const bool okGot = WrDpFloatAt(body, n, p, &got);
            if (okRef != okGot)
                mismatched++;
            else if (!okRef)
                refusedBoth++;
            else if (AsBits(want) != AsBits(got))
                mismatched++;
            tested++;
        }
        printf("     %lld bit positions, %lld of them out of bounds\n",
               tested, refusedBoth);
        Check(mismatched == 0, "every bit position reads the same float");

        // The bound is the reference's, exactly: a read is refused when
        // (p >> 3) + 4 > n, which admits reads whose top bits are past the end.
        double d = 0.0;
        Check(WrDpFloatAt(body, n, (long long)(n - 4) * 8, &d),
              "a read ending exactly at the last byte is allowed");
        Check(!WrDpFloatAt(body, n, (long long)(n - 3) * 8, &d),
              "and one byte further is not");
        Check(WrDpFloatAt(body, n, (long long)(n - 4) * 8 + 7, &d),
              "a read seven bits past that IS allowed, and reads zeros");
        Check(!WrDpFloatAt(body, n, -1, &d), "a negative position is refused");
    }

    // -----------------------------------------------------------------------
    printf("\nend to end, on a netstream whose answer is known\n");
    {
        // 0xFF filler: a word of all ones has a biased exponent of 255, which
        // is outside [117, 141], so no read anywhere in it is a candidate.
        const int kPoints = 200;
        const int kStride = 32;             // bytes between triples
        const size_t n = 8192;
        unsigned char *body = (unsigned char *)malloc(n);
        memset(body, 0xFF, n);

        // A gentle helix, well inside the world limit and well inside the
        // smoothness gate. One step is deliberately doubled, at the halfway
        // point, so the 99.5th-percentile cap has something to drop.
        // 30 units, not more: the smoothness gate allows a step vector to
        // differ from the one before it by TOL_FLOOR + TOL_FRAC * the longer of
        // the two, so a 60-unit jolt after 7-unit steps is REFUSED and the
        // chain breaks there. 30 is comfortably inside that and still four
        // times the percentile cap, which is what the check below needs.
        float want[200][3];
        for (int i = 0; i < kPoints; i++)
        {
            const double t = i * 0.02;
            const double bump = (i >= kPoints / 2) ? 30.0 : 0.0;
            want[i][0] = (float)(2000.0 + 300.0 * cos(t) + bump);
            want[i][1] = (float)(3000.0 + 300.0 * sin(t));
            want[i][2] = (float)(1500.0 + 5.0 * i);

            const size_t at = (size_t)WR_DP_SCAN_START_BYTE
                            + (size_t)i * (size_t)kStride;
            memcpy(body + at, want[i], 12);
        }

        WrDpArgs a;
        memset(&a, 0, sizeof(a));
        a.body = body;
        a.bodyLen = n;
        a.tickInterval = 0.015;
        a.ticks = (unsigned int)kPoints;
        a.haveRef = false;                  // nothing to check against; take it
        a.keepDetail = true;

        WrDpResult r;
        bool cancelled = false;
        char err[256] = "";
        const bool got = WrDpExtract(&a, &r, &cancelled, err, sizeof(err));
        if (!got)
            printf("     refused: %s\n", err);
        Check(got, "a planted trajectory is extracted at all");

        if (got)
        {
            printf("     %d candidates, %d points, %d segment(s), %d rounds\n",
                   r.info.candidates, r.pointCount, r.info.segments,
                   r.info.rounds);

            // Every point that came out is one that went in. A recovered point
            // that is not a planted one would mean the chain wandered into the
            // filler, which is the failure this whole file exists to catch.
            int unknown = 0, matchedInOrder = 0, last = -1;
            for (int i = 0; i < r.pointCount; i++)
            {
                int found = -1;
                for (int k = 0; k < kPoints; k++)
                    if ((float)r.points[i].x == want[k][0] &&
                        (float)r.points[i].y == want[k][1] &&
                        (float)r.points[i].z == want[k][2])
                    {
                        found = k;
                        break;
                    }
                if (found < 0)
                    unknown++;
                else if (found > last)
                {
                    matchedInOrder++;
                    last = found;
                }
            }
            printf("     %d points recovered, %d of them planted and in order\n",
                   r.pointCount, matchedInOrder);
            Check(unknown == 0, "no recovered point came from the filler");
            Check(matchedInOrder == kPoints,
                  "the whole planted path came back, in order");
            Check(r.info.confident && strcmp(r.info.identifiedBy, "speed") == 0,
                  "with no reference speed, the first chain is taken");
            Check(r.info.segments == 1, "and it is one segment, not a stitch");
            Check(r.chainCount > 0 && r.chain != NULL,
                  "--dump-chain's bit positions are kept when asked for");

            // The 99.5th percentile. 199 steps, so the cap is lens[196] and the
            // deliberately doubled one is above it -- if it were not dropped,
            // the reported speed would be about four thousand rather than five
            // hundred.
            const double slow = 7.0 / 0.015;        // ~470 u/s
            printf("     max horizontal speed %.1f u/s (a normal step is %.0f)\n",
                   r.info.chainMaxHoriz, slow);
            Check(r.info.chainMaxHoriz < slow * 2.0,
                  "the outlier step is above the percentile cap and dropped");

            // The central difference, within a segment, with the ends held.
            const double dt = 0.015;
            const double vx0 = ((double)r.points[1].x - (double)r.points[0].x) / dt;
            Check(fabs(r.points[0].vx - vx0) < 1e-9,
                  "the first point differences forward over one tick, not two");

            WrDpFree(&r);
        }
        free(body);
    }

    // -----------------------------------------------------------------------
    printf("\nrefusals say what the reference says\n");
    {
        unsigned char *body = (unsigned char *)malloc(4096);
        memset(body, 0xFF, 4096);           // no plausible word anywhere

        WrDpArgs a;
        memset(&a, 0, sizeof(a));
        a.body = body;
        a.bodyLen = 4096;
        a.tickInterval = 0.015;
        a.ticks = 100;

        WrDpResult r;
        bool cancelled = false;
        char err[256] = "";
        const bool got = WrDpExtract(&a, &r, &cancelled, err, sizeof(err));
        Check(!got, "a body with no coordinates in it is refused");
        Check(strstr(err, "coordinate-triple candidates") != NULL &&
              strstr(err, "player delta stream") != NULL,
              "in the reference's words, which reach _failed.txt");
        Check(!cancelled, "and it is a failure rather than a cancellation");
        printf("     %s\n", err);
        WrDpFree(&r);
        free(body);
    }

    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
