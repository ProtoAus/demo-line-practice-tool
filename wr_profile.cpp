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
static bool Build(WrProfile *out, const WrPoint *pts, int count, int first,
                  const int *breaks, int breakCount,
                  float timeScale, bool timeUsable)
{
    FreeProfile(out);
    memset(out, 0, sizeof(*out));
    out->gravity = g_energy.gravity;
    out->timeUsable = timeUsable;
    out->builtFrom = count;
    out->builtStart = first;
    out->builtBuckets = WrClampI(g_wrProfileBuckets, 16, WR_PROFILE_BUCKETS);
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

            float e = WrEnergyOf(pts[i].pos, pts[i].vel) - e0;
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
        // averaging window; and the run's recovered start, which moves the
        // origin of both axes. Rebuilding on those three is what lets this be
        // cached at all.
        if (run->profile->b && run->profile->gravity == g_energy.gravity &&
            run->profile->builtStart == run->startIndex &&
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
               run->timingTrusted))
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
    if (g_live.b && g_live.builtFrom == count &&
        g_live.gravity == g_energy.gravity &&
        g_live.builtBuckets == WrClampI(g_wrProfileBuckets, 16, WR_PROFILE_BUCKETS))
        return &g_live;

    // No break list: the live recorder keeps none, so the distance test stands
    // in. Your own save-loc loads are exactly the jumps it has to catch.
    // first = 0: your own line starts when you start it, and has no pre-roll.
    if (!Build(&g_live, pts, count, 0, NULL, 0, 1.0f, true))
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
