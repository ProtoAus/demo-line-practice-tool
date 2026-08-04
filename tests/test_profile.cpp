// test_profile.cpp  --  the energy profiles behind the Graphs tab.
//
// A plot is a place where being subtly wrong is cheap to ship and expensive to
// notice: nobody double-checks a curve, and a curve that has quietly dropped a
// spike or added a kilometre of distance still looks like a curve. The three
// things asserted here are exactly the three that would have been invisible.
//
//   1. A teleport must not become distance. A save-loc load crosses the map in
//      one sample, and adding that chord puts a kilometre of "path" on the axis
//      where the player travelled none.
//   2. Bucketing must not lose an excursion. Taking every Nth point is the
//      obvious way to fit 38 751 points into a plot and it silently deletes
//      whole ramp exits; the min/max per bucket is the fix, and this measures
//      the difference rather than asserting the intent.
//   3. A curve must end where the run ends. Reading past it has to say nothing,
//      not repeat the final value as a flat line -- which is what a hover
//      comparison across runs of different lengths would otherwise show.
//
// Links wr_profile.cpp for real, with the four things it reaches outside itself
// for stubbed below.
//
// Build:  tests\build.bat
// Run:    tests\test_profile.exe

#include "wr_profile.h"
#include "wr_path.h"
#include "wr_energy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// --- stubs ------------------------------------------------------------------
//
// wr_profile.cpp touches the run store only to count what still needs building,
// and the live line only to read it. Both are trivially stubbed, which keeps
// this test off wr_path.cpp and its map loading, engine pokes and file IO.

static WrRun *g_stubRuns = NULL;
static int g_stubRunCount = 0;
static const WrPoint *g_stubLive = NULL;
static int g_stubLiveCount = 0;

int WrRunCount(void) { return g_stubRunCount; }
WrRun *WrRunAt(int i)
{
    return (i >= 0 && i < g_stubRunCount) ? &g_stubRuns[i] : NULL;
}
const WrPoint *WrLivePoints(int *count)
{
    if (count) *count = g_stubLiveCount;
    return g_stubLive;
}

bool WrCameraForward(Vec3 *out)
{
    if (out) *out = WrVec(1.0f, 0.0f, 0.0f);
    return true;
}

// --- harness ----------------------------------------------------------------

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

static const float G = 800.0f;
static const float TICK = 1.0f / 66.0f;

// A run of `n` points along +x at a steady speed, level. Energy is constant, so
// anything the profile shows moving is the profile's doing and not the data's.
static void MakeRun(WrRun *run, int n, float speed)
{
    memset(run, 0, sizeof(*run));
    run->points = (WrPoint *)calloc(n, sizeof(WrPoint));
    run->pointCount = n;
    run->tickInterval = TICK;
    run->timeScale = 1.0f;
    run->timingTrusted = true;
    run->enabled = true;
    for (int i = 0; i < n; i++)
    {
        run->points[i].pos = WrVec(speed * TICK * i, 0.0f, 1000.0f);
        run->points[i].vel = WrVec(speed, 0.0f, 0.0f);
        run->points[i].t = TICK * i;
    }
}

int main(void)
{
    WrEnergyDefaults();
    g_energy.gravity = G;

    printf("\n=== wrlines energy profiles ===\n");

    // -----------------------------------------------------------------------
    printf("\na profile starts at zero and measures its own run\n");
    {
        WrRun run;
        MakeRun(&run, 4000, 1500.0f);
        g_stubRuns = &run;
        g_stubRunCount = 1;

        WrProfilePending();                 // hands out the build budget
        const WrProfile *p = WrProfileFor(&run);

        Check(p != NULL, "a run with points gets a profile");
        if (p)
        {
            float expect = 1500.0f * TICK * (4000 - 1);
            printf("     %d buckets, %.0f units long, e from %.1f to %.1f\n",
                   p->n, p->dTotal, p->eMin, p->eMax);
            Check(p->n == WR_PROFILE_BUCKETS,
                  "a long run is bucketed down to the fixed count");
            Check(fabsf(p->b[0].e) < 1.0f,
                  "the curve starts at zero, not at absolute height");
            Check(fabsf(p->dTotal - expect) < 2.0f,
                  "distance is the path length, to within a point");
            Check(fabsf(p->eMax - p->eMin) < 1.0f,
                  "constant speed and height is a flat curve");
        }
        WrProfileFree(&run);
        free(run.points);
    }

    // -----------------------------------------------------------------------
    printf("\na teleport is not distance travelled\n");
    {
        // Two 1000-point legs 8000 units apart -- a save-loc load, which is the
        // case that put a kilometre on the axis.
        const int n = 2000;
        WrRun run;
        MakeRun(&run, n, 1500.0f);
        for (int i = n / 2; i < n; i++)
            run.points[i].pos.x += 8000.0f;

        int brk = n / 2 - 1;
        run.breaks = &brk;
        run.breakCount = 1;

        g_stubRuns = &run;
        g_stubRunCount = 1;
        WrProfilePending();
        const WrProfile *p = WrProfileFor(&run);

        float honest = 1500.0f * TICK * (n - 2);    // every step but the jump
        printf("     %.0f units with the break declared, %.0f if the jump counted\n",
               p ? p->dTotal : 0.0f, honest + 8000.0f);
        Check(p && fabsf(p->dTotal - honest) < 4.0f,
              "the 8000-unit jump adds nothing to the axis");

        // And the same run with no break list, which is how the live line
        // arrives -- the distance test has to catch it on its own.
        run.breaks = NULL;
        run.breakCount = 0;
        WrProfileFree(&run);
        WrProfilePending();
        const WrProfile *q = WrProfileFor(&run);
        Check(q && fabsf(q->dTotal - honest) < 4.0f,
              "and is caught without a break list too, as your own line is");

        WrProfileFree(&run);
        free(run.points);
    }

    // -----------------------------------------------------------------------
    printf("\nbucketing keeps an excursion that a strided sample deletes\n");
    {
        // A run with one short, sharp energy spike: 3 points out of 4000, well
        // inside the 8.3-point stride a 480-bucket plot implies, and placed at
        // a bucket's start so the point a stride would sample misses it. That
        // placement is the whole failure mode -- a spike wider than the stride
        // is caught by accident, and the ones that vanish are the narrow ones.
        const int n = 4000;
        WrRun run;
        MakeRun(&run, n, 1500.0f);
        const int at = 2000, wide = 3;
        for (int i = at; i < at + wide; i++)
            run.points[i].vel = WrVec(2600.0f, 0.0f, 0.0f);

        float spike = WrEnergyOf(run.points[at].pos, run.points[at].vel) -
                      WrEnergyOf(run.points[0].pos, run.points[0].vel);

        g_stubRuns = &run;
        g_stubRunCount = 1;
        WrProfilePending();
        const WrProfile *p = WrProfileFor(&run);

        // What every-Nth-point would have reported: the bucket's last value,
        // which is what a mid-line plot draws.
        float bestMid = 0.0f, bestMax = 0.0f;
        for (int k = 0; p && k < p->n; k++)
        {
            if (p->b[k].e > bestMid) bestMid = p->b[k].e;
            if (p->b[k].eMax > bestMax) bestMax = p->b[k].eMax;
        }
        printf("     spike is %.0f units: the band shows %.0f, a mid-line shows %.0f\n",
               spike, bestMax, bestMid);
        Check(p && fabsf(bestMax - spike) < 1.0f,
              "the band reaches the spike exactly");
        Check(p && bestMid < spike * 0.01f,
              "and a mid-line alone would have shown nothing at all");

        WrProfileFree(&run);
        free(run.points);
    }

    // -----------------------------------------------------------------------
    printf("\nreading past the end of a run says nothing\n");
    {
        WrRun run;
        MakeRun(&run, 4000, 1500.0f);
        g_stubRuns = &run;
        g_stubRunCount = 1;
        WrProfilePending();
        const WrProfile *p = WrProfileFor(&run);

        float e = -12345.0f;
        Check(p && WrProfileAt(p, p->dTotal * 0.5f, false, &e),
              "a distance inside the run reads a value");
        Check(fabsf(e) < 1.0f, "and it is the right one");

        e = -12345.0f;
        Check(p && !WrProfileAt(p, p->dTotal * 2.0f, false, &e),
              "twice its length reads nothing at all");
        Check(e == -12345.0f, "and leaves the caller's value alone");

        Check(p && !WrProfileAt(p, -50.0f, false, &e),
              "before the start reads nothing either");

        WrProfileFree(&run);
        free(run.points);
    }

    // -----------------------------------------------------------------------
    printf("\ngravity moving rebuilds rather than going stale\n");
    {
        WrRun run;
        MakeRun(&run, 4000, 1500.0f);
        // A climb, so the energy actually depends on the speed term and moving
        // gravity has to change the answer.
        for (int i = 0; i < 4000; i++)
            run.points[i].vel = WrVec(1500.0f, 0.0f, 200.0f);

        g_stubRuns = &run;
        g_stubRunCount = 1;
        WrProfilePending();
        const WrProfile *p = WrProfileFor(&run);
        Check(p && p->gravity == G, "a profile records the gravity it was built with");

        Check(WrProfilePending() == 0, "and is not owed a rebuild while it holds");

        g_energy.gravity = 600.0f;
        int owed = WrProfilePending();
        printf("     gravity %.0f -> %.0f leaves %d rebuild(s) owed\n",
               G, g_energy.gravity, owed);
        Check(owed == 1, "changing gravity owes the run a rebuild");

        const WrProfile *q = WrProfileFor(&run);
        Check(q && q->gravity == 600.0f, "and asking for it does the rebuild");

        g_energy.gravity = G;
        WrProfileFree(&run);
        free(run.points);
    }

    // -----------------------------------------------------------------------
    printf("\nthe build budget bounds a frame, and finishes over a few\n");
    {
        // Twelve enabled runs, one frame's budget. The tab must never do all of
        // them at once -- that is a visible hitch on the frame the tab opens,
        // which is the one frame the user is definitely looking at.
        const int runs = 12;
        WrRun *rr = (WrRun *)calloc(runs, sizeof(WrRun));
        for (int i = 0; i < runs; i++)
            MakeRun(&rr[i], 2000, 1500.0f);
        g_stubRuns = rr;
        g_stubRunCount = runs;

        int frames = 0, built = 0;
        while (built < runs && frames < 20)
        {
            WrProfilePending();
            int thisFrame = 0;
            for (int i = 0; i < runs; i++)
                if (WrProfileFor(&rr[i]) && rr[i].profile->b)
                    thisFrame++;
            frames++;
            if (thisFrame > built) built = thisFrame;
        }
        printf("     %d runs ready after %d frames\n", built, frames);
        Check(built == runs, "every enabled run is built eventually");
        Check(frames >= 2, "but not all in one frame");

        for (int i = 0; i < runs; i++)
        {
            WrProfileFree(&rr[i]);
            free(rr[i].points);
        }
        free(rr);
    }

    // -----------------------------------------------------------------------
    printf("\nyour own line is profiled by the same rules\n");
    {
        const int n = 900;
        WrPoint *pts = (WrPoint *)calloc(n, sizeof(WrPoint));
        for (int i = 0; i < n; i++)
        {
            pts[i].pos = WrVec(10.0f * i, 0.0f, 1200.0f);
            pts[i].vel = WrVec(660.0f, 0.0f, 0.0f);
            pts[i].t = 0.015f * i;
        }
        // A save-loc load half way through, with no break list to warn it.
        for (int i = n / 2; i < n; i++)
            pts[i].pos.x += 9000.0f;

        g_stubLive = pts;
        g_stubLiveCount = n;

        const WrProfile *p = WrProfileLive();
        Check(p != NULL, "the live line gets a profile");
        Check(p && fabsf(p->dTotal - 10.0f * (n - 2)) < 4.0f,
              "and your own save-loc load is not distance either");

        // Growing by one point must produce a new answer, not a cached one.
        int before = p ? p->builtFrom : 0;
        g_stubLiveCount = n - 100;
        const WrProfile *q = WrProfileLive();
        Check(q && q->builtFrom != before,
              "and it rebuilds when the recording changes length");

        free(pts);
        g_stubLive = NULL;
        g_stubLiveCount = 0;
        WrProfileShutdown();
    }

    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
