// wr_dp.cpp  --  see wr_dp.h.

#include "wr_dp.h"

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// CPython's vector norm, transcribed
// ---------------------------------------------------------------------------
//
// Modules/mathmodule.c, CPython 3.13.9. Deliberately kept in the original's
// shape and variable names so it can be diffed against upstream: dl_fast_sum,
// dl_mul, vector_norm. Four techniques, and every one of them changes the
// answer in the last places:
//
//   * lossless scaling by a power of two, so nothing overflows or underflows
//   * exact squaring, so x*x contributes no rounding error of its own
//   * compensated summation (Neumaier), so the sum of the squares is exact to
//     more than double precision
//   * a differential correction of the square root
//
// dl_mul is the fma form. The original has a second form using Veltkamp-Dekker
// splitting for platforms whose fma is slow, and the two are BIT-IDENTICAL --
// both are error-free transformations of the same product -- so which one the
// reference interpreter was built with cannot matter here. What would matter is
// an fma that is not correctly rounded; MSVC's is, and test_dp checks the whole
// function against a table the reference generated rather than taking that on
// trust.

struct DoubleLength
{
    double hi, lo;
};

static DoubleLength DlFastSum(double a, double b)
{
    // Compensated summation of two floating-point numbers, requiring
    // |a| >= |b|. The caller below satisfies that by construction: csum starts
    // at 1.0 and every added term is a square of something under 1.0.
    DoubleLength r;
    r.hi = a + b;
    r.lo = (a - r.hi) + b;
    return r;
}

static DoubleLength DlMul(double x, double y)
{
    // Error-free transformation of a product: hi is the rounded product and lo
    // is exactly the rounding error it dropped.
    DoubleLength r;
    r.hi = x * y;
    r.lo = fma(x, y, -r.hi);
    return r;
}

// The original takes a found_nan flag its caller computed and returns NaN for
// it. There is no such arm here because there is no such input: every value
// this file measures is a difference of coordinates that the scan already
// checked against +-65536, and NaN fails that test in both directions. A NaN
// reaching here would come out of the comparisons below as a NaN anyway, which
// is the same answer by a less deliberate route.
double WrDpVectorNorm(double *vec, int n, double max)
{
    double x, h, scale, csum = 1.0, frac1 = 0.0, frac2 = 0.0;
    DoubleLength pr, sm;
    int max_e;
    int i;

    if (isinf(max))
        return max;
    if (max == 0.0 || n <= 1)
        return max;
    frexp(max, &max_e);
    if (max_e < -1023)
    {
        // ldexp(1.0, -max_e) would overflow, so bring the subnormals up into
        // the normal range first and scale the answer back afterwards.
        for (i = 0; i < n; i++)
            vec[i] /= DBL_MIN;
        return DBL_MIN * WrDpVectorNorm(vec, n, max / DBL_MIN);
    }
    scale = ldexp(1.0, -max_e);
    for (i = 0; i < n; i++)
    {
        x = vec[i];
        x *= scale;                     // lossless scaling
        pr = DlMul(x, x);               // lossless squaring
        sm = DlFastSum(csum, pr.hi);    // lossless addition
        csum = sm.hi;
        frac1 += pr.lo;                 // lossy addition
        frac2 += sm.lo;                 // lossy addition
    }
    h = sqrt(csum - 1.0 + (frac1 + frac2));
    pr = DlMul(-h, h);
    sm = DlFastSum(csum, pr.hi);
    csum = sm.hi;
    frac1 += pr.lo;
    frac2 += sm.lo;
    x = csum - 1.0 + (frac1 + frac2);
    h += x / (2.0 * h);                 // differential correction
    return h / scale;
}

// ---------------------------------------------------------------------------
// Python's builtin sum(), which is also not what it looks like
// ---------------------------------------------------------------------------
//
// Python/bltinmodule.c, builtin_sum_impl. Since 3.12 the float fast path is the
// improved Kahan-Babuska algorithm of Arnold Neumaier, which differs from plain
// Kahan in taking the larger magnitude as the base of the correction -- that is
// what makes it right when a later term is bigger than the running total, which
// is the case a path length hits constantly.
//
// sum() starts at the integer 0, so the first float goes through 0 + x and is
// exact; starting the accumulator at 0.0 with c = 0.0 is the same thing, since
// t = 0.0 + x is exact and the correction it contributes is (0.0 - x) + x = 0.
//
// This is NOT math.fsum. fsum is exactly rounded and would disagree.

void WrDpSumInit(WrDpSum *s)
{
    s->total = 0.0;
    s->c = 0.0;
}

void WrDpSumAdd(WrDpSum *s, double x)
{
    const double t = s->total + x;
    if (fabs(s->total) >= fabs(x))
        s->c += (s->total - t) + x;
    else
        s->c += (x - t) + s->total;
    s->total = t;
}

double WrDpSumEnd(const WrDpSum *s)
{
    // CPython's own guard, and both halves of it earn their place: adding a
    // zero correction would turn a -0.0 total into +0.0, and adding an infinite
    // one would turn an overflowed total into a NaN.
    if (s->c != 0.0 && isfinite(s->c))
        return s->total + s->c;
    return s->total;
}

// math.dist(p, q). The absolute differences, their maximum, then the norm --
// and note that a NaN component is not possible here: every coordinate came out
// of the scan, which rejected everything outside +-65536 and NaN fails that
// test in both directions.
double WrDpDist3(const double a[3], const double b[3])
{
    double d[3];
    double max = 0.0;
    for (int i = 0; i < 3; i++)
    {
        d[i] = fabs(a[i] - b[i]);
        if (d[i] > max)
            max = d[i];
    }
    return WrDpVectorNorm(d, 3, max);
}

// math.hypot(dx, dy). The same function with n = 2; the reference has no
// two-argument special case and neither does this.
double WrDpHypot2(double dx, double dy)
{
    double d[2];
    d[0] = fabs(dx);
    d[1] = fabs(dy);
    double max = d[0] > d[1] ? d[0] : d[1];
    return WrDpVectorNorm(d, 2, max);
}

// ---------------------------------------------------------------------------
// Reading a float at a bit position
// ---------------------------------------------------------------------------
//
// The reference shifts the whole body by each of eight phases and reads the
// shifted copies byte-aligned; this is one unaligned load and a shift, which is
// the same 32 bits. See the header for why the two agree.
//
// PAST THE END READS ZEROS, and that is not sloppiness -- it is the behaviour
// being reproduced. The reference's shifted copies are made with
// int.from_bytes(...).to_bytes(n), so bits above the body's last byte are
// genuinely zero there, and a read whose top few bits fall off the end picks
// them up. Its bound is `(p >> 3) + 4 > n`, which admits reads that do exactly
// that, so the zero fill is reachable on real demos and has to be right.

static unsigned long long Load64Z(const unsigned char *b, size_t n, size_t off)
{
    unsigned long long v = 0;
    if (off + 8 <= n)
        memcpy(&v, b + off, 8);
    else if (off < n)
        memcpy(&v, b + off, n - off);
    return v;
}

// The same load where the caller has already established that all eight bytes
// exist. The scan's own bound guarantees it -- see WrDpScan -- and this is its
// innermost operation, run eight times per byte of body.
static inline unsigned long long Load64(const unsigned char *b, size_t off)
{
    unsigned long long v;
    memcpy(&v, b + off, 8);
    return v;
}

bool WrDpFloatAt(const unsigned char *body, size_t len, long long bitPos,
                 double *out)
{
    if (bitPos < 0)
        return false;
    unsigned long long p = (unsigned long long)bitPos;
    if ((p >> 3) + 4 > (unsigned long long)len)
        return false;

    unsigned int w = (unsigned int)(Load64Z(body, len, (size_t)(p >> 3)) >> (p & 7));
    float f;
    memcpy(&f, &w, 4);
    *out = (double)f;
    return true;
}

// ---------------------------------------------------------------------------
// The candidate scan
// ---------------------------------------------------------------------------

static inline bool PlausibleBits(unsigned int w)
{
    if (w == 0u || w == 0x80000000u)
        return true;
    unsigned int e = (w >> 23) & 0xFFu;
    return e >= WR_DP_EXP_LO && e <= WR_DP_EXP_HI;
}

static int CmpCandBit(const void *a, const void *b)
{
    unsigned int x = ((const WrDpCand *)a)->bit;
    unsigned int y = ((const WrDpCand *)b)->bit;
    return x < y ? -1 : (x > y ? 1 : 0);
}

// The one message that is not the reference's, because the reference cannot
// produce it: Python's dict grows until the machine says no and the resulting
// MemoryError takes the whole run down with it. Here it is one demo's failure.
static void Oom(char *err, int errCap, const char *where)
{
    _snprintf_s(err, (size_t)errCap, _TRUNCATE, "out of memory in %s", where);
}

bool WrDpScan(const unsigned char *body, size_t n, unsigned int startByte,
              WrDpCand **out, int *countOut, int *stop,
              WrDpAbort abort, void *user, char *err, int errCap)
{
    *out = NULL;
    *countOut = 0;
    *stop = 0;
    if (n < 64)
        return true;                    // the reference returns {}

    int cap = 4096, cnt = 0;
    WrDpCand *arr = (WrDpCand *)malloc(sizeof(WrDpCand) * (size_t)cap);
    if (!arr)
    {
        Oom(err, errCap, "the candidate scan");
        return false;
    }

    const long long skip = (long long)(startByte / 4);

    for (int phase = 0; phase < 8; phase++)
    {
        for (int q = 0; q < 4; q++)
        {
            // Per (phase, q) rather than per phase; see the header. It can only
            // make a run give up sooner, never change a number.
            if (abort)
            {
                int r = abort(user);
                if (r)
                {
                    free(arr);
                    *stop = r;
                    return false;
                }
            }

            // (n - q) // 4, and the last admissible j is count - 4. Both halves
            // of that depend on q, which is the whole trap; see the header.
            const long long count = (long long)((n - (size_t)q) / 4);
            if (count < 8)
                continue;

            for (long long j = skip; j < count - 3; j++)
            {
                // off + 8 + 8 <= n for every j the bound admits, so these three
                // loads never need the zero fill: 4*j <= (n - q) - 16.
                const size_t off = (size_t)q + 4u * (size_t)j;

                unsigned int wx = (unsigned int)(Load64(body, off) >> phase);
                if (!PlausibleBits(wx))
                    continue;
                unsigned int wy = (unsigned int)(Load64(body, off + 4) >> phase);
                if (!PlausibleBits(wy))
                    continue;
                unsigned int wz = (unsigned int)(Load64(body, off + 8) >> phase);
                if (!PlausibleBits(wz))
                    continue;

                float x, y, z;
                memcpy(&x, &wx, 4);
                memcpy(&y, &wy, 4);
                memcpy(&z, &wz, 4);
                if (!(x >= -(float)WR_DP_WORLD_LIMIT && x <= (float)WR_DP_WORLD_LIMIT))
                    continue;
                if (!(y >= -(float)WR_DP_WORLD_LIMIT && y <= (float)WR_DP_WORLD_LIMIT))
                    continue;
                if (!(z >= -(float)WR_DP_WORLD_LIMIT && z <= (float)WR_DP_WORLD_LIMIT))
                    continue;

                if (cnt == cap)
                {
                    if (cap > 0x30000000)
                    {
                        free(arr);
                        Oom(err, errCap, "the candidate scan");
                        return false;
                    }
                    int ncap = cap * 2;
                    WrDpCand *na = (WrDpCand *)realloc(arr,
                                        sizeof(WrDpCand) * (size_t)ncap);
                    if (!na)
                    {
                        free(arr);
                        Oom(err, errCap, "the candidate scan");
                        return false;
                    }
                    arr = na;
                    cap = ncap;
                }

                WrDpCand *c = &arr[cnt++];
                c->bit = (unsigned int)((long long)phase + 8ll * q + 32ll * j);
                c->x = x;
                c->y = y;
                c->z = z;
            }
        }
    }

    // The reference's `keys = sorted(cands)`. The bijection in the header is
    // what makes this a sort and not a merge: no two (phase, q, j) reach the
    // same bit, so nothing here can collide.
    qsort(arr, (size_t)cnt, sizeof(WrDpCand), CmpCandBit);

    *out = arr;
    *countOut = cnt;
    return true;
}

// ---------------------------------------------------------------------------
// The scratch a single extraction needs
// ---------------------------------------------------------------------------
//
// One allocation per array, all sized by the candidate count, all freed
// together. Written out rather than hidden behind a helper because the memory
// this file uses is the reason the port could not simply be dropped into the
// game -- it is now about 45 bytes per candidate plus 48 per point, against the
// reference's roughly ten times the body.

struct Scratch
{
    int *ks;            // the working key list: indices into cands
    int *dp;
    int *pred;
    int *chain;         // the current round's chain
    int *best;          // the longest chain seen, first-wins on ties
    int *ident;         // the first chain the derivative test confirmed
    int *chosen;        // the winner, copied so harvesting can reuse `chain`
    int *segFlat;       // every accepted segment's members, concatenated
    unsigned char *banned;
    unsigned char *hbanned;
    double *lens;       // step lengths, for the percentile cap
    unsigned int *stepBit;
    double *stepVz;
};

static void ScratchFree(Scratch *s)
{
    free(s->ks); free(s->dp); free(s->pred); free(s->chain);
    free(s->best); free(s->ident); free(s->chosen); free(s->segFlat);
    free(s->banned); free(s->hbanned); free(s->lens);
    free(s->stepBit); free(s->stepVz);
    memset(s, 0, sizeof(*s));
}

static bool ScratchAlloc(Scratch *s, int n)
{
    memset(s, 0, sizeof(*s));
    size_t ints = sizeof(int) * (size_t)n;
    s->ks      = (int *)malloc(ints);
    s->dp      = (int *)malloc(ints);
    s->pred    = (int *)malloc(ints);
    s->chain   = (int *)malloc(ints);
    s->best    = (int *)malloc(ints);
    s->ident   = (int *)malloc(ints);
    s->chosen  = (int *)malloc(ints);
    s->segFlat = (int *)malloc(ints);
    s->banned  = (unsigned char *)calloc((size_t)n, 1);
    s->hbanned = (unsigned char *)calloc((size_t)n, 1);
    s->lens    = (double *)malloc(sizeof(double) * (size_t)n);
    s->stepBit = (unsigned int *)malloc(sizeof(unsigned int) * (size_t)n);
    s->stepVz  = (double *)malloc(sizeof(double) * (size_t)n);

    if (!s->ks || !s->dp || !s->pred || !s->chain || !s->best || !s->ident ||
        !s->chosen || !s->segFlat || !s->banned || !s->hbanned || !s->lens ||
        !s->stepBit || !s->stepVz)
    {
        ScratchFree(s);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The dynamic program
// ---------------------------------------------------------------------------
//
// dp[j] is the length of the best chain ending at candidate j, so a wrong edge
// anywhere costs only that edge -- unlike a greedy walk, which loses the entire
// remainder of the run.
//
// The reference keeps a third array, step[], holding the vector into each
// candidate. It is not kept here because it is derivable: step[j] is set in the
// same statement as pred[j] and is exactly pos[j] - pos[pred[j]], so recomputing
// it from pred costs three subtractions per outer iteration and saves 24 bytes
// per candidate. The two are equal bit for bit, not merely close -- the same
// two doubles are subtracted in the same order.
//
// Returns the chain length, writing candidate indices into `out` in ascending
// bit order. *stop is set to the abort code if the caller's predicate fired.
static int LongestSmoothChain(const WrDpCand *c, const int *ks, int nks,
                              long long gapMax, Scratch *s, int *out,
                              WrDpAbort abort, void *user, int *stop)
{
    *stop = 0;
    if (nks < 2)
        return 0;

    int *dp = s->dp, *pred = s->pred;
    for (int i = 0; i < nks; i++)
    {
        dp[i] = 1;
        pred[i] = -1;
    }

    for (int i = 0; i < nks; i++)
    {
        if ((i & 0xFFF) == 0 && abort)
        {
            int r = abort(user);
            if (r)
            {
                *stop = r;
                return 0;
            }
        }

        const WrDpCand *ci = &c[ks[i]];
        const long long bi = (long long)ci->bit;
        const double xi = (double)ci->x, yi = (double)ci->y, zi = (double)ci->z;

        // The step INTO i, which is what the smoothness gate compares against.
        double six = 0.0, siy = 0.0, siz = 0.0;
        const bool haveSi = pred[i] >= 0;
        if (haveSi)
        {
            const WrDpCand *cp = &c[ks[pred[i]]];
            six = xi - (double)cp->x;
            siy = yi - (double)cp->y;
            siz = zi - (double)cp->z;
        }

        const int di = dp[i];
        const long long limit = bi + gapMax;

        for (int j = i + 1; j < nks; j++)
        {
            const WrDpCand *cj = &c[ks[j]];
            const long long bj = (long long)cj->bit;
            if (bj > limit)
                break;
            if (bj - bi < WR_DP_GAP_MIN)
                continue;

            const double sx = (double)cj->x - xi;
            const double sy = (double)cj->y - yi;
            const double sz = (double)cj->z - zi;
            // Plain sqrt, as in the reference. See the header: where it is
            // naive, so is this.
            const double sl = sqrt(sx * sx + sy * sy + sz * sz);
            if (sl <= WR_DP_MAX_STEP && di + 1 > dp[j])
            {
                bool ok = true;
                if (haveSi)
                {
                    const double dvx = sx - six;
                    const double dvy = sy - siy;
                    const double dvz = sz - siz;
                    const double dv = sqrt(dvx * dvx + dvy * dvy + dvz * dvz);
                    const double pl = sqrt(six * six + siy * siy + siz * siz);
                    if (dv > WR_DP_TOL_FLOOR + WR_DP_TOL_FRAC * (sl > pl ? sl : pl))
                        ok = false;
                }
                if (ok)
                {
                    dp[j] = di + 1;
                    pred[j] = i;
                }
            }
        }
    }

    // max(range(n), key=dp.__getitem__) -- the FIRST index attaining the
    // maximum, which is a determinism requirement and not a detail. Strict >.
    int best = 0;
    for (int i = 1; i < nks; i++)
        if (dp[i] > dp[best])
            best = i;

    int len = 0;
    for (int k = best; k != -1; k = pred[k])
        out[len++] = ks[k];
    for (int a = 0, b = len - 1; a < b; a++, b--)
    {
        int t = out[a];
        out[a] = out[b];
        out[b] = t;
    }
    return len;
}

// ---------------------------------------------------------------------------
// Scoring
// ---------------------------------------------------------------------------

static int CmpDouble(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static inline void CandXyz(const WrDpCand *c, int idx, double p[3])
{
    p[0] = (double)c[idx].x;
    p[1] = (double)c[idx].y;
    p[2] = (double)c[idx].z;
}

// Max horizontal speed implied by a chain's own step lengths.
//
// Steps above the 99.5th percentile are dropped: those are the places where a
// tick is missing from the stream, and a two-tick step would read as double the
// real speed.
//
// The distance is computed twice per step -- once to build the percentile, once
// in the pass that uses it -- exactly as the reference does. Keeping a second
// unsorted copy would halve the compensated-norm work and cost eight more bytes
// per candidate, and this is not where the time goes.
static double PeakHorizontalSpeed(const WrDpCand *c, const int *idx, int n,
                                  double dt, double *lens)
{
    if (n < 8)
        return 0.0;

    const int m = n - 1;
    for (int i = 0; i < m; i++)
    {
        double a[3], b[3];
        CandXyz(c, idx[i], a);
        CandXyz(c, idx[i + 1], b);
        lens[i] = WrDpDist3(a, b);
    }
    qsort(lens, (size_t)m, sizeof(double), CmpDouble);
    const double cap = lens[(int)(0.995 * (double)(m - 1))];

    double peak = 0.0;
    for (int i = 0; i < m; i++)
    {
        double a[3], b[3];
        CandXyz(c, idx[i], a);
        CandXyz(c, idx[i + 1], b);
        if (WrDpDist3(a, b) > cap)
            continue;
        double v = WrDpHypot2(b[0] - a[0], b[1] - a[1]) / dt;
        if (v > peak)
            peak = v;
    }
    return peak;
}

static bool OriginConfirmed(double rate, int steps)
{
    if (steps < WR_DP_DERIV_MIN_STEPS)
        return false;
    if (steps >= WR_DP_DERIV_SMALL_SAMPLE)
        return rate >= WR_DP_DERIV_MIN_FRACTION;
    return rate >= WR_DP_DERIV_SMALL_FRACTION;
}

// Does the stream carry this chain's own vertical velocity beside it?
//
// This is how a position stream is told from a velocity one WITHOUT relying on
// magnitude, and it is the only thing that works when a map's stage sits near
// the world origin. There the player's coordinates are a few hundred units --
// the same size as a velocity vector -- so "is this too big to be a position?"
// answers nothing, and the DP happily returns a velocity chain instead. Five of
// surf_colin_blaster_69000's tracks are laid out that way.
//
// Source networks position and velocity in the same entity delta, so if a chain
// is the origin then its own derivative is sitting a few dozen bits away. Only
// the VERTICAL component is reliably present in these demos -- a three-component
// match scores 0.19 even on a chain known to be correct -- so match on vz alone.
//
// One float matching once proves nothing. What makes this decisive is that it
// must match at the SAME relative bit offset on most steps, because the send
// table layout is fixed. Measured on a known-good chain: 74.6% at offset 128,
// against 1.9-8.2% for every velocity and projectile chain in the same file.
static void OriginScore(const unsigned char *body, size_t bodyLen,
                        const WrDpCand *c, const int *chain, int chainN,
                        double dt, long long frameBits, Scratch *s,
                        double *rateOut, int *offOut, int *nsOut,
                        WrDpAbort abort, void *user, int *stop)
{
    *rateOut = 0.0;
    *offOut = -1;
    *nsOut = 0;
    *stop = 0;
    if (chainN < 12 || frameBits <= 0)
        return;

    int ns = 0;
    for (int i = 0; i < chainN - 1; i++)
    {
        const long long gap = (long long)c[chain[i + 1]].bit
                            - (long long)c[chain[i]].bit;
        if ((double)gap > (double)frameBits * 1.6)
            continue;                   // not consecutive frames; not one tick
        const double vz = ((double)c[chain[i + 1]].z - (double)c[chain[i]].z) / dt;
        if (fabs(vz) < 60.0)
            continue;                   // level flight: vz ~ 0 matches far too much
        s->stepBit[ns] = c[chain[i]].bit;
        s->stepVz[ns] = vz;
        ns++;
    }

    // Below this the best-of-N offset search finds coincidences, so refuse to
    // answer rather than answer badly.
    if (ns < WR_DP_DERIV_MIN_STEPS)
    {
        *nsOut = ns;
        return;
    }

    // Cost is offsets x steps, and both grow with the run: a 30000-point chain
    // over ~500 offsets is 15 million float reads, per round, up to 14 rounds.
    // Subsample evenly instead -- the measured separation was 74.6% for a real
    // chain against 1.9-8.2% for everything else using 524 steps.
    //
    // In place and forward, which is safe because int(k * stride) >= k whenever
    // stride >= 1, and stride is total/400 with total > 400.
    if (ns > WR_DP_DERIV_MAX_SAMPLES)
    {
        const double stride = (double)ns / (double)WR_DP_DERIV_MAX_SAMPLES;
        for (int k = 0; k < WR_DP_DERIV_MAX_SAMPLES; k++)
        {
            const int src = (int)((double)k * stride);
            s->stepBit[k] = s->stepBit[src];
            s->stepVz[k] = s->stepVz[src];
        }
        ns = WR_DP_DERIV_MAX_SAMPLES;
    }

    double best = 0.0;
    int bestoff = -1;
    const long long lim = (long long)((double)frameBits * 1.2);
    for (long long off = 0; off < lim; off++)
    {
        if (abort)
        {
            int r = abort(user);
            if (r)
            {
                *stop = r;
                return;
            }
        }
        int hit = 0;
        for (int k = 0; k < ns; k++)
        {
            double w;
            if (!WrDpFloatAt(body, bodyLen, (long long)s->stepBit[k] + off, &w))
                continue;
            const double vz = s->stepVz[k];
            const double rel = WR_DP_DERIV_TOL_REL * fabs(vz);
            const double tol = WR_DP_DERIV_TOL_ABS > rel ? WR_DP_DERIV_TOL_ABS : rel;
            if (fabs(w - vz) <= tol)
                hit++;
        }
        // As written, not the algebraically equal form. best starts at 0.0, so
        // an all-miss sweep leaves bestoff at -1.
        if ((double)hit > best * (double)ns)
        {
            best = (double)hit / (double)ns;
            bestoff = (int)off;
        }
    }

    *rateOut = best;
    *offOut = bestoff;
    *nsOut = ns;
}

// Could this chain be a leg of the player's path?
//
// `deriv` is -1 when the derivative test could not be consulted, and 0 or 1
// otherwise. Near the world origin a velocity chain is the same size as a
// position one, so the magnitude tests below cannot separate them and this is
// the only thing that can. A clear negative overrides everything else.
static bool SegmentPlausible(const WrDpCand *c, const int *idx, int n,
                             double dt, bool haveRef, double ref, int deriv,
                             double *lens)
{
    if (n < WR_DP_MIN_SEGMENT)
        return false;
    if (deriv >= 0 && (double)deriv < WR_DP_DERIV_MIN_FRACTION)
        return false;

    double lo[3], hi[3];
    CandXyz(c, idx[0], lo);
    CandXyz(c, idx[0], hi);
    double sum[3] = {0.0, 0.0, 0.0};
    for (int i = 0; i < n; i++)
    {
        double p[3];
        CandXyz(c, idx[i], p);
        for (int k = 0; k < 3; k++)
        {
            if (p[k] < lo[k]) lo[k] = p[k];
            if (p[k] > hi[k]) hi[k] = p[k];
        }
    }
    if (WrDpDist3(lo, hi) < WR_DP_SEGMENT_MIN_EXTENT)
        return false;

    // sum(xs) / n, and sum() is Neumaier-compensated from Python 3.12 on. A
    // running total is close but not equal, and this one is not cosmetic: the
    // centroid is about to be measured against ORIGIN_CLUSTER_RADIUS, and a
    // segment that falls on the wrong side of that test is a leg of somebody's
    // route that does not get written.
    WrDpSum acc[3];
    for (int k = 0; k < 3; k++)
        WrDpSumInit(&acc[k]);
    for (int i = 0; i < n; i++)
    {
        double p[3];
        CandXyz(c, idx[i], p);
        for (int k = 0; k < 3; k++)
            WrDpSumAdd(&acc[k], p[k]);
    }
    for (int k = 0; k < 3; k++)
        sum[k] = WrDpSumEnd(&acc[k]);
    double centroid[3] = {sum[0] / n, sum[1] / n, sum[2] / n};
    const double zero[3] = {0.0, 0.0, 0.0};
    if (WrDpDist3(centroid, zero) < WR_DP_ORIGIN_CLUSTER_RADIUS)
        return false;

    const double peak = PeakHorizontalSpeed(c, idx, n, dt, lens);
    if (haveRef && ref > 0)
        return peak <= ref * (1.0 + WR_DP_SEGMENT_SPEED_SLACK) + 50.0;
    return peak <= 6000.0;
}

// ---------------------------------------------------------------------------
// extract_path
// ---------------------------------------------------------------------------

static void Fail(char *err, int errCap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(err, (size_t)errCap, _TRUNCATE, fmt, ap);
    va_end(ap);
}

// The deadline message, word for word, because it is written into
// paths\<map>\_failed.txt and a record with a different reason reads as a
// different failure.
static void Timeout(char *err, int errCap, double seconds, const char *where)
{
    Fail(err, errCap, "gave up after %.0f s in %s -- pass --timeout 0 for no "
                      "limit, or a larger value", seconds, where);
}

// True having filled in err; the caller unwinds. `stop` is an abort code.
static bool Stopped(int stop, bool *cancelled, char *err, int errCap,
                    double seconds, const char *where)
{
    if (stop == WR_DP_STOP_CANCEL)
    {
        *cancelled = true;
        Fail(err, errCap, "stopped");
    }
    else
    {
        Timeout(err, errCap, seconds, where);
    }
    return false;
}

void WrDpFree(WrDpResult *r)
{
    if (!r)
        return;
    free(r->points);
    free(r->chain);
    free(r->segBits);
    free(r->segOff);
    free(r->segLen);
    memset(r, 0, sizeof(*r));
}

bool WrDpExtract(const WrDpArgs *args, WrDpResult *out, bool *cancelled,
                 char *err, int errCap)
{
    memset(out, 0, sizeof(*out));
    *cancelled = false;
    err[0] = '\0';

    const double dt = args->tickInterval;
    const unsigned int ticks = args->ticks;

    WrDpCand *cands = NULL;
    int nCands = 0;
    int stop = 0;
    if (!WrDpScan(args->body, args->bodyLen, WR_DP_SCAN_START_BYTE, &cands,
                  &nCands, &stop, args->abort, args->abortUser, err, errCap))
    {
        if (stop)
            return Stopped(stop, cancelled, err, errCap, args->timeoutSeconds,
                           "the candidate scan");
        return false;                   // out of memory; err already says so
    }

    out->info.candidates = nCands;
    out->info.ticks = ticks;
    out->info.haveRef = args->haveRef;
    out->info.refMaxHoriz = args->refMaxHoriz;

    if (nCands < WR_DP_MIN_CHAIN)
    {
        Fail(err, errCap, "only %d coordinate-triple candidates; this body does "
                          "not look like a player delta stream", nCands);
        free(cands);
        return false;
    }

    Scratch s;
    if (!ScratchAlloc(&s, nCands))
    {
        Oom(err, errCap, "the chain search");
        free(cands);
        return false;
    }

    // Frames are roughly evenly spread through the body, so the average
    // bits-per-tick bounds how far apart two consecutive ones can sensibly be.
    const double avgBits = ticks ? ((double)args->bodyLen * 8.0 / (double)ticks)
                                 : 0.0;
    long long gapMax = (long long)(avgBits * 8.0);
    if (gapMax < WR_DP_GAP_MAX_FLOOR)
        gapMax = WR_DP_GAP_MAX_FLOOR;

    // Typical bits per networked frame, used to tell "the next tick" from "some
    // tick later" when looking for a chain's derivative.
    long long frameBits = 400;
    if (avgBits != 0.0)
    {
        frameBits = (long long)avgBits;
        if (frameBits < 96)
            frameBits = 96;
    }

    const bool haveRef = args->haveRef;
    const double ref = args->refMaxHoriz;

    int *chosen = NULL;
    int chosenN = 0;
    int bestN = 0;
    double bestPeak = 0.0;
    int identN = 0;
    int attempts = 0;

    for (int round = 0; round < WR_DP_MAX_IDENTIFY_ROUNDS; round++)
    {
        if (args->abort)
        {
            int r = args->abort(args->abortUser);
            if (r)
            {
                ScratchFree(&s);
                free(cands);
                return Stopped(r, cancelled, err, errCap, args->timeoutSeconds,
                               "chain identification");
            }
        }

        int nks = 0;
        for (int k = 0; k < nCands; k++)
            if (!s.banned[k])
                s.ks[nks++] = k;

        const int chainN = LongestSmoothChain(cands, s.ks, nks, gapMax, &s,
                                              s.chain, args->abort,
                                              args->abortUser, &stop);
        if (stop)
        {
            ScratchFree(&s);
            free(cands);
            return Stopped(stop, cancelled, err, errCap, args->timeoutSeconds,
                           "the chain search");
        }
        if (chainN < WR_DP_MIN_CHAIN)
            break;

        const double peak = PeakHorizontalSpeed(cands, s.chain, chainN, dt,
                                                s.lens);
        const double perr = haveRef ? fabs(peak - ref) : 0.0;
        attempts++;

        if (chainN > bestN)
        {
            bestN = chainN;
            bestPeak = peak;
            memcpy(s.best, s.chain, sizeof(int) * (size_t)chainN);
        }

        if (!haveRef)
        {
            chosen = s.chain;           // nothing to check against; take this one
            chosenN = chainN;
            break;
        }
        const double tol = WR_DP_MATCH_ABS > WR_DP_MATCH_REL * ref
                         ? WR_DP_MATCH_ABS : WR_DP_MATCH_REL * ref;
        if (perr <= tol)
        {
            chosen = s.chain;
            chosenN = chainN;
            break;
        }

        // The speed check compares one chain's peak against the WHOLE run's
        // max, so it can only pass when that chain covers most of the run. On a
        // map that launches the player around, the path survives as short
        // fragments and no single one of them can ever satisfy it. Remember the
        // first fragment the derivative test confirms is the origin, and fall
        // back to it below -- harvesting can rebuild the rest of the run from it.
        if (identN == 0)
        {
            double rate;
            int off, ns;
            OriginScore(args->body, args->bodyLen, cands, s.chain, chainN, dt,
                        frameBits, &s, &rate, &off, &ns, args->abort,
                        args->abortUser, &stop);
            if (stop)
            {
                ScratchFree(&s);
                free(cands);
                return Stopped(stop, cancelled, err, errCap,
                               args->timeoutSeconds, "the derivative sweep");
            }
            if (OriginConfirmed(rate, ns))
            {
                identN = chainN;
                memcpy(s.ident, s.chain, sizeof(int) * (size_t)chainN);
                out->info.haveDeriv = true;
                out->info.derivRate = rate;
                out->info.derivOffset = off;
            }
        }

        for (int k = 0; k < chainN; k++)
            s.banned[s.chain[k]] = 1;
    }

    out->info.rounds = attempts;
    if (attempts == 0)
    {
        Fail(err, errCap, "no chain of >=%d linked samples found",
             WR_DP_MIN_CHAIN);
        ScratchFree(&s);
        free(cands);
        return false;
    }

    out->info.confident = chosen != NULL;
    if (chosen)
        strcpy_s(out->info.identifiedBy, sizeof(out->info.identifiedBy), "speed");

    if (!chosen && identN > 0)
    {
        // Not proven by speed, but proven to BE the origin stream: the run
        // carries this chain's own derivative next to it. Take it and let
        // harvesting rebuild the rest; the combined path is re-checked against
        // the recorded max speed below, which can still promote it to confident.
        chosen = s.ident;
        chosenN = identN;
        strcpy_s(out->info.identifiedBy, sizeof(out->info.identifiedBy),
                 "derivative");
    }

    if (!chosen)
    {
        // The speed check is the only exact test we have, so failing it means we
        // cannot prove which chain is the path. But a single smooth chain that
        // spans most of the run's ticks is not something the other fields in the
        // stream produce, so take the longest, mark it low-confidence, and let
        // the caller decide. Better a flagged path than a silently discarded run.
        const double cover = ticks ? ((double)bestN / (double)ticks) : 0.0;
        if (cover < WR_DP_LOW_CONFIDENCE_MIN_COVERAGE)
        {
            Fail(err, errCap,
                 "no chain reproduced the recorded max speed (best %.1f vs "
                 "%.1f), none was confirmed as the origin stream, and the "
                 "longest covers only %.0f%% of ticks",
                 bestPeak, ref, 100.0 * cover);
            ScratchFree(&s);
            free(cands);
            return false;
        }
        chosen = s.best;
        chosenN = bestN;
        strcpy_s(out->info.identifiedBy, sizeof(out->info.identifiedBy),
                 "coverage");
    }

    // Copied out of whichever buffer it landed in, because harvesting is about
    // to reuse `chain` as its own scratch.
    memcpy(s.chosen, chosen, sizeof(int) * (size_t)chosenN);
    chosen = s.chosen;

    // -----------------------------------------------------------------------
    // harvest_segments
    // -----------------------------------------------------------------------
    //
    // The identified chain proves which data is the origin; it does not span a
    // teleport. Go back for the rest of the run -- but only at bit ranges no
    // accepted chain covers.
    //
    // That restriction is what keeps this honest. The velocity and wishvel
    // fields that also form long smooth chains are networked in the same frames
    // as the origin, so they sit inside an already-accepted bit range and cannot
    // be picked up by mistake. Anything found outside those ranges comes from a
    // stretch of the run we have no samples for at all, which is precisely what
    // is missing after a teleport.
    //
    // Chains that survive the range test but imply an impossible speed are
    // banned individually rather than by range, so rejecting one cannot hide a
    // real leg that overlaps it.
    long long spanLo[WR_DP_MAX_SEGMENTS + 1], spanHi[WR_DP_MAX_SEGMENTS + 1];
    int segOff[WR_DP_MAX_SEGMENTS + 1], segLen[WR_DP_MAX_SEGMENTS + 1];
    int nSeg = 1, segTotal = chosenN, nSpans = 1;

    memcpy(s.segFlat, chosen, sizeof(int) * (size_t)chosenN);
    segOff[0] = 0;
    segLen[0] = chosenN;
    spanLo[0] = (long long)cands[chosen[0]].bit;
    spanHi[0] = (long long)cands[chosen[chosenN - 1]].bit;

    for (int iter = 0; iter < WR_DP_MAX_SEGMENTS * 2; iter++)
    {
        int nOut = 0;
        for (int k = 0; k < nCands; k++)
        {
            if (s.hbanned[k])
                continue;
            const long long b = (long long)cands[k].bit;
            bool inside = false;
            for (int sp = 0; sp < nSpans; sp++)
                if (spanLo[sp] <= b && b <= spanHi[sp])
                {
                    inside = true;
                    break;
                }
            if (!inside)
                s.ks[nOut++] = k;
        }
        if (nOut < WR_DP_MIN_SEGMENT)
            break;

        const int chainN = LongestSmoothChain(cands, s.ks, nOut, gapMax, &s,
                                              s.chain, args->abort,
                                              args->abortUser, &stop);
        if (stop)
        {
            ScratchFree(&s);
            free(cands);
            return Stopped(stop, cancelled, err, errCap, args->timeoutSeconds,
                           "the chain search");
        }
        if (chainN < WR_DP_MIN_SEGMENT)
            break;

        int deriv = -1;
        if (frameBits)
        {
            double rate;
            int off, ns;
            OriginScore(args->body, args->bodyLen, cands, s.chain, chainN, dt,
                        frameBits, &s, &rate, &off, &ns, args->abort,
                        args->abortUser, &stop);
            if (stop)
            {
                ScratchFree(&s);
                free(cands);
                return Stopped(stop, cancelled, err, errCap,
                               args->timeoutSeconds, "the derivative sweep");
            }
            // Only treat the score as evidence when it had enough samples to
            // mean something; otherwise leave the decision to the other tests.
            if (ns >= WR_DP_DERIV_MIN_STEPS)
                deriv = OriginConfirmed(rate, ns) ? 1 : 0;
        }

        if (SegmentPlausible(cands, s.chain, chainN, dt, haveRef, ref, deriv,
                             s.lens))
        {
            memcpy(s.segFlat + segTotal, s.chain, sizeof(int) * (size_t)chainN);
            segOff[nSeg] = segTotal;
            segLen[nSeg] = chainN;
            segTotal += chainN;
            nSeg++;
            spanLo[nSpans] = (long long)cands[s.chain[0]].bit;
            spanHi[nSpans] = (long long)cands[s.chain[chainN - 1]].bit;
            nSpans++;
            if (nSeg >= WR_DP_MAX_SEGMENTS)
                break;
        }
        else
        {
            for (int k = 0; k < chainN; k++)
                s.hbanned[s.chain[k]] = 1;
        }
    }

    // Stream order is time order. Stable, by the segment's first bit, matching
    // list.sort(key=...) -- and segments never share a bit, so the stability
    // only matters for reading this and being sure.
    for (int i = 1; i < nSeg; i++)
    {
        const int ko = segOff[i], kl = segLen[i];
        const unsigned int kb = cands[s.segFlat[ko]].bit;
        int j = i - 1;
        while (j >= 0 && cands[s.segFlat[segOff[j]]].bit > kb)
        {
            segOff[j + 1] = segOff[j];
            segLen[j + 1] = segLen[j];
            j--;
        }
        segOff[j + 1] = ko;
        segLen[j + 1] = kl;
    }

    out->info.segments = nSeg;
    out->info.firstSegment = chosenN;

    // Per segment, so a teleport step never counts as movement.
    double peak = 0.0;
    for (int i = 0; i < nSeg; i++)
    {
        const double p = PeakHorizontalSpeed(cands, s.segFlat + segOff[i],
                                             segLen[i], dt, s.lens);
        if (i == 0 || p > peak)
            peak = p;
    }
    out->info.chainMaxHoriz = peak;
    out->info.matchError = haveRef ? fabs(peak - ref) : 0.0;

    // A fragment identified structurally could not be checked against the run's
    // max speed on its own -- it only covers part of the run. The reassembled
    // path can be, and if the fastest moment of the run is now in it, that is
    // the same exact confirmation the clean maps get.
    if (!out->info.confident && haveRef)
    {
        const double tol = WR_DP_MATCH_ABS > WR_DP_MATCH_REL * ref
                         ? WR_DP_MATCH_ABS : WR_DP_MATCH_REL * ref;
        if (out->info.matchError <= tol)
        {
            out->info.confident = true;
            strcat_s(out->info.identifiedBy, sizeof(out->info.identifiedBy),
                     "+speed");
        }
    }

    out->info.samples = segTotal;
    out->info.coverage = ticks ? ((double)segTotal / (double)ticks) : 0.0;

    // sum(...) over a generator, so Neumaier again -- see WrDpSum. Nine
    // thousand step lengths accumulate far enough for a plain += to differ in
    // the last places, and this number is printed.
    WrDpSum len;
    WrDpSumInit(&len);
    for (int i = 0; i < nSeg; i++)
    {
        const int *seg = s.segFlat + segOff[i];
        for (int k = 0; k + 1 < segLen[i]; k++)
        {
            double a[3], b[3];
            CandXyz(cands, seg[k], a);
            CandXyz(cands, seg[k + 1], b);
            WrDpSumAdd(&len, WrDpDist3(a, b));
        }
    }
    out->info.pathLength = WrDpSumEnd(&len);

    // A structurally-identified fragment is only worth keeping if reassembly
    // actually produced a route. Writing a 59-point stub covering 11% of a run
    // would put a meaningless stub of a line in the world, which is worse than
    // admitting we could not extract this one.
    if (!out->info.confident && strcmp(out->info.identifiedBy, "derivative") == 0
        && out->info.coverage < WR_DP_DERIV_MIN_COVERAGE)
    {
        Fail(err, errCap,
             "origin stream identified but only %.0f%% of ticks could be "
             "recovered (%d points) -- too fragmented to be a route",
             100.0 * out->info.coverage, segTotal);
        ScratchFree(&s);
        free(cands);
        return false;
    }

    // Velocity by central difference. The stream does carry velocity floats near
    // the origin, but not at a reliable offset across runs, so we do not use
    // them.
    //
    // Differenced within a segment only: across a teleport the difference is the
    // length of the teleport, which would read as a speed of tens of thousands
    // of units per second and wreck colour-by-speed for the whole run.
    out->points = (WrDpPoint *)malloc(sizeof(WrDpPoint) * (size_t)segTotal);
    if (!out->points)
    {
        Oom(err, errCap, "the point buffer");
        ScratchFree(&s);
        free(cands);
        return false;
    }

    int w = 0;
    for (int i = 0; i < nSeg; i++)
    {
        const int *seg = s.segFlat + segOff[i];
        const int n = segLen[i];
        for (int k = 0; k < n; k++)
        {
            double a[3], b[3], c[3];
            CandXyz(cands, seg[k > 0 ? k - 1 : k], a);
            CandXyz(cands, seg[k + 1 < n ? k + 1 : k], b);
            CandXyz(cands, seg[k], c);
            const double span = ((k > 0 ? 1 : 0) + (k + 1 < n ? 1 : 0)) * dt;

            WrDpPoint *p = &out->points[w++];
            p->x = c[0];
            p->y = c[1];
            p->z = c[2];
            if (span <= 0.0)
            {
                p->vx = p->vy = p->vz = 0.0;
            }
            else
            {
                p->vx = (b[0] - a[0]) / span;
                p->vy = (b[1] - a[1]) / span;
                p->vz = (b[2] - a[2]) / span;
            }
        }
    }
    out->pointCount = w;

    if (args->keepDetail)
    {
        out->chain = (unsigned int *)malloc(sizeof(unsigned int) * (size_t)chosenN);
        out->segBits = (unsigned int *)malloc(sizeof(unsigned int) * (size_t)segTotal);
        out->segOff = (int *)malloc(sizeof(int) * (size_t)nSeg);
        out->segLen = (int *)malloc(sizeof(int) * (size_t)nSeg);
        if (out->chain && out->segBits && out->segOff && out->segLen)
        {
            for (int k = 0; k < chosenN; k++)
                out->chain[k] = cands[chosen[k]].bit;
            out->chainCount = chosenN;
            int at = 0;
            for (int i = 0; i < nSeg; i++)
            {
                out->segOff[i] = at;
                out->segLen[i] = segLen[i];
                for (int k = 0; k < segLen[i]; k++)
                    out->segBits[at++] = cands[s.segFlat[segOff[i] + k]].bit;
            }
            out->segCount = nSeg;
        }
    }

    ScratchFree(&s);
    free(cands);
    return true;
}
