// wr_quick.h  --  the front door. One page, on Delete.
//
// WHY THERE IS A SECOND PANEL AT ALL
//
// The one on Insert is complete on purpose: nine tabs, every setting anybody
// could want, and it is not going anywhere. But completeness has a cost that
// only shows up in somebody else's hands -- the path from "a run on the
// leaderboard" to "a line on the screen" is five presses across two tabs, and
// two of those tabs do not mention each other. A fetch finishes, the store
// reloads, and the demos it just downloaded are still not extracted, because
// downloading and extracting are different buttons on different pages and
// nothing says so.
//
// So this is the same capability with the decisions taken out. One page. The
// legs of the map you are standing in, the fastest twenty runs of whichever leg
// you are looking at, and a tick box per run that means "put that line on the
// screen" -- downloading it, extracting it and enabling it without being asked
// again. What it cannot do is anything the other panel is for. There are no
// settings here beyond the colour mode, and that is the point of it.
//
// WHAT IT IS NOT
//
// Not a tenth tab. A tab would have inherited the parent window's size, its
// position, and its "you are already in the deep end" framing, and it would
// still have been five presses away from a beginner who does not know the tool
// has tabs. It is its own window with its own remembered geometry, on its own
// key.
//
// Not a second input owner either. See WrPanel in wr_hook.h: there is one window
// procedure and one virtual cursor between the two panels, and the game gets its
// input back when the LAST one closes.
//
// THE THREE THINGS THAT MAKE THIS HARDER THAN IT LOOKS
//
//   ONE JOB SLOT. Fetching, extracting and board-reading share a single latch in
//   wr_extract.h, and it is also the latch behind every button in the full
//   panel. So a tick cannot call three functions; it has to become a want, and
//   the chain has to advance one step per frame when the slot is free. It waits
//   rather than fighting, and it says it is waiting.
//
//   A STORE RELOAD WIPES THE ENABLED SET. FinishLoad in wr_path.cpp turns
//   everything off and WrUpdateNearest then turns exactly one run on. Since
//   every extraction ends in a reload, a naive "tick means set enabled" would
//   lose every previous tick each time the next one finished. So the tick list
//   is the truth and the store is made to agree with it after every reload --
//   which is also why WrPathCancelAutoEnable exists.
//
//   TICKS ARE A RECORD OF WHAT YOU WATCHED. They cannot go in settings.cfg,
//   which promises in writing to contain no names, no run data and no record of
//   what was watched. They go to wrlines_data\quickpicks.txt with everything
//   else that names other players.

#ifndef WR_QUICK_H
#define WR_QUICK_H

#include "wr_common.h"

// How many places the list shows. Twenty is what was asked for and it is also
// about where a leaderboard stops being a list of runs and starts being a
// database -- the top twenty of a nine-thousand-run board is the part anybody
// would actually watch.
#define WR_QUICK_TOP_DEFAULT 20
#define WR_QUICK_TOP_MAX 100

// How many runs may be ticked at once, across all legs of a map.
//
// A cap and not a warning, because every tick is a demo download and an
// extraction, and the failure mode of not having one is somebody holding the
// tick-five button until the game spends an hour of CPU on work they will never
// look at.
#define WR_QUICK_MAX_PICKS 64

struct WrQuickSettings
{
    // Consent to read the public leaderboard without a press, and the one real
    // change of posture this panel makes.
    //
    // Everything else in this tool gates the network behind g_fetchEnabled,
    // which defaults to false and RESETS EVERY LAUNCH -- nothing reaches the
    // network without a press in that session. That is right for a panel you
    // are already looking at and wrong for one whose whole promise is that it
    // fills itself in. So this is the same fence moved: still off by default,
    // still one explicit yes, but persisted, so it is one press ever rather
    // than one press per session.
    //
    // It only ever covers reading a BOARD, and only for the leg you are looking
    // at, and only when nothing is cached for it. Demo bodies are never
    // downloaded without a tick.
    bool network;

    int top;                    // places listed per leg
    int gamemode;               // which leaderboard; 1 is the usual one
};

extern WrQuickSettings g_quick;

// Called from dllmain with the other *Defaults(), BEFORE settings.cfg is read
// over the top. The registration itself is in wr_settings.cpp beside the rest.
void WrQuickDefaults(void);

// Once per frame from inside the ImGui frame, when the panel is up.
void WrQuickDraw(void);

// Once per frame from WrIdleTick, WHETHER OR NOT THE PANEL IS UP.
//
// That is the difference between "it downloads and extracts in the background"
// and "it does that while you watch it". Tick a run, close the panel, keep
// playing: the chain still advances and the line appears when it is ready.
void WrQuickTick(void);

void WrQuickOnMapChanged(const char *map);

// ---------------------------------------------------------------------------
// The chain, as a decision
// ---------------------------------------------------------------------------
//
// Separated from everything that acts on it so it can be tested. What makes this
// worth doing is not the arithmetic -- there is barely any -- but that the state
// machine has to TERMINATE, and terminate having said something, for every run
// that cannot be got. A chain that silently retries a demo the server does not
// have is indistinguishable from one that is still working, for ever.
//
// So: one fetch attempt and one extract attempt per run, and then a reason. The
// function below touches no files, no globals and no job slot, and it never
// mutates a pick.
//
// static inline IN A HEADER, which is how every other pure piece of this project
// is written -- wr_pacing.h, wr_matrixlife.h, wr_smooth.h, wr_budget.h. The
// property that buys is that tests\test_quick.exe links NOTHING: no ImGui, no
// Windows, no job slot, no run store. A harness that had to drag the panel in to
// reach the state machine would be testing the panel.

enum WrQuickAction
{
    WQ_NOTHING = 0,
    WQ_ENABLE,              // it is in the store -- draw it
    WQ_FETCH,               // download it (and every other pick wanting one)
    WQ_EXTRACT,             // read the demo we now hold
    WQ_GIVE_UP_NO_DEMO,     // the fetch ran and no file arrived
    WQ_GIVE_UP_NO_PATH      // the extract ran and no run appeared
};

struct WrQuickPick
{
    char hash[48];              // the replay hash, which IS the .mtv basename
    int rank;
    unsigned char trackType, trackNum;

    // Facts, refreshed by the caller before each decision.
    bool inStore;               // a loaded run carries this hash
    bool haveDemo;              // the .mtv is on disk somewhere we look

    // What has been tried. One shot each; see above.
    bool fetched;
    bool extracted;

    bool done;
    bool failed;
    char why[80];
};

// What to do next, and which pick it is about.
//
// `busy` is whether the shared job slot is taken; `loading` whether the run
// store is still feeding itself files. Both suppress everything except WQ_ENABLE
// -- enabling costs nothing and must not wait behind a download -- and `loading`
// in particular is what stops a run being declared missing during the very
// reload that is about to produce it.
static inline WrQuickAction WrQuickDecide(const WrQuickPick *picks, int n,
                                          bool busy, bool loading, int *outIndex)
{
    if (outIndex)
        *outIndex = -1;
    if (!picks || n <= 0)
        return WQ_NOTHING;

    // Enabling first, and NOT behind the busy check. A run already in the store
    // needs no job and no disk: making it draw is one bool. Putting it behind
    // `busy` would mean ticking a run you already hold did nothing visible until
    // somebody else's download finished, which is the exact complaint this panel
    // exists to answer.
    for (int i = 0; i < n; i++)
    {
        if (picks[i].done || picks[i].failed)
            continue;
        if (picks[i].inStore)
        {
            if (outIndex) *outIndex = i;
            return WQ_ENABLE;
        }
    }

    // Nothing else can be decided while a job holds the slot or the store is
    // still reading files. The second is the subtle one: an extraction that has
    // just succeeded is followed by a reload, and during that reload the run it
    // produced is genuinely not in the store yet. Deciding here would call it
    // missing and give up on a run that was about to appear.
    if (busy || loading)
        return WQ_NOTHING;

    // One fetch attempt, for everything that has not had one. The caller batches
    // every other pick in the same state into the same request, so this
    // returning the first of them is the whole signal it needs.
    for (int i = 0; i < n; i++)
    {
        if (picks[i].done || picks[i].failed || picks[i].inStore)
            continue;
        if (!picks[i].fetched)
        {
            if (outIndex) *outIndex = i;
            return WQ_FETCH;
        }
    }

    for (int i = 0; i < n; i++)
    {
        if (picks[i].done || picks[i].failed || picks[i].inStore)
            continue;

        if (!picks[i].haveDemo)
        {
            // The fetch ran and no file arrived. Asking again would put the same
            // question to the same server and get the same answer.
            if (outIndex) *outIndex = i;
            return WQ_GIVE_UP_NO_DEMO;
        }
        if (!picks[i].extracted)
        {
            if (outIndex) *outIndex = i;
            return WQ_EXTRACT;
        }
        // Fetched, held, extracted, and still not in the store. That is a demo
        // the extractor refused, which is a property of the file and not of the
        // attempt.
        if (outIndex) *outIndex = i;
        return WQ_GIVE_UP_NO_PATH;
    }

    return WQ_NOTHING;
}

// The sentence shown on a row that gave up. Here rather than in the panel so the
// reason a user reads is the reason a test asserts.
static inline const char *WrQuickGiveUpReason(WrQuickAction a)
{
    if (a == WQ_GIVE_UP_NO_DEMO)
        return "the download did not arrive";
    if (a == WQ_GIVE_UP_NO_PATH)
        return "that demo could not be read";
    return "";
}

#endif // WR_QUICK_H
