// test_start.cpp  --  where a run actually begins, and where a map's start is.
//
// Two pieces of arithmetic that are easy to get subtly wrong and impossible to
// see going wrong: a graph whose origin is three quarters of a second early
// still looks like a graph.
//
// THE NUMBERS HERE ARE MEASURED, not invented. Across the 1735 .wrpath files on
// the development machine:
//
//   - marker-derived pre-roll runs 0.19 s to 1.11 s across the deciles, 1.74 s
//     at worst, and is never negative
//   - the back-solve (pointCount-1) - runTime/tick agrees with that to within
//     0.05 s on 99.4% of the files that carry both, and 0.15 s on all of them
//   - implied post-roll is a median 0.00 s: the extracted stream ends AT the
//     finish, which is what makes the back-solve valid at all
//   - on a fragmented stream the back-solve does not go slightly wrong, it goes
//     to -1089 s, which is why the plausibility range is really a completeness
//     test
//
// Build:  tests\build.bat
// Run:    tests\test_start.exe

#include "wr_path.h"
#include "wr_start.h"
#include "wr_energy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

// wr_path.cpp and wr_start.cpp reach outside themselves for these. None of them
// matters to the arithmetic under test, and a fake keeps the harness from
// dragging in the whole engine layer.
bool WrCameraForward(Vec3 *out) { if (out) *out = WrVec(1, 0, 0); return true; }
static Vec3 g_fakeCam;
static bool g_haveCam = false;
bool WrCameraOrigin(Vec3 *out)
{
    if (!g_haveCam) return false;
    if (out) *out = g_fakeCam;
    return true;
}

extern void WrPathTestLoad(const WrRun *runs, int count);
extern void WrPathTestFindStart(WrRun *run, int stored, bool storedUsable);

// A run of `n` points at `dt`, whose recorded duration covers the last
// `n - preroll` of them. That is exactly the shape a real .wrpath has.
static WrPoint g_pts[4][8192];
static int g_used = 0;

static WrRun MakeRun(int n, float dt, int prerollTicks, int trackType,
                     int trackNum)
{
    WrRun r;
    memset(&r, 0, sizeof(r));
    r.tickInterval = dt;
    // What CheckTimes computes for a stream with no missing ticks, which is what
    // these are. Set explicitly because a zeroed struct has timeScale 0, and a
    // time check against a scale of zero passes by agreeing that everything is
    // zero -- which is how this fixture hid its own first version.
    r.timeScale = 1.0f;
    r.runTime = (double)(n - 1 - prerollTicks) * (double)dt;
    r.trackType = (unsigned char)trackType;
    r.trackNum = (unsigned char)trackNum;
    r.pointCount = n;
    r.points = g_pts[g_used++ % 4];
    for (int i = 0; i < n; i++)
    {
        // A straight line out along +x from a fixed pad, so a zone fitted to
        // several of these has an obvious right answer.
        r.points[i].pos = WrVec((float)(i - prerollTicks) * 10.0f, 0.0f, 64.0f);
        r.points[i].vel = WrVec(600.0f, 0.0f, 0.0f);
        r.points[i].t = (float)i * dt;
    }
    return r;
}

int main(void)
{
    printf("\n=== wrlines run start and start zones ===\n");
    WrEnergyDefaults();
    WrStartDefaults();

    const float dt = 0.015f;

    // -----------------------------------------------------------------------
    printf("\nthe back-solve finds the pre-roll on a complete stream\n");
    {
        // 0.72 s is the measured median.
        int preroll = 48;                   // 48 * 0.015 = 0.72 s
        WrRun r = MakeRun(4000, dt, preroll, 0, 1);
        WrPathTestFindStart(&r, 0, false);
        Check(r.startTrusted, "it is trusted");
        Check(abs(r.startIndex - preroll) <= 1, "within a tick of the truth");
        Check(fabsf(r.startPos.x) < 11.0f, "startPos is the run's own first point");
    }

    // -----------------------------------------------------------------------
    printf("\na fragmented stream is refused rather than guessed at\n");
    {
        // The real failure: the point stream is far SHORTER than the recorded
        // duration, so the back-solve goes hugely negative. -1089 s was the
        // worst case measured. It must not be believed, and it must not be
        // clamped to something plausible-looking either.
        WrRun r = MakeRun(500, dt, 0, 0, 1);
        r.runTime = 80.0;               // 500 ticks is 7.5 s of points
        WrPathTestFindStart(&r, 0, false);
        Check(!r.startTrusted, "not trusted");
        Check(r.startIndex == 0, "and falls back to index 0, as before it existed");
    }

    // -----------------------------------------------------------------------
    printf("\nan absurd pre-roll is refused too\n");
    {
        // The other direction: more surplus than any demo carries. The worst
        // real one is 4.11 s, and the range stops at 5.
        WrRun r = MakeRun(4000, dt, 1200, 0, 1);    // 18 s of "pre-roll"
        WrPathTestFindStart(&r, 0, false);
        Check(!r.startTrusted, "not trusted");
        Check(r.startIndex == 0, "and falls back to index 0");
    }

    // -----------------------------------------------------------------------
    printf("\nthe markers agree with the back-solve, and confirm it\n");
    {
        int preroll = 48;
        WrRun r = MakeRun(4000, dt, preroll, 0, 1);
        r.flags |= WRPATH_FLAG_MARKERS_OK;
        r.markerCount = 1;
        r.markers[0].pointIndex = (unsigned int)(preroll + 1000);
        r.markers[0].timeReached = 1000.0 * (double)dt;
        WrPathTestFindStart(&r, 0, false);
        Check(r.startTrusted, "trusted");
        Check(abs(r.startIndex - preroll) <= 1, "and lands on the same index");
    }

    // -----------------------------------------------------------------------
    printf("\nwhen they disagree, the measured one wins and trust is withdrawn\n");
    {
        // A marker that says the run started 3 s in, and a duration that says
        // it started 0.72 s in. Both are inside the plausible range, so this is
        // not a range test -- it is the disagreement itself that matters, and
        // the marker carries a time the GAME measured.
        int preroll = 48;
        WrRun r = MakeRun(4000, dt, preroll, 0, 1);
        r.flags |= WRPATH_FLAG_MARKERS_OK;
        r.markerCount = 1;
        r.markers[0].pointIndex = 1200;
        r.markers[0].timeReached = 1000.0 * (double)dt;   // -> start at 200
        WrPathTestFindStart(&r, 0, false);
        Check(r.startIndex == 200, "the marker's answer is used");
        Check(!r.startTrusted, "but it is not trusted, because the two disagree");
    }

    // -----------------------------------------------------------------------
    printf("\nthe extractor's own answer beats both\n");
    {
        WrRun r = MakeRun(4000, dt, 48, 0, 1);
        WrPathTestFindStart(&r, 123, true);
        Check(r.startIndex == 123, "stored index used");
        Check(r.startTrusted, "and trusted");

        // Zero is an answer when the flag says so: a demo with no pre-roll.
        WrRun r2 = MakeRun(4000, dt, 48, 0, 1);
        WrPathTestFindStart(&r2, 0, true);
        Check(r2.startIndex == 0 && r2.startTrusted,
              "a stored zero means zero, not \"unknown\"");

        // Out of range is not.
        WrRun r3 = MakeRun(4000, dt, 48, 0, 1);
        WrPathTestFindStart(&r3, 99999, true);
        Check(r3.startIndex != 99999, "an out-of-range stored index is ignored");
    }

    // -----------------------------------------------------------------------
    printf("\nelapsed time is measured from the run's start, not the recording's\n");
    {
        // The regression this section exists for: once the clock is anchored at
        // startIndex, anything still reading the raw stored `t` is out by the
        // whole pre-roll -- a constant 0.72 s on a median run, and it would look
        // like a plausible delta rather than like a bug.
        int preroll = 48;
        WrRun r = MakeRun(4000, dt, preroll, 0, 1);
        WrPathTestFindStart(&r, 0, false);
        r.timingTrusted = true;

        Check(fabsf(WrRunTimeAt(&r, r.startIndex)) < 1e-4f,
              "zero at the run's own first point");

        // And the last point is the recorded duration, not the duration plus
        // the walk-in. timeScale is fitted over the run span, so applying it to
        // an unshifted t would inflate this by exactly preroll/span.
        float atEnd = WrRunTimeAt(&r, r.pointCount - 1);
        Check(fabsf(atEnd - (float)r.runTime) < 0.02f,
              "and the finish reads the run's recorded time");

        // An untrusted run keeps the old behaviour exactly: startIndex is 0, so
        // this is the raw stored time and nothing has moved under it.
        WrRun u = MakeRun(500, dt, 0, 0, 1);
        u.runTime = 80.0;                       // fragmented: refused
        WrPathTestFindStart(&u, 0, false);
        Check(u.startIndex == 0 &&
              fabsf(WrRunTimeAt(&u, 100) - u.points[100].t * u.timeScale) < 1e-4f,
              "an unrecovered run is unchanged from before");
    }

    // -----------------------------------------------------------------------
    printf("\na zone is fitted per leg, at the runs' own starts\n");
    {
        WrRun runs[3];
        runs[0] = MakeRun(2000, dt, 48, 0, 1);
        runs[1] = MakeRun(2000, dt, 48, 0, 1);
        runs[2] = MakeRun(2000, dt, 48, 2, 4);      // a bonus, elsewhere
        for (int i = 0; i < 3; i++)
        {
            WrPathTestFindStart(&runs[i], 0, false);
            runs[i].enabled = true;
        }
        // Move the bonus a long way off so the two legs cannot be confused.
        for (int i = 0; i < runs[2].pointCount; i++)
            runs[2].points[i].pos.y += 8000.0f;
        runs[2].startPos = runs[2].points[runs[2].startIndex].pos;

        WrPathTestLoad(runs, 3);
        WrStartReset();
        g_haveCam = true;
        g_fakeCam = WrVec(0.0f, 0.0f, 128.0f);
        WrStartTick(g_fakeCam, 0.016f, false);

        Check(WrStartZoneCount() == 2, "two legs, two zones");
        const WrStartZone *z = WrStartZoneAt(0);
        Check(z != NULL, "the first exists");
        if (z)
        {
            Check(fabsf(z->centre.x) < 12.0f,
                  "centred on the run start, not on point 0");
            Check(z->outDir.x > 0.9f, "and points the way the runs left");
            Check(z->trusted == 2 && z->members == 2, "both members placed");
            Check(!z->approx, "so it is not marked approximate");
        }
    }

    // -----------------------------------------------------------------------
    printf("\nwith nothing loaded it is inert, and says why\n");
    {
        WrPathTestLoad(NULL, 0);
        WrStartReset();
        WrStartTick(WrVec(0.0f, 0.0f, 0.0f), 0.016f, false);
        Check(WrStartZoneCount() == 0, "no zones");
        Check(WrStartStateNow() == WR_START_AWAY, "and no state");
        Check(WrStartWhyNot()[0] != '\0', "and it says so in words");
        const WrStartZone *w = NULL;
        Check(!WrStartTakeCrossed(&w), "nothing can fire");
    }

    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
