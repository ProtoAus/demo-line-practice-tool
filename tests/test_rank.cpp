// test_rank.cpp  --  where a run places, and on which leg.
//
// The failure this exists to catch is not a crash and would not look like a bug
// on screen: it would look like a winning line. Momentum records a separate run
// per stage and per bonus, and the run store is sorted by time across all of
// them, so the quickest time in a map's files is routinely a stage rather than
// the main track. Rank without splitting by leg and a 34-second bonus takes
// first place from a main track it was never racing -- and the result is
// entirely plausible right up until you notice it is on the wrong line.
//
// The numbers below are the real shape of bhop_futile as extracted here: twenty
// main runs between 52.85 and 54.34 seconds, plus one bonus at 33.97.
//
// Build:  tests\build.bat
// Run:    tests\test_rank.exe

#include "wr_path.h"

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

// wr_path.cpp reaches outside itself for these; none matter to ranking.
bool WrCameraForward(Vec3 *out) { if (out) *out = WrVec(1, 0, 0); return true; }

// The store is private to wr_path.cpp and filled by loading files, which a unit
// test has no business doing. WrPathTestLoad hands it a table directly -- the
// same array the loader would have produced, minus the IO.
extern void WrPathTestLoad(const WrRun *runs, int count);

static WrRun Make(double t, int trackType, int trackNum, const char *who)
{
    WrRun r;
    memset(&r, 0, sizeof(r));
    r.runTime = t;
    r.trackType = (unsigned char)trackType;
    r.trackNum = (unsigned char)trackNum;
    r.pointCount = 100;         // anything >= 2 counts as a real run
    strcpy_s(r.player, sizeof(r.player), who);
    return r;
}

int main(void)
{
    printf("\n=== wrlines run ranking ===\n");

    // -----------------------------------------------------------------------
    printf("\none leg, in order\n");
    {
        WrRun runs[4] = {
            Make(37.17, 0, 1, "first"),
            Make(39.19, 0, 1, "second"),
            Make(40.29, 0, 1, "third"),
            Make(79.08, 0, 1, "last"),
        };
        WrPathTestLoad(runs, 4);

        int outOf = 0;
        Check(WrRunRankInTrack(WrRunAt(0), &outOf) == 1, "the fastest is 1st");
        Check(outOf == 4, "and knows how big the field is");
        Check(WrRunRankInTrack(WrRunAt(1), NULL) == 2, "2nd");
        Check(WrRunRankInTrack(WrRunAt(2), NULL) == 3, "3rd");
        Check(WrRunRankInTrack(WrRunAt(3), NULL) == 4, "4th");
    }

    // -----------------------------------------------------------------------
    printf("\na bonus cannot take first place from the main track\n");
    {
        // bhop_futile as it actually sits on disk here.
        WrRun runs[4] = {
            Make(33.97, 2, 1, "the bonus"),
            Make(52.85, 0, 1, "main best"),
            Make(53.40, 0, 1, "main mid"),
            Make(54.34, 0, 1, "main worst"),
        };
        WrPathTestLoad(runs, 4);

        int mainField = 0, bonusField = 0;
        int bonusRank = WrRunRankInTrack(WrRunAt(0), &bonusField);
        int mainRank = WrRunRankInTrack(WrRunAt(1), &mainField);
        printf("     the 33.97 bonus is %d of %d; the 52.85 main is %d of %d\n",
               bonusRank, bonusField, mainRank, mainField);

        Check(mainRank == 1,
              "the fastest MAIN run is 1st, though it is not the fastest run");
        Check(mainField == 3, "and its field is the three main runs");
        Check(bonusRank == 1, "the bonus is also 1st -- of its own leg");
        Check(bonusField == 1, "which has one run in it");
    }

    // -----------------------------------------------------------------------
    printf("\nstages are separate legs from each other\n");
    {
        WrRun runs[5] = {
            Make(10.0, 1, 1, "s1 best"),
            Make(11.0, 1, 1, "s1 second"),
            Make(12.0, 1, 2, "s2 best"),
            Make(13.0, 1, 2, "s2 second"),
            Make(14.0, 1, 2, "s2 third"),
        };
        WrPathTestLoad(runs, 5);

        int f1 = 0, f2 = 0;
        Check(WrRunRankInTrack(WrRunAt(0), &f1) == 1 && f1 == 2, "stage 1 has two");
        Check(WrRunRankInTrack(WrRunAt(2), &f2) == 1 && f2 == 3, "stage 2 has three");
        Check(WrRunRankInTrack(WrRunAt(4), NULL) == 3, "and its slowest is 3rd");
    }

    // -----------------------------------------------------------------------
    printf("\nequal times share a place rather than one losing it to array order\n");
    {
        WrRun runs[4] = {
            Make(41.82, 0, 1, "a"),
            Make(41.82, 0, 1, "b"),
            Make(41.82, 0, 1, "c"),
            Make(50.00, 0, 1, "d"),
        };
        WrPathTestLoad(runs, 4);

        // Three genuine ties -- surf_demise really does have three runs at
        // 41.820 -- so all three are 1st and the next is 4th, as a scoreboard
        // does it. Nothing here should depend on which one the sort put first.
        Check(WrRunRankInTrack(WrRunAt(0), NULL) == 1, "the first tie is 1st");
        Check(WrRunRankInTrack(WrRunAt(1), NULL) == 1, "so is the second");
        Check(WrRunRankInTrack(WrRunAt(2), NULL) == 1, "and the third");
        Check(WrRunRankInTrack(WrRunAt(3), NULL) == 4,
              "and the next run is 4th, not 2nd");
    }

    // -----------------------------------------------------------------------
    printf("\na run with no path is not placed, and does not pad the field\n");
    {
        // Rank is computed once when the store settles and read back as a field,
        // so "unranked" has to be representable. Rank 0 is it -- the renderer
        // falls back to the palette colour rather than colouring a run as though
        // it came last, which is what a rank of `total` would have meant.
        WrRun runs[3] = {
            Make(40.00, 0, 1, "real"),
            Make(41.00, 0, 1, "also real"),
            Make(39.00, 0, 1, "empty"),
        };
        runs[2].pointCount = 0;         // loaded, rejected, no path
        WrPathTestLoad(runs, 3);

        int outOf = -1;
        Check(WrRunRankInTrack(WrRunAt(2), &outOf) == 0, "the empty run is rank 0");
        Check(outOf == 0, "and reports no field");
        Check(WrRunRankInTrack(WrRunAt(0), &outOf) == 1 && outOf == 2,
              "and the two real runs are a field of two, not three");
        Check(WrRunRankInTrack(WrRunAt(1), NULL) == 2,
              "so the 41.00 is 2nd, not 3rd behind a run with no path");
    }

    // -----------------------------------------------------------------------
    printf("\nnothing loaded, and nothing asked\n");
    {
        WrPathTestLoad(NULL, 0);
        int outOf = -1;
        Check(WrRunRankInTrack(NULL, &outOf) == 0, "a null run is rank 0");
        Check(outOf == 0, "with an empty field");
    }

    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
