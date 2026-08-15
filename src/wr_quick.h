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
#include "wr_board.h"           // WR_GAMEMODE_COUNT, which bounds a parsed mode

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

    // The mode to fall back on when nothing about the map says which.
    //
    // A FALLBACK, and no longer the answer. It used to be read directly as "the
    // leaderboard to ask for", which meant every map on the machine was asked
    // about in surf -- see WrQuickGamemodeGuess below for what that cost. The
    // resolution order is there; this is the last step of it.
    int gamemode;
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

    // ...with exactly one exception, and it is a press rather than a retry.
    //
    // "One extract attempt and then a reason" is right when the reason is a
    // property of the file, and wrong for the one reason that is a property of
    // the ATTEMPT: running out of time. Those are most of the failures there
    // are -- 403 of 415 on the machine this was measured on -- so the page
    // offers a second go with no limit on it, once, when the extractor says
    // that is what happened. Set by the button, never by the state machine, so
    // nothing here can loop.
    bool canRetry;              // the last attempt timed out
    bool retried;               // and the user asked for the unlimited one

    bool done;
    bool failed;
    char why[144];              // the extractor's own sentence, which is long
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

// ---------------------------------------------------------------------------
// Which leaderboard, from the name of the map
// ---------------------------------------------------------------------------
//
// THE BUG THIS EXISTS TO NOT HAVE AGAIN. This page asked gamemode 1 for every
// map ever loaded, because that is what g_quick.gamemode was initialised to and
// there was no control to change it. Gamemode 1 is SURF. So on bhop_hades the
// automatic read fired correctly, half a second after the map change, asked the
// surf leaderboard of a bhop map, was told -- correctly -- that it is empty, and
// then drew a button offering to ask the same wrong question again. The log
// shows the job succeeding with exit code 0 and no cache file appearing, which
// is exactly what a successful request for an empty board looks like.
//
// It made the front door useless on roughly half the game while looking, from
// the inside, like everything working.
//
// WHY THE MAP INDEX CANNOT ANSWER THIS. maps.txt carries a `modes` column and it
// is no help at all: bhop_hades lists 1,2,3,5,6,7,8,9,10,11,12,13. Momentum
// gives nearly every map a leaderboard in nearly every mode and almost all of
// them are empty -- wr_board.h says so about the surf catalogue too. A list of
// boards that MIGHT exist cannot pick the one that does.
//
// So the caller resolves in this order, most certain first:
//
//   1. a board already cached for this map -- the filename carries the mode, and
//      it is the mode you already fetched, not a guess at all;
//   2. a mode chosen for this map before, remembered in quickpicks.txt;
//   3. this function;
//   4. the g_quick.gamemode setting.
//
// WHY A PREFIX IS ENOUGH, AND WHERE IT STOPS. Momentum's map names are prefixed
// by discipline and the convention is near-universal, so the common cases are
// exact. The climb family is deliberately absent: it has THREE modes (4 Momentum,
// 5 KZT, 6 16-unit) and a kz_ or climb_ prefix cannot tell you which, so it
// returns 0 -- "no opinion" -- rather than being confidently wrong two times in
// three. Same for tricksurf, which has no mode in the enum at all.
#define WR_QUICK_MODE_UNKNOWN 0

static inline bool WrQuickHasPrefix(const char *s, const char *p)
{
    for (; *p; s++, p++)
    {
        const char a = (*s >= 'A' && *s <= 'Z') ? (char)(*s - 'A' + 'a') : *s;
        if (a != *p)
            return false;
    }
    return true;
}

// Pull the mode and the leg back out of a board cache filename.
//
// The filename is boards\<map>_g<mode>_t<type><num>.tsv, and it is the only
// record of which legs and which gamemodes have ever been fetched for a map --
// one directory listing answers both with no file opened.
//
// IT IS HERE, PURE AND TESTED, BECAUSE THE INLINE VERSION WAS WRONG FOR TWO
// RELEASES AND NOTHING NOTICED. It looked for the last 't' with strrchr and
// then rejected any that fell after the dot -- and the last 't' in
// "surf_helloworld_g1_t11.tsv" is the one in ".tsv", which always falls after
// the dot. Every file, every time, skipped, in a function whose entire job was
// to list legs. What it cost was invisible rather than loud: a leg was a chip
// only while runs for it happened to be LOADED, so the stage list collapsed to
// the main track during every store reload -- which is one reload per finished
// download.
//
// Reading past the map name rather than searching for a separator is what makes
// it safe. A map called "surf_t_thing" has a "_t" of its own and a map called
// "surf_g_thing" has a "_g"; neither is ever looked at, because the length of
// the name we are asking about says where the fields begin.
//
// `name` is the bare filename, not a path. Any out-param may be NULL.
static inline bool WrQuickParseBoardName(const char *name, const char *map,
                                         int *mode, int *type, int *num)
{
    if (mode) *mode = 0;
    if (type) *type = 0;
    if (num)  *num = 0;
    if (!name || !map || !*map)
        return false;

    size_t skip = 0;
    while (map[skip])
        skip++;
    skip += 2;                          // past "_g"

    size_t n = 0;
    while (name[n])
        n++;
    if (n <= skip)
        return false;

    const char *p = name + skip;        // "<mode>_t<type><num>.tsv"
    if (*p < '0' || *p > '9')
        return false;                   // a longer map's file caught by the glob

    int m = 0;
    for (const char *d = p; *d >= '0' && *d <= '9'; d++)
        m = m * 10 + (*d - '0');
    if (m < 1 || m > WR_GAMEMODE_COUNT)
        return false;

    const char *t = NULL;
    for (const char *c = p; c[0] && c[1]; c++)
        if (c[0] == '_' && c[1] == 't')
        {
            t = c;
            break;
        }
    if (!t || t[2] < '0' || t[2] > '9')
        return false;

    const int ty = t[2] - '0';
    if (ty < 0 || ty > 2)
        return false;

    int nu = 0;
    bool anyDigit = false;
    for (const char *d = t + 3; *d >= '0' && *d <= '9'; d++)
    {
        nu = nu * 10 + (*d - '0');
        anyDigit = true;
    }
    if (!anyDigit || nu < 0 || nu > 255)
        return false;

    if (mode) *mode = m;
    if (type) *type = ty;
    if (num)  *num = nu;
    return true;
}

// The gamemode a map's NAME implies, or WR_QUICK_MODE_UNKNOWN.
static inline int WrQuickGamemodeGuess(const char *map)
{
    if (!map || !*map)
        return WR_QUICK_MODE_UNKNOWN;

    // Order does not matter here and that is a property worth keeping: no entry
    // is a prefix of another, so exactly one can ever match and the table can be
    // read as a set. Adding one that overlaps -- "df" beside "df_", say -- would
    // quietly make this list order-dependent, which is the kind of thing that is
    // discovered by a wrong answer rather than by reading.
    struct { const char *prefix; int mode; } kByName[] = {
        { "surf_",    1 },
        { "bhop_",    2 },
        { "rj_",      7 },
        { "sj_",      8 },
        { "ahop_",    9 },
        { "conc_",   10 },
        { "defrag_", 11 },
        { "df_",     11 },
    };
    for (int i = 0; i < (int)(sizeof(kByName) / sizeof(kByName[0])); i++)
        if (WrQuickHasPrefix(map, kByName[i].prefix))
            return kByName[i].mode;

    return WR_QUICK_MODE_UNKNOWN;
}

#endif // WR_QUICK_H
