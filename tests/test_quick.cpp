// test_quick.cpp  --  the quick menu's chain, and the promise that it stops.
//
// Ticking a run on the quick page means "download it, extract it, draw it",
// across however many frames that takes, through a job slot shared with every
// button in the other panel. That is a state machine, and the thing a state
// machine gets wrong is not the happy path.
//
// WHAT THIS IS REALLY ABOUT
//
// Termination. A run that cannot be got has to STOP being tried, and it has to
// say why, because a chain that quietly retries for ever is indistinguishable
// on screen from one that is still working -- the row says "working..." either
// way, and it says it until the game is closed. So most of what follows is
// about the ways a run fails to arrive, and every one of them ends in a give-up
// with a reason attached.
//
// The other half is the two suppressions. WQ_ENABLE must NOT wait for the job
// slot, because a run already on disk needs no job and making it draw is one
// bool -- waiting there would mean ticking a run you already hold does nothing
// visible until somebody else's download finishes. And nothing except enabling
// may be decided while the run store is loading, because the reload that
// follows a successful extraction is exactly the window in which a run that is
// about to appear is genuinely not there yet. Deciding then would give up on
// every run the moment it succeeded.
//
// THIS LINKS NOTHING
//
// WrQuickDecide is a static inline in wr_quick.h, with the rest of this
// project's pure logic -- wr_pacing.h, wr_matrixlife.h, wr_smooth.h. So this
// harness needs no ImGui, no Windows, no job slot and no run store. A test that
// had to drag the panel in to reach the state machine would be a test of the
// panel.
//
// Build:  tests\build.bat
// Run:    tests\test_quick.exe

#include "wr_quick.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

static const char *Name(WrQuickAction a)
{
    switch (a)
    {
    case WQ_NOTHING:          return "nothing";
    case WQ_ENABLE:           return "enable";
    case WQ_FETCH:            return "fetch";
    case WQ_EXTRACT:          return "extract";
    case WQ_GIVE_UP_NO_DEMO:  return "give up (no demo)";
    case WQ_GIVE_UP_NO_PATH:  return "give up (no path)";
    }
    return "?";
}

static WrQuickPick Pick(const char *hash, int rank)
{
    WrQuickPick p;
    memset(&p, 0, sizeof(p));
    strcpy_s(p.hash, sizeof(p.hash), hash);
    p.rank = rank;
    p.trackType = 0;
    p.trackNum = 1;
    return p;
}

// Act on a decision the way the panel does, so a whole chain can be run to its
// end and the number of steps counted. Deliberately a copy of the switch in
// WrQuickTick with the I/O taken out -- if the two ever disagree, this file is
// the one describing what was intended.
//
// `world` says what the outside is like: whether a fetch produces a file and
// whether an extract produces a run.
struct World { bool fetchWorks; bool extractWorks; };

static WrQuickAction Step(WrQuickPick *picks, int n, const World &w, bool busy,
                          bool loading)
{
    int idx = -1;
    const WrQuickAction a = WrQuickDecide(picks, n, busy, loading, &idx);
    switch (a)
    {
    case WQ_ENABLE:
        picks[idx].done = true;
        break;
    case WQ_FETCH:
        // The panel batches every pick wanting a fetch into one request, so all
        // of them are marked, not just the one named.
        for (int i = 0; i < n; i++)
        {
            if (picks[i].done || picks[i].failed || picks[i].inStore ||
                picks[i].fetched)
                continue;
            picks[i].fetched = true;
            if (w.fetchWorks)
                picks[i].haveDemo = true;
        }
        break;
    case WQ_EXTRACT:
        picks[idx].extracted = true;
        if (w.extractWorks)
            picks[idx].inStore = true;
        break;
    case WQ_GIVE_UP_NO_DEMO:
    case WQ_GIVE_UP_NO_PATH:
        picks[idx].failed = true;
        strcpy_s(picks[idx].why, sizeof(picks[idx].why), WrQuickGiveUpReason(a));
        break;
    default:
        break;
    }
    return a;
}

// Run until nothing is left to do, with a hard stop well above any legitimate
// number of steps. Returns how many steps it took, or -1 if it never settled --
// which is the failure this whole file exists to catch.
static int RunToEnd(WrQuickPick *picks, int n, const World &w, int cap = 200)
{
    for (int steps = 0; steps < cap; steps++)
    {
        if (Step(picks, n, w, false, false) == WQ_NOTHING)
            return steps;
    }
    return -1;
}

int main(void)
{
    printf("\n=== wrlines quick-menu chain ===\n\n");

    printf("a run that is already extracted\n");
    {
        WrQuickPick p[1] = { Pick("aaa", 1) };
        p[0].inStore = true;

        int idx = -1;
        WrQuickAction a = WrQuickDecide(p, 1, false, false, &idx);
        Check(a == WQ_ENABLE, "is drawn, with nothing downloaded");
        Check(idx == 0, "and the panel is told which one");

        // The suppression that matters most in practice: the other panel is
        // mid-extraction and this run is already on disk.
        a = WrQuickDecide(p, 1, true, true, &idx);
        Check(a == WQ_ENABLE, "even while the job slot is busy and the store loads");

        p[0].done = true;
        Check(WrQuickDecide(p, 1, false, false, &idx) == WQ_NOTHING,
              "and once drawn it asks for nothing further");
    }
    printf("\n");

    printf("a run that has to be got\n");
    {
        WrQuickPick p[1] = { Pick("bbb", 7) };
        const World fine = { true, true };

        int idx = -1;
        Check(WrQuickDecide(p, 1, false, false, &idx) == WQ_FETCH,
              "is downloaded first, because nothing else can be done yet");

        Step(p, 1, fine, false, false);
        Check(p[0].fetched, "the attempt is recorded before the answer arrives");
        Check(WrQuickDecide(p, 1, false, false, &idx) == WQ_EXTRACT,
              "then the demo we now hold is read");

        Step(p, 1, fine, false, false);
        Check(p[0].inStore, "which produces a run");
        Check(WrQuickDecide(p, 1, false, false, &idx) == WQ_ENABLE,
              "and only then is it drawn");

        Step(p, 1, fine, false, false);
        Check(p[0].done && !p[0].failed, "three steps, one line, no complaints");
    }
    printf("\n");

    printf("nothing is decided while the store is loading\n");
    {
        // The window after a successful extraction: the .wrpath is written, the
        // store is re-reading every file for the map, and the run is genuinely
        // not in it yet. Deciding here would call it missing -- so this is the
        // case that would make every SUCCESS look like a failure.
        WrQuickPick p[1] = { Pick("ccc", 3) };
        p[0].fetched = true;
        p[0].haveDemo = true;
        p[0].extracted = true;

        int idx = -1;
        Check(WrQuickDecide(p, 1, false, true, &idx) == WQ_NOTHING,
              "a run mid-reload is not yet a run that failed");
        Check(WrQuickDecide(p, 1, true, false, &idx) == WQ_NOTHING,
              "and neither is one whose job has not finished");

        p[0].inStore = true;
        Check(WrQuickDecide(p, 1, false, false, &idx) == WQ_ENABLE,
              "once the reload lands it is simply drawn");
    }
    printf("\n");

    printf("the job slot belongs to the other panel\n");
    {
        WrQuickPick p[1] = { Pick("ddd", 2) };
        int idx = -1;
        Check(WrQuickDecide(p, 1, true, false, &idx) == WQ_NOTHING,
              "so nothing is submitted on top of it");
        Check(WrQuickDecide(p, 1, false, false, &idx) == WQ_FETCH,
              "and the moment it is free the chain carries on");
    }
    printf("\n");

    printf("a run the server will not give us\n");
    {
        WrQuickPick p[1] = { Pick("eee", 9000) };
        const World noDownload = { false, true };

        const int steps = RunToEnd(p, 1, noDownload);
        Check(steps >= 0, "stops rather than asking for ever");
        Check(steps == 2, "after exactly one download attempt and one verdict");
        Check(p[0].failed, "and the row is marked as given up on");
        Check(strcmp(p[0].why, "the download did not arrive") == 0,
              "with the reason a person could act on");
        Check(!p[0].done, "and never claims to be drawn");
    }
    printf("\n");

    printf("a demo the extractor cannot read\n");
    {
        // The other failure, and it looks nothing like the first from the
        // outside: the file downloaded perfectly and is simply not readable.
        WrQuickPick p[1] = { Pick("fff", 40) };
        const World badDemo = { true, false };

        const int steps = RunToEnd(p, 1, badDemo);
        Check(steps >= 0, "also stops");
        Check(steps == 3, "one download, one read, one verdict");
        Check(p[0].failed && strcmp(p[0].why, "that demo could not be read") == 0,
              "and says which of the two things went wrong");
    }
    printf("\n");

    printf("a run that is missing and never even downloads\n");
    {
        // Fetch attempted, no file, and the panel has since noticed the demo IS
        // there after all -- a copy landing late, or the game downloading it
        // itself. The chain must take the file rather than the earlier verdict.
        WrQuickPick p[1] = { Pick("ggg", 5) };
        p[0].fetched = true;
        p[0].haveDemo = true;

        int idx = -1;
        Check(WrQuickDecide(p, 1, false, false, &idx) == WQ_EXTRACT,
              "is read, because the facts are refreshed and not remembered");
    }
    printf("\n");

    printf("five ticked at once\n");
    {
        WrQuickPick p[5] = {
            Pick("h0", 1), Pick("h1", 2), Pick("h2", 3), Pick("h3", 4), Pick("h4", 5)
        };
        const World fine = { true, true };

        int idx = -1;
        Check(WrQuickDecide(p, 5, false, false, &idx) == WQ_FETCH,
              "download comes first for all of them");

        Step(p, 5, fine, false, false);
        int fetched = 0;
        for (int i = 0; i < 5; i++)
            if (p[i].fetched)
                fetched++;
        Check(fetched == 5, "and it is ONE request, not five");

        const int steps = RunToEnd(p, 5, fine);
        Check(steps >= 0, "the rest settles");
        int drawn = 0;
        for (int i = 0; i < 5; i++)
            if (p[i].done && !p[i].failed)
                drawn++;
        Check(drawn == 5, "with all five lines up");
        // One fetch, then per run an extract and an enable.
        Check(steps == 10, "in ten further steps: an extract and an enable each");
    }
    printf("\n");

    printf("a mixed handful, where two of them cannot be got\n");
    {
        // The realistic case, and the one where a chain that gives up wrongly
        // does the most damage: three good runs must still arrive.
        WrQuickPick p[5] = {
            Pick("m0", 1), Pick("m1", 2), Pick("m2", 3), Pick("m3", 4), Pick("m4", 5)
        };
        p[0].inStore = true;        // already extracted
        p[2].fetched = true;        // asked for, nothing came back
        p[4].fetched = true;        // came back, but unreadable
        p[4].haveDemo = true;
        p[4].extracted = true;

        const World fine = { true, true };
        const int steps = RunToEnd(p, 5, fine);
        Check(steps >= 0, "settles");

        int drawn = 0, gaveUp = 0;
        for (int i = 0; i < 5; i++)
        {
            if (p[i].failed) gaveUp++;
            else if (p[i].done) drawn++;
        }
        Check(drawn == 3, "the three that could be got are drawn");
        Check(gaveUp == 2, "the two that could not are given up on");
        Check(strcmp(p[2].why, "the download did not arrive") == 0 &&
              strcmp(p[4].why, "that demo could not be read") == 0,
              "each with its own reason, not one shared one");
        Check(!p[2].done && !p[4].done, "and neither is counted as drawn");
    }
    printf("\n");

    printf("nothing ticked\n");
    {
        int idx = 7;
        Check(WrQuickDecide(NULL, 0, false, false, &idx) == WQ_NOTHING,
              "asks for nothing");
        Check(idx == -1, "and names nothing");

        WrQuickPick p[1] = { Pick("zzz", 1) };
        Check(WrQuickDecide(p, 0, false, false, NULL) == WQ_NOTHING,
              "a zero count is respected over the array behind it");
    }
    printf("\n");

    printf("every decision is one of the six\n");
    {
        // A crude sweep over the whole state space of one pick, which is 64
        // combinations of six booleans. Not looking for a particular answer --
        // looking for the absence of a state that falls through the switch, and
        // for an index that points at a pick whenever one is named.
        bool sawOutOfRange = false, badIndex = false;
        for (int bits = 0; bits < 64; bits++)
        {
            WrQuickPick p[1] = { Pick("s", 1) };
            p[0].inStore   = (bits & 1) != 0;
            p[0].haveDemo  = (bits & 2) != 0;
            p[0].fetched   = (bits & 4) != 0;
            p[0].extracted = (bits & 8) != 0;
            p[0].done      = (bits & 16) != 0;
            p[0].failed    = (bits & 32) != 0;

            for (int b = 0; b < 4; b++)
            {
                int idx = -2;
                WrQuickAction a = WrQuickDecide(p, 1, (b & 1) != 0, (b & 2) != 0,
                                                &idx);
                if (a < WQ_NOTHING || a > WQ_GIVE_UP_NO_PATH)
                    sawOutOfRange = true;
                if (a == WQ_NOTHING) { if (idx != -1) badIndex = true; }
                else if (idx != 0)   { badIndex = true; }
            }
        }
        Check(!sawOutOfRange, "over all 64 states and 4 gatings, in both");
        Check(!badIndex, "and an index is written exactly when there is one");
    }
    printf("\n");

    printf("a done or failed pick is inert\n");
    {
        // Whatever else is true of it. This is what stops a row being reworked
        // after it has been given up on -- the one path back to an endless loop.
        bool moved = false;
        for (int bits = 0; bits < 16; bits++)
        {
            WrQuickPick p[1] = { Pick("t", 1) };
            p[0].inStore   = (bits & 1) != 0;
            p[0].haveDemo  = (bits & 2) != 0;
            p[0].fetched   = (bits & 4) != 0;
            p[0].extracted = (bits & 8) != 0;

            p[0].done = true;
            if (WrQuickDecide(p, 1, false, false, NULL) != WQ_NOTHING)
                moved = true;

            p[0].done = false;
            p[0].failed = true;
            if (WrQuickDecide(p, 1, false, false, NULL) != WQ_NOTHING)
                moved = true;
        }
        Check(!moved, "so a settled row never starts working again");
    }
    printf("\n");

    // -----------------------------------------------------------------------
    printf("which leaderboard a map name implies\n");
    {
        // The bug this table exists for: every map on the machine was asked
        // about in gamemode 1, which is surf, so bhop_hades came back empty and
        // the page offered to ask again. See the essay in wr_quick.h.
        Check(WrQuickGamemodeGuess("bhop_hades") == 2,
              "bhop_hades is bhop, which is the whole point of this");
        Check(WrQuickGamemodeGuess("surf_helloworld") == 1, "surf_ is surf");
        Check(WrQuickGamemodeGuess("bhop_telehop_theory") == 2,
              "and a bhop map with underscores later on is still bhop");
        Check(WrQuickGamemodeGuess("rj_bhop_mix") == 7,
              "it matches the START and not anywhere, so a rocket-jump map "
              "named after bhop is not bhop");
        Check(WrQuickGamemodeGuess("sj_kaizo") == 8, "sj_ is sticky jump");
        Check(WrQuickGamemodeGuess("ahop_lostworld") == 9, "ahop_ is ahop");
        Check(WrQuickGamemodeGuess("conc_hops") == 10, "conc_ is conc");

        Check(WrQuickGamemodeGuess("defrag_speed") == 11,
              "defrag_ is defrag CPM");
        Check(WrQuickGamemodeGuess("df_speed") == 11,
              "and so is the df_ spelling of it");

        // The refusals matter more than the answers. A wrong mode is an empty
        // board, and an empty board on a map somebody has definitely run is the
        // failure this whole change is about.
        Check(WrQuickGamemodeGuess("kz_cellblock") == WR_QUICK_MODE_UNKNOWN,
              "climb has three modes and a kz_ prefix cannot say which");
        Check(WrQuickGamemodeGuess("climb_frenzy") == WR_QUICK_MODE_UNKNOWN,
              "nor can climb_, for the same reason");
        Check(WrQuickGamemodeGuess("tricksurf_arena") == WR_QUICK_MODE_UNKNOWN,
              "and tricksurf has no mode in the enum at all");
        Check(WrQuickGamemodeGuess("agtricks") == WR_QUICK_MODE_UNKNOWN,
              "a map with no prefix says nothing");
        Check(WrQuickGamemodeGuess("surfing") == WR_QUICK_MODE_UNKNOWN,
              "and a name that merely STARTS like one is not one -- the "
              "underscore is part of the prefix");

        Check(WrQuickGamemodeGuess("") == WR_QUICK_MODE_UNKNOWN,
              "no name, no opinion");
        Check(WrQuickGamemodeGuess(NULL) == WR_QUICK_MODE_UNKNOWN,
              "and no map at all is not a crash");

        Check(WrQuickGamemodeGuess("BHOP_Hades") == 2,
              "the match folds case, because a map name is not a hash");
    }
    printf("\n");

    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
