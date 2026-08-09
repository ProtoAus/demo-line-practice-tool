// wr_profile.cpp  --  see wr_profile.h.

#include "wr_profile.h"
#include "wr_path.h"
#include "wr_energy.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

// How many profiles may be built in one frame.
//
// A build is one pass over every point of one run. That is 38 751 points on the
// longest run measured, and with two dozen runs enabled doing them all at once
// is a visible hitch on the frame the tab is opened -- which is the worst
// possible frame to hitch on, because it is the one where the user is looking.
// Six is imperceptible and fills a full plot in under half a second.
#define BUILDS_PER_FRAME 6

static int g_budget = 0;        // reset each time the pending count is asked for
static WrProfile g_live;
static bool g_liveInit = false;

int g_wrProfileBuckets = WR_PROFILE_BUCKETS;
bool g_wrProfileDespike = true;
float g_wrProfileLiveSmooth = 0.20f;

// Scratch for the smoothing pass, grown once and kept.
//
// Two floats per point over a 32 768-point live buffer is 256 KB, and
// WrProfileLive rebuilds every frame while you are recording -- so this is
// allocated once rather than malloc'd and freed sixty to three hundred times a
// second. Single-threaded by the same rule as everything else here: profiles are
// built from the render thread and nowhere else.
static float *g_scratch = NULL;
static int g_scratchCap = 0;

static bool ScratchEnsure(int floats)
{
    if (floats <= g_scratchCap)
        return true;
    int want = floats + floats / 2;
    float *grown = (float *)realloc(g_scratch, sizeof(float) * (size_t)want);
    if (!grown)
        return false;
    g_scratch = grown;
    g_scratchCap = want;
    return true;
}

// A centred mean over a fixed span of time.
//
// Two pointers and a running sum, so it is one pass whatever the window. The
// pointers only ever move forward, which is what keeps it linear -- and it is
// also what makes it safe when `t` is not monotone, which the live series can
// briefly fail to be when a save-loc that did not move you puts the clock back.
// At such a point the window is simply whichever samples the pointers were
// holding rather than the exact span asked for; it cannot read out of range and
// it cannot loop. The forced expansion below guarantees the window always
// contains the sample being smoothed.
static void SmoothOverTime(const float *in, float *out, const WrPoint *pts,
                           int n, float seconds)
{
    float half = seconds * 0.5f;
    int a = 0, b = 0;
    double sum = 0.0;

    for (int i = 0; i < n; i++)
    {
        while (b < n && pts[b].t <= pts[i].t + half) { sum += in[b]; b++; }
        while (b <= i)                  { sum += in[b]; b++; }
        while (a < i && pts[a].t < pts[i].t - half) { sum -= in[a]; a++; }

        int cnt = b - a;
        out[i] = (cnt > 0) ? (float)(sum / (double)cnt) : in[i];
    }
}

// ---------------------------------------------------------------------------

static void FreeProfile(WrProfile *p)
{
    if (!p)
        return;
    free(p->b);
    p->b = NULL;
    p->n = 0;
    p->builtFrom = 0;
}

// The one place a profile is actually computed, shared by stored runs and the
// live line so the two are built by identical rules and can be laid over each
// other honestly.
//
// `breaks` may be NULL, in which case a step longer than WR_TELEPORT_UNITS is
// treated as a teleport directly. Stored runs pass their recorded break list --
// found at full resolution at load, which is exact -- and the live line has no
// such list, so it gets the distance test. Both must skip the jump: adding the
// chord of a save-loc load to the distance axis would put a kilometre of "path"
// where the player crossed the map in one frame.
// `first` is the point the RUN starts at, which is not point 0 -- a demo carries
// roughly two seconds of pre-roll before the timer starts. Everything here used
// to originate at index 0, so the energy zero was taken while the player was
// still walking into the start zone and both axes began there too. That is the
// whole reason a graph could look wrong for a run that was fine. See startIndex
// in wr_path.h.
//
// Runs whose start could not be recovered pass first = 0 and get exactly the
// behaviour they had before.
// The energy at one point, with two-tick transients taken out.
//
// WHAT IS BEING REMOVED, AND HOW IT IS KNOWN TO BE NOISE
//
// Momentum's recorded velocity jumps for a tick or two and comes straight back,
// at fixed places on a map. Measured over the 14 surf_fiellu bonus-4 runs on
// this machine: 208 single-tick energy jumps above 150 units, of which 206 are
// in the SPEED term and only 2 in height -- so it is not ducking, which is the
// obvious suspect and would move the height. 88% of the excursions are back
// where they started within two ticks. Across the whole library it is 0.13% of
// 7.4 million ticks, but 76% of runs carry at least one.
//
// The proof that they are not real: they happen in free fall, where energy is
// conserved by definition. One run sits at 1476 units for twenty ticks, reads
// 2054 for exactly two, and returns to 1470, while its height falls smoothly at
// 21 units a tick throughout. Nothing can gain 577 units and give them back
// inside 30 ms under gravity alone.
//
// WHY A MEDIAN AND NOT AN AVERAGE
//
// A median of five discards up to two outliers in its window and passes
// everything else through UNCHANGED. An average would do the opposite of what is
// wanted: it would spread that 577-unit spike across 75 ms rather than remove
// it, and it would round off the genuine ramp exits either side -- which are the
// steepest real features on the curve and the entire reason to look at it.
//
// Five samples, not more: it is the smallest odd window that survives two
// adjacent bad ticks, and it stays under the shortest real feature worth seeing.
static float EnergyAt(const WrPoint *pts, int count, int i, int lo, float e0)
{
    float raw = WrEnergyOf(pts[i].pos, pts[i].vel) - e0;
    if (!g_wrProfileDespike)
        return raw;

    // Gathered on the fly rather than kept in an array. WrEnergyOf is a dot
    // product and a divide with no sqrt, and a stored array would go stale the
    // moment the gravity slider moved -- the same reason wr_path.cpp gives for
    // not caching per-point energy at all.
    float v[5];
    int n = 0;
    for (int k = i - 2; k <= i + 2; k++)
    {
        if (k < lo || k >= count)
            continue;
        v[n++] = WrEnergyOf(pts[k].pos, pts[k].vel) - e0;
    }
    if (n < 3)
        return raw;         // too near an end to have neighbours either side

    // Insertion sort of at most five floats.
    for (int a = 1; a < n; a++)
    {
        float key = v[a];
        int b = a - 1;
        while (b >= 0 && v[b] > key) { v[b + 1] = v[b]; b--; }
        v[b + 1] = key;
    }
    return v[n / 2];
}

static bool Build(WrProfile *out, const WrPoint *pts, int count, int first,
                  const int *breaks, int breakCount,
                  float timeScale, bool timeUsable, float smoothSeconds)
{
    FreeProfile(out);
    memset(out, 0, sizeof(*out));
    out->gravity = g_energy.gravity;
    out->timeUsable = timeUsable;
    out->builtFrom = count;
    out->builtStart = first;
    out->builtBuckets = WrClampI(g_wrProfileBuckets, 16, WR_PROFILE_BUCKETS);
    out->builtDespike = g_wrProfileDespike;
    out->builtSmooth = smoothSeconds;
    out->despiked = 0;
    if (!pts || count < 4)
        return false;
    if (first < 0 || first > count - 4)
        first = 0;

    int want = WrClampI(g_wrProfileBuckets, 16, WR_PROFILE_BUCKETS);
    int span = count - first;
    int n = span < want ? span : want;
    out->b = (WrProfileBucket *)malloc(sizeof(WrProfileBucket) * n);
    if (!out->b)
        return false;
    out->n = n;

    const float e0 = WrEnergyOf(pts[first].pos, pts[first].vel);
    const float t0 = pts[first].t;
    float d = 0.0f;
    int nextBreak = 0;

    // The optional low-pass, built once over the whole series before bucketing
    // rather than inside the bucket loop -- a centred window needs neighbours on
    // both sides, and a bucket does not have them at its edges.
    //
    // Applied on top of the despike filter, not instead of it: they answer
    // different questions. The median removes two-tick lies; this reduces the
    // width of an estimate. A stored run passes 0 here and is bit-identical to
    // what it was before this existed.
    const float *smoothed = NULL;
    if (smoothSeconds > 1e-4f && span >= 3 && ScratchEnsure(span * 2))
    {
        float *raw = g_scratch;
        float *sm = g_scratch + span;
        for (int i = 0; i < span; i++)
            raw[i] = EnergyAt(pts, count, first + i, first, e0);
        SmoothOverTime(raw, sm, pts + first, span, smoothSeconds);
        smoothed = sm;
    }

    out->eMin = 0.0f;
    out->eMax = 0.0f;

    for (int k = 0; k < n; k++)
    {
        // Ranges by index, so a bucket is a fixed slice of TIME. Splitting by
        // distance instead would give a stationary player a bucket of his own
        // and squeeze a fast section into one column.
        int a = first + (int)(((long long)k * span) / n);
        int b = first + (int)(((long long)(k + 1) * span) / n);
        if (b <= a)
            b = a + 1;
        if (b > count)
            b = count;

        float lo = 1e30f, hi = -1e30f, last = 0.0f;

        for (int i = a; i < b; i++)
        {
            // i > first, not i > 0: the chord from the last pre-roll point into
            // the start would otherwise be charged to the run's distance axis.
            if (i > first)
            {
                bool jump;
                if (breaks)
                {
                    while (nextBreak < breakCount && breaks[nextBreak] < i - 1)
                        nextBreak++;
                    jump = (nextBreak < breakCount && breaks[nextBreak] == i - 1);
                }
                else
                {
                    jump = WrDistSqr(pts[i - 1].pos, pts[i].pos) >
                           WR_TELEPORT_UNITS * WR_TELEPORT_UNITS;
                }
                if (!jump)
                {
                    float dx = pts[i].pos.x - pts[i - 1].pos.x;
                    float dy = pts[i].pos.y - pts[i - 1].pos.y;
                    float dz = pts[i].pos.z - pts[i - 1].pos.z;
                    d += sqrtf(dx * dx + dy * dy + dz * dz);
                }
            }

            // Filtered BEFORE the band, not just before the drawn value. The
            // faint envelope around each curve is min/max within the bucket, so
            // it is where a two-tick spike shows most -- `last` can miss one
            // entirely while the band cannot.
            float e = smoothed ? smoothed[i - first]
                               : EnergyAt(pts, count, i, first, e0);
            if (g_wrProfileDespike)
            {
                float raw = WrEnergyOf(pts[i].pos, pts[i].vel) - e0;
                float diff = e - raw;
                if (diff < 0.0f) diff = -diff;
                if (diff > WR_DESPIKE_NOTE)
                    out->despiked++;
            }
            if (e < lo) lo = e;
            if (e > hi) hi = e;
            last = e;
        }

        out->b[k].d = d;
        out->b[k].t = (pts[b - 1].t - t0) * timeScale;
        out->b[k].e = last;
        out->b[k].eMin = lo;
        out->b[k].eMax = hi;

        if (lo < out->eMin) out->eMin = lo;
        if (hi > out->eMax) out->eMax = hi;
    }

    out->dTotal = d;
    out->tTotal = out->b[n - 1].t;
    return true;
}

// ---------------------------------------------------------------------------

const WrProfile *WrProfileFor(WrRun *run)
{
    if (!run || run->pointCount < 4)
        return NULL;

    if (run->profile)
    {
        // What can move under a built profile: gravity, which moves the whole
        // curve (E = z + |v|^2/2g); the bucket count, which is the graph's
        // averaging window; the run's recovered start, which moves the origin of
        // both axes; and the transient filter, which changes the samples
        // themselves. Rebuilding on those four is what lets this be cached at
        // all -- and a toggle that did not appear here would leave the old curve
        // on screen while the checkbox said otherwise.
        // builtSmooth is NOT tested here, and deliberately: a stored run always
        // builds with 0, so the field can never differ and testing it would only
        // invite the belief that the live smoothing reaches these curves.
        if (run->profile->b && run->profile->gravity == g_energy.gravity &&
            run->profile->builtStart == run->startIndex &&
            run->profile->builtDespike == g_wrProfileDespike &&
            run->profile->builtBuckets ==
                WrClampI(g_wrProfileBuckets, 16, WR_PROFILE_BUCKETS))
            return run->profile;
    }
    else
    {
        run->profile = (WrProfile *)calloc(1, sizeof(WrProfile));
        if (!run->profile)
            return NULL;
    }

    if (g_budget <= 0)
        return NULL;
    g_budget--;

    if (!Build(run->profile, run->points, run->pointCount, run->startIndex,
               run->breaks, run->breakCount,
               run->timingTrusted ? run->timeScale : 1.0f,
               run->timingTrusted, 0.0f))
        return NULL;
    return run->profile;
}

int WrProfilePending(void)
{
    g_budget = BUILDS_PER_FRAME;

    int owed = 0;
    for (int i = 0; i < WrRunCount(); i++)
    {
        const WrRun *r = WrRunAt(i);
        if (!r || !r->enabled || r->pointCount < 4)
            continue;
        // Must agree with the cache test in WrProfileFor, or the budget is spent
        // on rebuilds this never counted and the plot fills in slowly.
        if (!r->profile || !r->profile->b ||
            r->profile->gravity != g_energy.gravity ||
            r->profile->builtStart != r->startIndex ||
            r->profile->builtDespike != g_wrProfileDespike ||
            r->profile->builtBuckets !=
                WrClampI(g_wrProfileBuckets, 16, WR_PROFILE_BUCKETS))
            owed++;
    }
    return owed;
}

const WrProfile *WrProfileLive(void)
{
    int count = 0;
    const WrPoint *pts = WrLivePoints(&count);
    if (!pts || count < 4)
        return NULL;

    if (!g_liveInit)
    {
        memset(&g_live, 0, sizeof(g_live));
        g_liveInit = true;
    }

    // Rebuilt whenever it has grown, which while recording is every frame. The
    // whole buffer is 32 768 points and a rebuild is a fraction of a
    // millisecond; keeping an incremental version in step with a ring buffer
    // that also gets cleared on a map change is not worth that.
    float smooth = g_wrProfileLiveSmooth;
    if (!(smooth > 0.0f)) smooth = 0.0f;
    if (smooth > 2.0f)    smooth = 2.0f;

    if (g_live.b && g_live.builtFrom == count &&
        g_live.gravity == g_energy.gravity &&
        g_live.builtDespike == g_wrProfileDespike &&
        g_live.builtSmooth == smooth &&
        g_live.builtBuckets == WrClampI(g_wrProfileBuckets, 16, WR_PROFILE_BUCKETS))
        return &g_live;

    // No break list: the live recorder keeps none, so the distance test stands
    // in. Your own save-loc loads are exactly the jumps it has to catch.
    // first = 0: your own line starts when you start it, and has no pre-roll.
    if (!Build(&g_live, pts, count, 0, NULL, 0, 1.0f, true, smooth))
        return NULL;
    return &g_live;
}

void WrProfileFree(WrRun *run)
{
    if (!run || !run->profile)
        return;
    FreeProfile(run->profile);
    free(run->profile);
    run->profile = NULL;
}

void WrProfileShutdown(void)
{
    FreeProfile(&g_live);
    g_liveInit = false;
    free(g_scratch);
    g_scratch = NULL;
    g_scratchCap = 0;
}

bool WrProfileAt(const WrProfile *p, float x, bool byTime, float *e)
{
    if (!p || !p->b || p->n < 2)
        return false;

    // Binary search: the axis is non-decreasing in both modes -- distance only
    // ever accumulates, and a run's clock only runs forward.
    int lo = 0, hi = p->n - 1;
    float x0 = byTime ? p->b[0].t : p->b[0].d;
    float x1 = byTime ? p->b[hi].t : p->b[hi].d;
    if (x < x0 || x > x1)
        return false;

    while (hi - lo > 1)
    {
        int mid = (lo + hi) / 2;
        float xm = byTime ? p->b[mid].t : p->b[mid].d;
        if (xm <= x) lo = mid;
        else         hi = mid;
    }

    float xa = byTime ? p->b[lo].t : p->b[lo].d;
    float xb = byTime ? p->b[hi].t : p->b[hi].d;
    float f = (xb - xa) > 1e-6f ? (x - xa) / (xb - xa) : 0.0f;
    if (e)
        *e = p->b[lo].e + (p->b[hi].e - p->b[lo].e) * f;
    return true;
}
