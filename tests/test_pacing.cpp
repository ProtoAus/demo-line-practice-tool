// test_pacing.cpp  --  run the frame schedule against scripted workloads.
//
// The previous harness fed the limiter a steady workload plus one injected
// hitch, which is not what a game does. Real per-frame cost wobbles constantly
// and occasionally spikes, and the reported symptom -- "capped to 240 and it
// feels like 60" -- is about what those wobbles turn into, not about the average
// rate. So this measures the INTERVAL DISTRIBUTION, and in particular the step
// between consecutive frames, which is what judder actually is.
//
// It also runs the previous catch-up policy side by side, because the claim that
// the new one is better should be a number rather than an opinion.
//
// Build:  cl /nologo /EHsc /I.. tests\test_pacing.cpp /Fe:tests\test_pacing.exe
// Run:    tests\test_pacing.exe

#include "wr_pacing.h"

#include <stdio.h>
#include <math.h>

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-56s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

// A tenth of a microsecond per tick: the same resolution as QueryPerformance-
// Counter on a modern machine (10 MHz), so integer rounding here matches what
// the real limiter sees rather than inventing an error of its own.
static const long long TICKS_PER_MS = 10000;

// A deterministic wobble. Not rand(): the numbers in a comment should be the
// numbers you get when you run it.
static unsigned int g_seed = 12345;
static float Wobble(void)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return (float)((g_seed >> 16) & 0x7FFF) / 32767.0f;   // 0..1
}

struct Stats
{
    double meanMs, minMs, maxMs;
    double worstStepRatio;      // largest ratio between consecutive intervals
    int roughFrames;            // intervals more than 10% off the target
    int count;
};

// `catchUpDenom` reproduces the old policy: 0 means the current one (no
// catch-up), 4 means "may repay up to a quarter of a period per frame".
static Stats Simulate(double targetFps, const float *work, int frames,
                      int catchUpDenom)
{
    const long long period = (long long)(1000.0 * TICKS_PER_MS / targetFps);
    WrPacing p;
    WrPacingReset(&p);

    long long now = 0;
    long long prevRelease = 0;
    double intervals[8192];
    int n = 0;

    for (int i = 0; i < frames; i++)
    {
        now += (long long)(work[i] * TICKS_PER_MS);      // the game's frame work

        long long target = WrPacingTargetFor(&p, now, period);
        long long release = (target > now) ? target : now;

        if (catchUpDenom > 0)
        {
            // The old rule, inlined for comparison.
            p.nextTarget += period;
            long long earliest = release + period - period / catchUpDenom;
            if (p.nextTarget < earliest)
                p.nextTarget = earliest;
        }
        else
        {
            WrPacingAdvance(&p, release, period);
        }

        if (prevRelease != 0 && n < 8192)
            intervals[n++] = (double)(release - prevRelease) / TICKS_PER_MS;
        prevRelease = release;
        now = release;
    }

    Stats s;
    s.count = n;
    s.meanMs = 0.0;
    s.minMs = 1e9;
    s.maxMs = 0.0;
    s.worstStepRatio = 1.0;
    s.roughFrames = 0;
    const double targetMs = 1000.0 / targetFps;
    for (int i = 0; i < n; i++)
    {
        s.meanMs += intervals[i];
        if (intervals[i] < s.minMs) s.minMs = intervals[i];
        if (intervals[i] > s.maxMs) s.maxMs = intervals[i];
        if (fabs(intervals[i] - targetMs) > targetMs * 0.10)
            s.roughFrames++;
        if (i > 0)
        {
            double a = intervals[i - 1], b = intervals[i];
            double r = (a > b) ? a / b : b / a;
            if (r > s.worstStepRatio)
                s.worstStepRatio = r;
        }
    }
    if (n)
        s.meanMs /= n;
    return s;
}

static void Report(const char *label, const Stats &s)
{
    printf("     %-22s mean %6.3f  min %6.3f  max %6.3f  worst step %.2fx  "
           "rough %d/%d\n", label, s.meanMs, s.minMs, s.maxMs,
           s.worstStepRatio, s.roughFrames, s.count);
}

int main(void)
{
    const int FRAMES = 2000;
    static float work[8192];

    printf("\nsteady work, comfortably inside the budget\n");
    {
        // 300 fps of work, capped to 240. The straightforward case.
        for (int i = 0; i < FRAMES; i++)
            work[i] = 3.33f;
        Stats now = Simulate(240.0, work, FRAMES, 0);
        Report("no catch-up", now);
        Check(now.worstStepRatio < 1.01, "consecutive frames never step by 1%");
        Check(now.roughFrames == 0, "no interval is more than 10% off target");
    }

    printf("\nwork that wobbles, as a real frame does\n");
    {
        g_seed = 12345;
        for (int i = 0; i < FRAMES; i++)
            work[i] = 3.0f + Wobble() * 1.0f;            // 3.0 - 4.0 ms
        Stats now = Simulate(240.0, work, FRAMES, 0);
        Report("no catch-up", now);
        Check(now.roughFrames == 0,
              "wobble under the budget is absorbed completely");
    }

    printf("\noccasional spikes -- the case that matters\n");
    {
        // Every 37th frame costs 6 ms: over the 4.167 ms budget, so it cannot be
        // absorbed. The question is only what the NEXT frame does.
        g_seed = 999;
        for (int i = 0; i < FRAMES; i++)
            work[i] = (i % 37 == 0) ? 6.0f : (3.0f + Wobble() * 0.6f);

        Stats old4 = Simulate(240.0, work, FRAMES, 4);
        Stats now = Simulate(240.0, work, FRAMES, 0);
        Report("old, quarter-period", old4);
        Report("no catch-up", now);

        Check(now.minMs > 4.0,
              "no frame is ever released early to make up time");
        Check(old4.minMs < 3.2, "the old policy did release them early");
        Check(now.worstStepRatio < old4.worstStepRatio - 0.4,
              "the step between consecutive frames is materially smaller");
        Check(now.roughFrames * 2 <= old4.roughFrames,
              "and at most half as many intervals are off target at all");
        printf("     step %.2fx -> %.2fx,  off-target intervals %d -> %d\n",
               old4.worstStepRatio, now.worstStepRatio,
               old4.roughFrames, now.roughFrames);
    }

    printf("\nthe game cannot keep up with the cap\n");
    {
        // 5 ms of work against a 4.167 ms budget. The cap is not the limiter
        // here and must not make things worse than simply getting out of the way.
        g_seed = 4242;
        for (int i = 0; i < FRAMES; i++)
            work[i] = 5.0f + Wobble() * 0.2f;
        Stats now = Simulate(240.0, work, FRAMES, 0);
        Report("no catch-up", now);
        Check(now.meanMs > 4.9 && now.meanMs < 5.3,
              "it steps aside rather than fighting the workload");
        Check(now.worstStepRatio < 1.15,
              "and adds no step beyond the workload's own");
    }

    printf("\nlong-run cadence is exact when nothing overruns\n");
    {
        for (int i = 0; i < FRAMES; i++)
            work[i] = 2.0f;
        Stats now = Simulate(240.0, work, FRAMES, 0);
        double err = fabs(now.meanMs - 1000.0 / 240.0);
        printf("     mean %.6f ms against a target of %.6f\n",
               now.meanMs, 1000.0 / 240.0);
        Check(err < 0.001, "mean interval matches the target to a microsecond");
    }

    printf("\na long stall, then normal work\n");
    {
        for (int i = 0; i < FRAMES; i++)
            work[i] = 3.0f;
        work[100] = 250.0f;         // alt-tab, a shader compile, a level load
        Stats now = Simulate(240.0, work, FRAMES, 0);
        Report("no catch-up", now);
        Check(now.minMs > 4.0, "no burst of early frames follows the stall");
        Check(now.maxMs > 249.0 && now.maxMs < 251.0,
              "the stall itself is passed through, not amplified");
    }

    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
