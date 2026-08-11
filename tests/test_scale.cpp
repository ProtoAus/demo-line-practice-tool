// test_scale.cpp  --  one colour range per leg.
//
// WHAT GOES WRONG WITHOUT THIS
//
// "Scale to what is on" fits the colour ramp to the runs actually enabled. With
// one leg on screen that is obviously right. With two it used to notice the
// mixture and give up, pooling every enabled run into one range -- so enabling
// a stage alongside the main track turned the feature off, silently, and the
// user was left looking at a picture that had quietly reverted to the behaviour
// they had switched on to avoid.
//
// Pooling is not a neutral fallback either. Momentum's legs are not comparable:
// a stage can sit four thousand units above the one before it, so the union of
// two legs' energy bands is mostly empty space, each leg occupies a fraction of
// the ramp, and every line on it comes out one flat colour. That is the exact
// failure auto-scaling exists to fix, reappearing one checkbox later.
//
// So the table below keeps a range per leg, and the two properties worth
// asserting are that a leg's range is fitted to ITS OWN runs and that the
// fallback for "there is nothing usable here" is reachable rather than
// theoretical -- because that fallback is what stops a one-run leg from being
// drawn in a single colour.
//
// THIS LINKS NOTHING
//
// wr_scale.h is static inline, like wr_pacing.h and wr_matrixlife.h. No ImGui,
// no run store, no renderer.
//
// Build:  tests\build.bat
// Run:    tests\test_scale.exe

#include "wr_scale.h"

#include <stdio.h>

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

int main(void)
{
    printf("\n=== wrlines per-leg colour ranges ===\n\n");

    printf("a leg is fitted to its own runs and nobody else's\n");
    {
        WrLegScale t[WR_SCALE_MAX_LEGS];
        WrLegScaleReset(t, WR_SCALE_MAX_LEGS);

        // The shape that broke: a main track down at ground level and a stage
        // four thousand units above it.
        const int mainT = WrLegScaleSlot(t, WR_SCALE_MAX_LEGS, 0, 1);
        WrLegScaleAdd(t, mainT, 100.0f, 900.0f);
        WrLegScaleAdd(t, mainT, 250.0f, 1200.0f);

        const int stage2 = WrLegScaleSlot(t, WR_SCALE_MAX_LEGS, 1, 2);
        WrLegScaleAdd(t, stage2, 4100.0f, 4800.0f);

        Check(mainT != stage2, "two legs get two slots");
        Check(t[mainT].lo == 100.0f && t[mainT].hi == 1200.0f,
              "the main track spans both of its runs");
        Check(t[stage2].lo == 4100.0f && t[stage2].hi == 4800.0f,
              "and the stage is not dragged down to meet it");

        // The pooled answer, for comparison: 100..4800. The stage's own 700
        // units of range would be 15% of it, which is why every line on it came
        // out one colour.
        Check(t[stage2].hi - t[stage2].lo == 700.0f,
              "the stage keeps its own 700 units rather than 15% of 4700");

        Check(WrLegScaleFind(t, WR_SCALE_MAX_LEGS, 0, 1) == mainT,
              "and a leg is found again by its own type and number");
        Check(WrLegScaleFind(t, WR_SCALE_MAX_LEGS, 2, 1) < 0,
              "while a leg nothing was added for is not there at all");
        Check(WrLegScaleCount(t, WR_SCALE_MAX_LEGS) == 2, "two legs, counted");
    }
    printf("\n");

    printf("the same leg twice is the same slot\n");
    {
        // trackType and trackNum together, not either alone: stage 1 and bonus 1
        // share a number, main and stage 1 share nothing but must still not
        // collide.
        WrLegScale t[WR_SCALE_MAX_LEGS];
        WrLegScaleReset(t, WR_SCALE_MAX_LEGS);

        Check(WrLegScaleSlot(t, WR_SCALE_MAX_LEGS, 1, 1) ==
              WrLegScaleSlot(t, WR_SCALE_MAX_LEGS, 1, 1),
              "asking twice does not make two");
        Check(WrLegScaleSlot(t, WR_SCALE_MAX_LEGS, 1, 1) !=
              WrLegScaleSlot(t, WR_SCALE_MAX_LEGS, 2, 1),
              "stage 1 and bonus 1 are different legs");
        Check(WrLegScaleSlot(t, WR_SCALE_MAX_LEGS, 1, 1) !=
              WrLegScaleSlot(t, WR_SCALE_MAX_LEGS, 1, 2),
              "and so are stage 1 and stage 2");
        Check(WrLegScaleCount(t, WR_SCALE_MAX_LEGS) == 3, "three legs, counted");
    }
    printf("\n");

    printf("a range that cannot colour anything is refused\n");
    {
        WrLegScale t[WR_SCALE_MAX_LEGS];
        WrLegScaleReset(t, WR_SCALE_MAX_LEGS);

        // The case this is really about: one run whose speed barely varies. Its
        // range is a point, so every one of its points would land at the same
        // end of the ramp and the whole line would be one colour -- which reads
        // as a broken measurement rather than as a run with nothing to measure.
        const int flat = WrLegScaleSlot(t, WR_SCALE_MAX_LEGS, 0, 1);
        WrLegScaleAdd(t, flat, 500.0f, 500.0f);
        Check(!WrLegScaleUsable(t, flat), "a zero-width range is not usable");

        WrLegScaleAdd(t, flat, 500.0f, 500.5f);
        Check(!WrLegScaleUsable(t, flat), "nor is half a unit of it");

        WrLegScaleAdd(t, flat, 400.0f, 900.0f);
        Check(WrLegScaleUsable(t, flat), "five hundred units is");

        const int empty = WrLegScaleSlot(t, WR_SCALE_MAX_LEGS, 1, 1);
        Check(!WrLegScaleUsable(t, empty),
              "a leg that collected nothing is not usable either");
        Check(!WrLegScaleUsable(t, -1),
              "and neither is the slot you get when the table is full");
    }
    printf("\n");

    printf("the table fills up rather than overflowing\n");
    {
        // Past WR_SCALE_MAX_LEGS the caller falls back to the pooled range,
        // which is coarse and visible rather than wrong and silent. The thing
        // that must NOT happen is a write past the end.
        WrLegScale t[WR_SCALE_MAX_LEGS];
        WrLegScaleReset(t, WR_SCALE_MAX_LEGS);

        bool allFit = true;
        for (int i = 0; i < WR_SCALE_MAX_LEGS; i++)
            if (WrLegScaleSlot(t, WR_SCALE_MAX_LEGS, 1, (unsigned char)(i + 1)) < 0)
                allFit = false;
        Check(allFit, "sixteen legs all get a slot");
        Check(WrLegScaleCount(t, WR_SCALE_MAX_LEGS) == WR_SCALE_MAX_LEGS,
              "and the table is full");

        Check(WrLegScaleSlot(t, WR_SCALE_MAX_LEGS, 2, 99) < 0,
              "the seventeenth is refused rather than squeezed in");

        // The refusal has to be survivable at every call site, since -1 is what
        // the renderer then passes straight back in.
        WrLegScaleAdd(t, -1, 1.0f, 2.0f);
        Check(WrLegScaleCount(t, WR_SCALE_MAX_LEGS) == WR_SCALE_MAX_LEGS,
              "adding to a refused slot changes nothing and does not crash");

        Check(WrLegScaleSlot(t, WR_SCALE_MAX_LEGS, 1, 3) >= 0,
              "and a leg already in the table is still found when it is full");
    }
    printf("\n");

    printf("placing runs within a leg\n");
    {
        // How the renderer uses members/placed: one pass counts, the second
        // hands out places. Two legs interleaved, because the run store is
        // time-sorted ACROSS legs and a 34-second bonus sits above a 52-second
        // main run in it.
        WrLegScale t[WR_SCALE_MAX_LEGS];
        WrLegScaleReset(t, WR_SCALE_MAX_LEGS);

        struct { unsigned char type, num; } runs[] = {
            { 2, 1 },   // bonus, fastest in the whole store
            { 0, 1 },   // main
            { 0, 1 },
            { 2, 1 },
            { 0, 1 },
        };
        const int n = (int)(sizeof(runs) / sizeof(runs[0]));

        for (int i = 0; i < n; i++)
            t[WrLegScaleSlot(t, WR_SCALE_MAX_LEGS, runs[i].type, runs[i].num)].members++;

        int place[8] = {0}, outOf[8] = {0};
        for (int i = 0; i < n; i++)
        {
            const int slot = WrLegScaleFind(t, WR_SCALE_MAX_LEGS, runs[i].type,
                                            runs[i].num);
            place[i] = ++t[slot].placed;
            outOf[i] = t[slot].members;
        }

        Check(place[0] == 1 && outOf[0] == 2, "the bonus places 1st of 2");
        Check(place[1] == 1 && outOf[1] == 3,
              "and the first main run is ALSO 1st -- of 3, on its own leg");
        Check(place[2] == 2 && place[4] == 3, "the other main runs follow it");
        Check(place[3] == 2, "and the second bonus run is 2nd of the bonus");
    }
    printf("\n");

    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
