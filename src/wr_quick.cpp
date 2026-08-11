// wr_quick.cpp  --  see wr_quick.h.

#include "wr_quick.h"
#include "wr_board.h"
#include "wr_extract.h"
#include "wr_hook.h"
#include "wr_log.h"
#include "wr_maps.h"
#include "wr_path.h"
#include "wr_render.h"
#include "wr_settings.h"
#include "wr_steam.h"

#include "imgui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

WrQuickSettings g_quick;

void WrQuickDefaults(void)
{
    g_quick.network = false;
    g_quick.top = WR_QUICK_TOP_DEFAULT;
    g_quick.gamemode = 1;
}

// The registration lives in wr_settings.cpp beside RegisterRender and
// RegisterLimit, which is where every settings-struct module's does.

// The decision -- WrQuickDecide -- is a static inline in wr_quick.h, with the
// rest of this project's pure logic. See the essay there: it is what lets
// tests\test_quick.exe drive the chain without linking a line of this file.

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

#define MAX_LEGS 64
#define MAX_LEGS_ASKED 64

// How many more places "show more" adds, and asks for.
#define SHOW_MORE_STEP 20

struct Leg { unsigned char type, num; };

static char g_map[72] = "";
static int g_mapId = 0;

static Leg g_legs[MAX_LEGS];
static int g_legCount = 0;
static int g_leg = 0;               // index into g_legs

// The board rows of the leg being looked at. Re-read when the leg, the map or
// the cache file changes -- never per frame. Reading the top twenty of a cached
// board is a twenty-line fgets loop because the file is rank-sorted and
// WrBoardReadCache stops at maxRows, so this is cheap; doing it 300 times a
// second would still be silly.
static WrBoardRow g_rows[WR_QUICK_TOP_MAX];
static int g_rowCount = 0;
static bool g_rowsStale = true;
static int g_boardTotal = 0;
static long long g_boardFetched = 0;

// What each row's last column says, worked out when something changes rather
// than while drawing.
//
// Both halves of it are searches: "is a run with this hash loaded" walks the
// store, and "is the demo on disk" is up to three GetFileAttributes. Twenty rows
// against a thousand-run store is twenty thousand string compares, and doing
// that inside the draw call would spend it three hundred times a second to
// answer a question whose answer changes when a job finishes.
enum { ROW_NOTHING = 0, ROW_ON_DISK, ROW_READY };
static unsigned char g_rowState[WR_QUICK_TOP_MAX];

static WrQuickPick g_picks[WR_QUICK_MAX_PICKS];
static int g_pickCount = 0;
static bool g_picksDirty = false;   // needs writing to disk

// A board read that is wanted and has not happened yet, either because the job
// slot was busy or because it has only just been decided.
static bool g_wantBoard = false;
static unsigned char g_wantBoardType = 0, g_wantBoardNum = 1;
static int g_wantBoardFrom = 1;         // 1-based rank to start at
static int g_wantBoardCount = WR_QUICK_TOP_DEFAULT;

// How many places the table is showing. Starts at the setting and grows by the
// same step each time "show more" is pressed, up to the row array.
//
// Per session and per leg rather than persisted: it is where you have scrolled
// to, not a preference, and coming back to a map tomorrow wanting the top twenty
// again is the ordinary case.
static int g_showTop = WR_QUICK_TOP_DEFAULT;

// Which legs this session has already offered to read, so that switching back
// and forth across the chips is not one request each way.
//
// A leg that came back EMPTY is in here too, and that is the point: a stage with
// no runs would otherwise be asked about every single time it is selected, for
// ever, and the answer would be the same every time.
static unsigned short g_asked[MAX_LEGS_ASKED];
static int g_askedCount = 0;

static unsigned int g_lastExtractGen = 0;
static unsigned int g_lastStoreGen = 0;
static unsigned long long g_nextPollAt = 0;

// What the chain is doing, for the line under the table.
static char g_note[160] = "";

static bool SameHash(const char *a, const char *b)
{
    return _stricmp(a, b) == 0;
}

// ---------------------------------------------------------------------------
// Legs
// ---------------------------------------------------------------------------

static void LegAdd(unsigned char type, unsigned char num)
{
    for (int i = 0; i < g_legCount; i++)
        if (g_legs[i].type == type && g_legs[i].num == num)
            return;
    if (g_legCount >= MAX_LEGS)
        return;
    g_legs[g_legCount].type = type;
    g_legs[g_legCount].num = num;
    g_legCount++;
}

static int __cdecl LegOrder(const void *a, const void *b)
{
    const Leg *x = (const Leg *)a, *y = (const Leg *)b;
    if (x->type != y->type)
        return (int)x->type - (int)y->type;
    return (int)x->num - (int)y->num;
}

// Every board cache file we already hold for this map and gamemode.
//
// The filename IS the answer: boards\<map>_g<mode>_t<type><num>.tsv, so one
// directory listing names every leg anybody has ever fetched here, with no file
// opened. Kept because tracks.txt covers the map's SHAPE and this covers what
// has actually been looked at -- a map absent from the catalogue, or a
// catalogue that has never been read, still lists whatever is cached.
static void LegsFromCache(void)
{
    char pat[MAX_PATH];
    char rel[224];
    _snprintf_s(rel, sizeof(rel), _TRUNCATE, "boards\\%s_g%d_t*.tsv", g_map,
                g_quick.gamemode);
    strcpy_s(pat, sizeof(pat), WrDataPath(rel));

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        const char *t = strrchr(fd.cFileName, 't');
        // The LAST 't' before the extension, which is the one the writer put
        // there -- a map named "surf_torus" has several others.
        const char *dot = strrchr(fd.cFileName, '.');
        if (!t || !dot || t > dot || t[1] < '0' || t[1] > '9')
            continue;
        int type = t[1] - '0';
        int num = atoi(t + 2);
        if (type < 0 || type > 2 || num < 0 || num > 255)
            continue;
        LegAdd((unsigned char)type, (unsigned char)num);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void RebuildLegs(void)
{
    const unsigned char wasType = g_legCount ? g_legs[g_leg].type : 0;
    const unsigned char wasNum = g_legCount ? g_legs[g_leg].num : 1;

    g_legCount = 0;
    LegAdd(0, 1);                   // the main track always exists

    // The catalogue's answer, which is the only exact one. See WrMapsTracksFor:
    // nothing else knows a map has nine stages until you have already asked for
    // all nine.
    int stages = 0, bonuses = 0;
    WrMapsTracksFor(g_map, &stages, &bonuses);
    for (int i = 1; i <= stages && i <= 255; i++)
        LegAdd(1, (unsigned char)i);
    for (int i = 1; i <= bonuses && i <= 255; i++)
        LegAdd(2, (unsigned char)i);

    // Anything the loaded runs are on, in case the catalogue is stale or absent.
    for (int i = 0; i < WrRunCount(); i++)
    {
        const WrRun *r = WrRunAt(i);
        if (r && r->pointCount >= 2)
            LegAdd(r->trackType, r->trackNum);
    }

    LegsFromCache();

    qsort(g_legs, (size_t)g_legCount, sizeof(Leg), LegOrder);

    // Stay on the leg that was selected if it survived the rebuild.
    g_leg = 0;
    for (int i = 0; i < g_legCount; i++)
        if (g_legs[i].type == wasType && g_legs[i].num == wasNum)
            g_leg = i;
}

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

static void BoardPathForLeg(char *out, int cap, int leg)
{
    const unsigned char type = (leg >= 0 && leg < g_legCount) ? g_legs[leg].type : 0;
    const unsigned char num = (leg >= 0 && leg < g_legCount) ? g_legs[leg].num : 1;
    WrBoardCachePath(out, cap, g_map, g_quick.gamemode, type, num);
}

static void ReloadRows(void)
{
    g_rowsStale = false;
    g_rowCount = 0;
    g_boardTotal = 0;
    g_boardFetched = 0;

    if (!g_map[0] || g_legCount <= 0)
        return;

    int top = g_showTop;
    if (top < 1) top = 1;
    if (top > WR_QUICK_TOP_MAX) top = WR_QUICK_TOP_MAX;

    char path[MAX_PATH];
    BoardPathForLeg(path, sizeof(path), g_leg);

    int mapId = 0;
    int n = WrBoardParseFile(path, g_rows, top, &g_boardTotal, &g_boardFetched,
                             &mapId);
    if (n < 0)
        return;                     // nothing cached; not an error
    g_rowCount = n;
    if (mapId > 0)
        g_mapId = mapId;
}

static WrRun *RunWithHash(const char *hash);
static bool DemoPath(const char *hash, char *out, int cap);

// ---------------------------------------------------------------------------
// Asking for a board
// ---------------------------------------------------------------------------

static unsigned short LegKey(unsigned char type, unsigned char num)
{
    return (unsigned short)(((unsigned short)type << 8) | num);
}

static bool AlreadyAsked(unsigned char type, unsigned char num)
{
    const unsigned short k = LegKey(type, num);
    for (int i = 0; i < g_askedCount; i++)
        if (g_asked[i] == k)
            return true;
    return false;
}

static void MarkAsked(unsigned char type, unsigned char num)
{
    if (AlreadyAsked(type, num) || g_askedCount >= MAX_LEGS_ASKED)
        return;
    g_asked[g_askedCount++] = LegKey(type, num);
}

// Ask for a window of the leg being looked at, if it is worth asking for.
//
// THE WHOLE POINT OF THIS PAGE IS THAT IT FILLS ITSELF IN, and until now it did
// not: it drew a button saying "Read the top 20" and waited to be pressed, which
// is one press fewer than the panel it was meant to replace and not the same
// thing at all. Loading a map now reads the main track, and pressing a stage
// chip reads that stage.
//
// Four things keep that from becoming traffic. It needs the persisted consent.
// It only asks for a leg NOTHING is cached for -- coming back to a map you have
// looked at before costs nothing at all. It asks once per leg per session, even
// if the answer was an empty board, so flicking across the chips is not one
// request each way. And it is one page of twenty, not a leaderboard: surf_demise
// has nine thousand runs on its main track and this asks for the first twenty of
// them.
static void WantBoard(unsigned char type, unsigned char num, int from, int count)
{
    if (!g_quick.network)
        return;
    g_wantBoard = true;
    g_wantBoardType = type;
    g_wantBoardNum = num;
    g_wantBoardFrom = from < 1 ? 1 : from;
    g_wantBoardCount = count < 1 ? SHOW_MORE_STEP : count;
    g_nextPollAt = 0;
}

// The automatic half: the leg you are looking at, once, when it is empty.
static void MaybeAutoRead(void)
{
    if (!g_quick.network || g_legCount <= 0 || g_wantBoard)
        return;

    const unsigned char type = g_legs[g_leg].type;
    const unsigned char num = g_legs[g_leg].num;
    if (AlreadyAsked(type, num))
        return;

    if (g_rowsStale)
        ReloadRows();
    if (g_rowCount > 0)
    {
        // Something is cached. Nothing to ask, and nothing to ask again later.
        MarkAsked(type, num);
        return;
    }

    MarkAsked(type, num);
    WantBoard(type, num, 1, g_quick.top);
}

static void RefreshRowState(void)
{
    for (int i = 0; i < g_rowCount; i++)
    {
        if (RunWithHash(g_rows[i].hash))
        {
            g_rowState[i] = ROW_READY;
            continue;
        }
        char path[MAX_PATH];
        g_rowState[i] = DemoPath(g_rows[i].hash, path, sizeof(path))
                            ? ROW_ON_DISK : ROW_NOTHING;
    }
}

// ---------------------------------------------------------------------------
// Picks, and the file that remembers them
// ---------------------------------------------------------------------------
//
// wrlines_data\quickpicks.txt, sectioned by map:
//
//     [surf_utopia]
//     0 1 3 <replay hash>
//
// One file rather than one per map, because the whole thing is a few kilobytes
// and a directory of two thousand tiny files is worse in every way that matters.
// Rewritten whole on a change, carrying every other map's section through
// untouched -- which is also what makes deleting a map's section as simple as
// unticking its runs.
//
// NOT settings.cfg. That file says of itself, in writing, that it holds no
// names, no run data and no record of what was watched, and it is offered to
// users as safe to paste into a bug report. A list of replay hashes you chose to
// watch is exactly the thing that promise excludes.

static const char *PicksPath(void)
{
    return WrDataPath("quickpicks.txt");
}

static int FindPick(const char *hash)
{
    for (int i = 0; i < g_pickCount; i++)
        if (SameHash(g_picks[i].hash, hash))
            return i;
    return -1;
}

static void SavePicks(void)
{
    g_picksDirty = false;
    if (!g_map[0])
        return;

    char path[MAX_PATH];
    strcpy_s(path, sizeof(path), PicksPath());
    char tmp[MAX_PATH];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s.tmp", path);

    // Read the old file first so every other map's section survives. Streaming
    // it through rather than holding it means the size of this file is bounded
    // by the disk and not by a buffer somebody guessed at.
    FILE *in = NULL;
    fopen_s(&in, path, "r");

    FILE *out = NULL;
    if (fopen_s(&out, tmp, "w") != 0 || !out)
    {
        if (in) fclose(in);
        WrLogf("[!] quick: could not write %s", tmp);
        return;
    }

    fprintf(out, "# WrLines quick-menu picks. Which runs you asked to see, per "
                 "map.\n");
    fprintf(out, "# tracktype tracknum rank hash\n");

    if (in)
    {
        char line[256];
        bool skipping = false;
        while (fgets(line, sizeof(line), in))
        {
            if (line[0] == '#')
                continue;           // the header is rewritten above
            if (line[0] == '[')
            {
                char name[72] = {0};
                sscanf_s(line, "[%71[^]]", name, (unsigned)sizeof(name));
                skipping = (_stricmp(name, g_map) == 0);
                if (skipping)
                    continue;
                fputs(line, out);
                continue;
            }
            if (!skipping)
                fputs(line, out);
        }
        fclose(in);
    }

    if (g_pickCount > 0)
    {
        fprintf(out, "[%s]\n", g_map);
        for (int i = 0; i < g_pickCount; i++)
            fprintf(out, "%d %d %d %s\n", (int)g_picks[i].trackType,
                    (int)g_picks[i].trackNum, g_picks[i].rank, g_picks[i].hash);
    }

    if (fclose(out) != 0 || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
    {
        WrLogf("[!] quick: could not replace %s", path);
        DeleteFileA(tmp);
    }
}

static void LoadPicks(void)
{
    g_pickCount = 0;
    g_picksDirty = false;
    if (!g_map[0])
        return;

    FILE *f = NULL;
    if (fopen_s(&f, PicksPath(), "r") != 0 || !f)
        return;

    char line[256];
    bool ours = false;
    while (fgets(line, sizeof(line), f))
    {
        if (line[0] == '#')
            continue;
        if (line[0] == '[')
        {
            char name[72] = {0};
            sscanf_s(line, "[%71[^]]", name, (unsigned)sizeof(name));
            ours = (_stricmp(name, g_map) == 0);
            continue;
        }
        if (!ours || g_pickCount >= WR_QUICK_MAX_PICKS)
            continue;

        int type = 0, num = 1, rank = 0;
        char hash[48] = {0};
        if (sscanf_s(line, "%d %d %d %47s", &type, &num, &rank, hash,
                     (unsigned)sizeof(hash)) < 4)
            continue;
        // rank >= 1, and that bound is load-bearing rather than tidy. A fetch is
        // by PLACE, so a pick with no rank can never be submitted -- and the
        // chain would keep deciding "fetch" about a request that comes out
        // empty, for ever. The panel cannot produce one; a hand-edited file can.
        if (!hash[0] || type < 0 || type > 2 || rank < 1)
            continue;

        WrQuickPick *p = &g_picks[g_pickCount++];
        memset(p, 0, sizeof(*p));
        strcpy_s(p->hash, sizeof(p->hash), hash);
        p->rank = rank;
        p->trackType = (unsigned char)type;
        p->trackNum = (unsigned char)(num < 0 ? 0 : num > 255 ? 255 : num);
    }
    fclose(f);
}

static void PickAdd(const WrBoardRow *r, unsigned char type, unsigned char num)
{
    if (!r || FindPick(r->hash) >= 0)
        return;
    if (g_pickCount >= WR_QUICK_MAX_PICKS)
    {
        _snprintf_s(g_note, sizeof(g_note), _TRUNCATE,
                    "%d is as many as this page will take at once", WR_QUICK_MAX_PICKS);
        return;
    }
    WrQuickPick *p = &g_picks[g_pickCount++];
    memset(p, 0, sizeof(*p));
    strcpy_s(p->hash, sizeof(p->hash), r->hash);
    p->rank = r->rank;
    p->trackType = type;
    p->trackNum = num;
    g_picksDirty = true;
    g_nextPollAt = 0;               // decide about it on the very next tick
}

// Untick. Turns the line off as well, because a tick means "draw this" and the
// only honest reading of removing it is "stop".
static void PickRemove(const char *hash)
{
    const int i = FindPick(hash);
    if (i < 0)
        return;

    for (int k = 0; k < WrRunCount(); k++)
    {
        WrRun *r = WrRunAt(k);
        if (r && WrRunIsFrom(r, hash))
            r->enabled = false;
    }

    for (int k = i; k < g_pickCount - 1; k++)
        g_picks[k] = g_picks[k + 1];
    g_pickCount--;
    g_picksDirty = true;
}

// ---------------------------------------------------------------------------
// Facts the decision needs
// ---------------------------------------------------------------------------

// WrRunIsFrom, not a string compare: see its essay in wr_path.h. A .wrpath
// stores thirty-nine characters of a forty-character replay hash, so equality
// here is never true for a downloaded run -- which is exactly the bug that made
// every successful extraction report "that demo could not be read".
static WrRun *RunWithHash(const char *hash)
{
    for (int i = 0; i < WrRunCount(); i++)
    {
        WrRun *r = WrRunAt(i);
        if (r && WrRunIsFrom(r, hash))
            return r;
    }
    return NULL;
}

// The three places a .mtv for this map can be. Same three the board reader and
// the fetcher walk, in the same order, and spelled here rather than shared
// because those two build a whole SET for thousands of rows and this wants one
// answer about one file.
static bool DemoPath(const char *hash, char *out, int cap)
{
    char p[MAX_PATH];

    _snprintf_s(p, sizeof(p), _TRUNCATE, "demos\\%s\\%s.mtv", g_map, hash);
    strcpy_s(out, (size_t)cap, WrDataPath(p));
    if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES)
        return true;

    if (g_mapId > 0)
    {
        _snprintf_s(out, (size_t)cap, _TRUNCATE,
                    "%s\\momentum\\momtv\\online\\%d\\%s.mtv", WrGameDir(),
                    g_mapId, hash);
        if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES)
            return true;
    }

    _snprintf_s(out, (size_t)cap, _TRUNCATE,
                "%s\\momentum\\momtv\\local\\%s\\%s.mtv", WrGameDir(), g_map,
                hash);
    if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES)
        return true;

    out[0] = '\0';
    return false;
}

static void RefreshFacts(void)
{
    for (int i = 0; i < g_pickCount; i++)
    {
        WrQuickPick *p = &g_picks[i];
        if (p->done || p->failed)
            continue;
        p->inStore = (RunWithHash(p->hash) != NULL);
        if (!p->inStore)
        {
            char path[MAX_PATH];
            p->haveDemo = DemoPath(p->hash, path, sizeof(path));
        }
    }
}

// ---------------------------------------------------------------------------
// The chain
// ---------------------------------------------------------------------------

static void SubmitBoard(void)
{
    WrExtractRequest req = {WR_JOB_BOARD};
    strcpy_s(req.map, sizeof(req.map), g_map);
    req.mapId = g_mapId;
    req.gamemode = g_quick.gamemode;
    req.trackType = g_wantBoardType;
    req.trackNum = g_wantBoardNum;
    req.boardMode = WR_BOARD_WINDOW;
    req.fromRank = g_wantBoardFrom;
    req.count = g_wantBoardCount;

    if (WrExtractSubmit(&req))
    {
        g_wantBoard = false;
        _snprintf_s(g_note, sizeof(g_note), _TRUNCATE,
                    "reading the %s leaderboard...",
                    WrTrackNameOf(g_wantBoardType, g_wantBoardNum));
    }
}

static void SubmitFetch(void)
{
    // Every pick that wants a download, in ONE request. The fetcher dedupes by
    // hash against what is already on disk before it asks for anything, so
    // including a run we turn out to hold costs a lookup and no request -- which
    // is why batching is safe as well as polite.
    int ranks[WR_QUICK_MAX_PICKS];
    int n = 0;
    unsigned char type = 0, num = 1;
    for (int i = 0; i < g_pickCount && n < WR_QUICK_MAX_PICKS; i++)
    {
        WrQuickPick *p = &g_picks[i];
        if (p->done || p->failed || p->inStore || p->fetched || p->rank <= 0)
            continue;
        // One leg per request: a fetch is scoped to a leaderboard, and asking
        // for rank 3 without saying which board is a different run.
        if (n == 0) { type = p->trackType; num = p->trackNum; }
        else if (p->trackType != type || p->trackNum != num) continue;
        ranks[n++] = p->rank;
    }
    if (n == 0)
        return;

    WrExtractRequest req = {WR_JOB_FETCH};
    strcpy_s(req.map, sizeof(req.map), g_map);
    req.mapId = g_mapId;
    req.gamemode = g_quick.gamemode;
    req.trackType = type;
    req.trackNum = num;
    req.ranks = ranks;
    req.rankCount = n;

    if (!WrExtractSubmit(&req))
        return;                     // somebody took the slot; try next tick

    for (int i = 0; i < g_pickCount; i++)
    {
        WrQuickPick *p = &g_picks[i];
        if (p->done || p->failed || p->inStore || p->fetched)
            continue;
        if (p->trackType == type && p->trackNum == num)
            p->fetched = true;
    }
    _snprintf_s(g_note, sizeof(g_note), _TRUNCATE,
                "downloading %d run%s...", n, n == 1 ? "" : "s");
}

static void SubmitExtract(int index)
{
    WrQuickPick *p = &g_picks[index];

    char file[MAX_PATH];
    if (!DemoPath(p->hash, file, sizeof(file)))
        return;                     // RefreshFacts will call it missing

    // The walk-free path. A whole-map extraction would work too and would cost a
    // pass over every demo in three trees to find the one we are already holding
    // the name of -- and it would extract every OTHER unprocessed demo for the
    // map as well, which is minutes of work nobody asked for.
    WrExtractRequest req = {WR_JOB_EXTRACT};
    strcpy_s(req.file, sizeof(req.file), file);
    req.timeoutSeconds = WrExtractTimeout();
    req.jobs = 1;                   // one demo; a pool would be four idle threads

    if (!WrExtractSubmit(&req))
        return;

    p->extracted = true;
    _snprintf_s(g_note, sizeof(g_note), _TRUNCATE, "reading a demo...");
}

// Make the store agree with the ticks.
//
// Called after every store reload, because FinishLoad turns everything off and
// WrUpdateNearest then turns exactly one run on -- so without this, finishing
// one extraction would silently untick every line already on screen.
static void ApplyPicks(void)
{
    int on = 0;
    for (int i = 0; i < g_pickCount; i++)
    {
        WrRun *r = RunWithHash(g_picks[i].hash);
        if (!r)
            continue;
        r->enabled = true;
        on++;
    }
    if (on > 0)
    {
        // The auto-enable runs LATER in the frame, from inside the renderer, and
        // clears every run it did not choose. Somebody has said what they want;
        // that is exactly when guessing has to stop.
        WrPathCancelAutoEnable();
    }
}

void WrQuickTick(void)
{
    if (!g_map[0])
        return;

    const unsigned int egen = WrExtractRunGeneration();
    const unsigned int sgen = WrRunStoreGeneration();
    bool poke = false;

    bool boardJustRan = false;
    if (egen != g_lastExtractGen)
    {
        g_lastExtractGen = egen;
        g_rowsStale = true;         // a board or fetch may have rewritten it
        boardJustRan = (WrExtractLastKind() == WR_JOB_BOARD);
        poke = true;
    }
    if (sgen != g_lastStoreGen)
    {
        g_lastStoreGen = sgen;
        RebuildLegs();
        ApplyPicks();
        RefreshRowState();
        poke = true;
    }

    if (g_picksDirty)
        SavePicks();

    // Everything below opens files. Do it on a change, or four times a second,
    // and not on every one of three hundred frames.
    const unsigned long long now = GetTickCount64();
    if (!poke && now < g_nextPollAt)
        return;
    g_nextPollAt = now + 250;

    if (g_rowsStale)
    {
        ReloadRows();
        RefreshRowState();
    }

    // A read that finished and left the table empty. Said out loud, because the
    // alternative is a page that quietly stays blank and looks broken -- and the
    // usual cause is not a failure at all: plenty of stage boards have nobody on
    // them. The full panel's Board tab carries the actual error text if there
    // was one.
    if (boardJustRan && g_rowCount == 0)
        strcpy_s(g_note, sizeof(g_note),
                 "nothing came back for this leg -- it may simply have no runs");

    // Fill the page in. Here rather than in the draw path, so it happens on a map
    // change whether or not the panel is open -- by the time you press Delete the
    // board is already there.
    MaybeAutoRead();

    const bool busy = WrExtractRunning();

    // A board the page asked for. Ahead of the demo chain because there is
    // nothing to download until the leaderboard says what rank 3 is.
    if (g_wantBoard && !busy)
    {
        SubmitBoard();
        return;
    }

    if (g_pickCount == 0)
    {
        // Not unconditionally: the "nothing came back" line above is set on a
        // page with no ticks by definition, and clearing it here would wipe it
        // on the very next tick.
        if (!boardJustRan)
            g_note[0] = '\0';
        return;
    }

    RefreshFacts();

    int done = 0, working = 0;
    for (int i = 0; i < g_pickCount; i++)
    {
        if (g_picks[i].done) done++;
        else if (!g_picks[i].failed) working++;
    }

    int loadDone = 0, loadTotal = 0;
    const bool loading = WrPathLoading(&loadDone, &loadTotal);

    int idx = -1;
    const WrQuickAction act = WrQuickDecide(g_picks, g_pickCount, busy, loading,
                                            &idx);
    switch (act)
    {
    case WQ_ENABLE:
    {
        WrRun *r = RunWithHash(g_picks[idx].hash);
        if (r)
        {
            r->enabled = true;
            WrPathCancelAutoEnable();
        }
        g_picks[idx].done = true;
        g_nextPollAt = 0;           // there may be more to enable this frame
        break;
    }
    case WQ_FETCH:
        SubmitFetch();
        break;
    case WQ_EXTRACT:
        SubmitExtract(idx);
        break;
    case WQ_GIVE_UP_NO_DEMO:
    case WQ_GIVE_UP_NO_PATH:
        g_picks[idx].failed = true;
        strcpy_s(g_picks[idx].why, sizeof(g_picks[idx].why),
                 WrQuickGiveUpReason(act));
        WrLogf("quick: giving up on %s -- %s", g_picks[idx].hash,
               g_picks[idx].why);
        g_nextPollAt = 0;
        break;
    case WQ_NOTHING:
    default:
        if (working == 0)
            g_note[0] = '\0';
        else if (busy)
            strcpy_s(g_note, sizeof(g_note), "waiting for the other panel's job");
        else if (loading)
            strcpy_s(g_note, sizeof(g_note), "loading the lines...");
        break;
    }
}

void WrQuickOnMapChanged(const char *map)
{
    // BEFORE g_map changes, because that is what SavePicks writes the section
    // for. A tick made on the frame the level changed would otherwise be written
    // under the new map's name, or lost -- and "I ticked it and it forgot" is
    // the one failure a remembered list must not have.
    if (g_picksDirty)
        SavePicks();

    strcpy_s(g_map, sizeof(g_map), map ? map : "");
    g_mapId = 0;
    g_leg = 0;
    g_legCount = 0;
    g_rowsStale = true;
    g_rowCount = 0;
    g_wantBoard = false;
    g_showTop = g_quick.top;
    g_askedCount = 0;           // a new map is a new set of legs to ask about
    g_note[0] = '\0';
    g_nextPollAt = 0;

    LoadPicks();
    RebuildLegs();
}

// There is no WrQuickShutdown, and the absence is deliberate -- see the same
// note at the bottom of wr_extract.h. Nothing here needs one: a changed tick
// list is written by the next WrQuickTick, which is the next frame, and the
// write is a temp file plus an atomic replace. A shutdown function with no
// reachable caller reads as a lifecycle that exists.

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------

static void Marker(const char *text)
{
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static void TimeText(double t, char *out, int cap)
{
    const int mins = (int)(t / 60.0);
    const double secs = t - mins * 60.0;
    if (mins > 0)
        _snprintf_s(out, (size_t)cap, _TRUNCATE, "%d:%06.3f", mins, secs);
    else
        _snprintf_s(out, (size_t)cap, _TRUNCATE, "%.3f", secs);
}

static void DrawLegChips(void)
{
    const float avail = ImGui::GetContentRegionAvail().x;
    float x = 0.0f;
    for (int i = 0; i < g_legCount; i++)
    {
        char label[40];
        strcpy_s(label, sizeof(label), WrTrackNameOf(g_legs[i].type, g_legs[i].num));

        const float w = ImGui::CalcTextSize(label).x +
                        ImGui::GetStyle().FramePadding.x * 2.0f;
        if (i > 0 && x + w < avail)
            ImGui::SameLine();
        else
            x = 0.0f;
        x += w + ImGui::GetStyle().ItemSpacing.x;

        const bool selected = (i == g_leg);
        if (selected)
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(label))
        {
            g_leg = i;
            g_rowsStale = true;
            g_showTop = g_quick.top;    // back to the top of the new leg
            // The tick that fills the page in. MaybeAutoRead does the deciding;
            // this only has to make the next one happen without waiting out the
            // poll interval, so a chip press feels like a chip press.
            g_nextPollAt = 0;
        }
        if (selected)
            ImGui::PopStyleColor();
    }
}

static void DrawColourRow(void)
{
    ImGui::SeparatorText("Colour");

    static const char *kNames[WR_LINE_MODE_COUNT] = {
        "off", "speed", "energy", "energy (relative)", "efficiency"
    };
    for (int i = 0; i < WR_LINE_MODE_COUNT; i++)
    {
        if (i > 0)
            ImGui::SameLine();
        if (ImGui::RadioButton(kNames[i], g_render.lineColour == i))
            g_render.lineColour = i;
    }

    bool byRank = (g_render.rankColour != WR_RANK_OFF);
    if (ImGui::Checkbox("by rank", &byRank))
        g_render.rankColour = byRank ? WR_RANK_BY_PLACING : WR_RANK_OFF;
    ImGui::SameLine();
    Marker("Colours each whole line by where it placed, rather than colouring "
           "ALONG the line the way the row above does. The two compose: rank "
           "sets what a run's base colour is and efficiency shades from it.");

    ImGui::SameLine();
    ImGui::Checkbox("scale to what is on", &g_render.autoScale);
    ImGui::SameLine();
    Marker("Takes the ends of the colour range from the runs you actually have "
           "on, on the leg you are looking at, instead of from two fixed "
           "numbers.\n\n"
           "It matters most on the slow end of a board. A 250-3500 speed ramp "
           "spends most of its length on speeds a learner's run never reaches, "
           "so every line looks the same colour; scaled to the runs on screen, "
           "the ramp covers what those runs actually did.\n\n"
           "Turning it off puts your own sliders back exactly as they were.");
}

void WrQuickDraw(void)
{
    ImGui::SetNextWindowSize(ImVec2(640.0f, 520.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(120.0f, 120.0f), ImGuiCond_FirstUseEver);

    bool open = true;
    if (!ImGui::Begin("WrLines Quick", &open, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        if (!open)
            WrSetPanelOpen(WR_PANEL_QUICK, false);
        return;
    }

    if (!g_map[0])
    {
        ImGui::TextDisabled("No map loaded yet.");
        ImGui::End();
        if (!open)
            WrSetPanelOpen(WR_PANEL_QUICK, false);
        return;
    }

    ImGui::Text("%s", g_map);
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", WrGamemodeName(g_quick.gamemode));
    ImGui::SameLine();
    Marker("Everything on this page is about the map you are standing in. The "
           "full panel on Insert can look at any map, and has every setting "
           "this one leaves out.");

    // --- consent ------------------------------------------------------------
    if (!g_quick.network)
    {
        ImGui::Separator();
        ImGui::TextWrapped(
            "This page reads Momentum's public leaderboard to know which runs "
            "exist. Nothing is downloaded until you tick a run.");
        if (ImGui::Button("Allow it to look"))
        {
            g_quick.network = true;
            g_nextPollAt = 0;
        }
        ImGui::SameLine();
        Marker("One press, remembered. The rest of the tool asks once per "
               "session; this page asks once, because a page that fills itself "
               "in is not much use if it has to be switched on first every "
               "time.\n\n"
               "It reads one leaderboard: the leg you are looking at, when "
               "nothing is cached for it. Demo files are only ever downloaded "
               "for runs you tick.");
    }

    // --- legs ---------------------------------------------------------------
    ImGui::Separator();
    DrawLegChips();

    if (!WrMapsTracksKnown())
    {
        ImGui::TextDisabled("Stage list is a guess.");
        ImGui::SameLine();
        Marker("The game keeps a catalogue of every map on disk, and it says "
               "exactly how many stages and bonuses each one has. It has not "
               "been read yet, so the legs above are whatever the runs and "
               "boards you already hold happen to cover.\n\n"
               "The Maps tab of the full panel has a Rebuild button that reads "
               "it. One press, no network, and it answers for every map at "
               "once.");
    }

    // --- the board ----------------------------------------------------------
    const unsigned char legType = g_legCount ? g_legs[g_leg].type : 0;
    const unsigned char legNum = g_legCount ? g_legs[g_leg].num : 1;

    if (g_rowsStale)
    {
        ReloadRows();
        RefreshRowState();
    }

    if (g_rowCount == 0)
    {
        ImGui::TextDisabled("Nothing cached for this leg yet.");
        const bool busy = WrExtractRunning();
        if (!g_quick.network)
        {
            ImGui::TextDisabled("Allow it to look, above, and this fills itself in.");
        }
        else if (g_wantBoard || busy)
        {
            ImGui::TextDisabled("reading the leaderboard...");
        }
        else
        {
            // Reachable when the automatic read has already been spent on this
            // leg and came back with nothing -- an empty stage board, or a
            // request that failed. Asking again is then a decision rather than a
            // default, so it is a button.
            char lbl[64];
            _snprintf_s(lbl, sizeof(lbl), _TRUNCATE, "Read the top %d", g_quick.top);
            if (ImGui::Button(lbl))
                WantBoard(legType, legNum, 1, g_quick.top);
        }
    }
    else if (g_boardFetched > 0)
    {
        long long age = (long long)time(NULL) - g_boardFetched;
        if (age < 0) age = 0;
        if (age < 5400)        ImGui::TextDisabled("read %lld minutes ago", age / 60);
        else if (age < 172800) ImGui::TextDisabled("read %lld hours ago", age / 3600);
        else                   ImGui::TextDisabled("read %lld days ago", age / 86400);
        if (g_boardTotal > 0)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("-- %d runs on this board", g_boardTotal);
        }
    }

    // --- the table ----------------------------------------------------------
    if (g_rowCount > 0)
    {
        const float footer = ImGui::GetFrameHeightWithSpacing() * 5.5f;
        if (ImGui::BeginTable("quickrows", 5,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingStretchProp,
                              ImVec2(0.0f, -footer)))
        {
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 26.0f);
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("time", ImGuiTableColumnFlags_WidthFixed, 78.0f);
            ImGui::TableSetupColumn("runner");
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            for (int i = 0; i < g_rowCount; i++)
            {
                const WrBoardRow *r = &g_rows[i];
                ImGui::TableNextRow();
                ImGui::PushID(i);

                const int pi = FindPick(r->hash);
                bool ticked = (pi >= 0);

                ImGui::TableSetColumnIndex(0);
                if (ImGui::Checkbox("##on", &ticked))
                {
                    if (ticked) PickAdd(r, legType, legNum);
                    else        PickRemove(r->hash);
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%d", r->rank);

                ImGui::TableSetColumnIndex(2);
                char t[32];
                TimeText(r->time, t, sizeof(t));
                ImGui::TextUnformatted(t);

                ImGui::TableSetColumnIndex(3);
                {
                    // The current Steam persona when we know it, because that is
                    // the name written on the line in the world -- the alias in
                    // the cache is whatever it was when the board was fetched.
                    const char *persona = WrSteamPersona(r->steamId);
                    ImGui::TextUnformatted((persona && *persona) ? persona : r->alias);
                }

                // ONE WORD, and the sentence on hover and underneath.
                //
                // It used to print the whole reason here and the column clipped
                // it -- "that demo could no", with no way to widen the column and
                // no way to reach the rest. A status column has to fit in a
                // status column; anything longer belongs somewhere it can wrap.
                ImGui::TableSetColumnIndex(4);
                if (pi >= 0 && g_picks[pi].failed)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1.0f), "gave up");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", g_picks[pi].why);
                }
                else if (pi >= 0 && g_picks[pi].done)
                    ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.55f, 1.0f), "drawn");
                else if (pi >= 0)
                    ImGui::TextDisabled("working");
                else if (g_rowState[i] == ROW_READY)
                    ImGui::TextDisabled("ready");
                else if (g_rowState[i] == ROW_ON_DISK)
                    ImGui::TextDisabled("on disk");

                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // --- more of the board ----------------------------------------------
        //
        // The cache ACCUMULATES, so this is two different things wearing one
        // button: if the file already holds more places than the table is
        // showing -- fetched from the Board tab, or from a previous press -- it
        // costs nothing at all and simply shows them. Only when it does not is a
        // request made, and then for one page.
        if (g_showTop < WR_QUICK_TOP_MAX)
        {
            const bool haveMore = (g_rowCount > 0 && g_rowCount >= g_showTop);
            const bool boardHasMore = (g_boardTotal <= 0 || g_boardTotal > g_rowCount);
            if (haveMore && boardHasMore)
            {
                char lbl[64];
                _snprintf_s(lbl, sizeof(lbl), _TRUNCATE, "Show %d more##more",
                            SHOW_MORE_STEP);
                if (ImGui::Button(lbl))
                {
                    g_showTop += SHOW_MORE_STEP;
                    if (g_showTop > WR_QUICK_TOP_MAX)
                        g_showTop = WR_QUICK_TOP_MAX;
                    g_rowsStale = true;
                    ReloadRows();
                    RefreshRowState();
                    // Still short after re-reading, so the places are genuinely
                    // not cached and this is where a request is owed.
                    if (g_rowCount < g_showTop)
                        WantBoard(legType, legNum, g_rowCount + 1,
                                  g_showTop - g_rowCount);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("showing %d", g_rowCount);
                ImGui::SameLine();
                Marker("Adds the next twenty places. If they are already cached "
                       "-- from the Board tab, or from pressing this before -- "
                       "it costs no request at all.\n\n"
                       "The full panel is the place to go past a hundred, or to "
                       "reach the slow end of a board, which is often the more "
                       "useful end: a 37-second world record is not a line a "
                       "learner can trace and the run at rank 9,000 is.");
            }
        }
    }

    // --- the buttons --------------------------------------------------------
    if (g_rowCount > 0)
    {
        if (ImGui::Button("Top 5"))
            for (int i = 0; i < 5 && i < g_rowCount; i++)
                PickAdd(&g_rows[i], legType, legNum);
        ImGui::SameLine();
        if (ImGui::Button("Clear"))
        {
            while (g_pickCount > 0)
                PickRemove(g_picks[g_pickCount - 1].hash);
        }
        ImGui::SameLine();
        Marker("Clear unticks every run on every leg of this map, not only the "
               "leg you are looking at.");
        ImGui::SameLine();

        int done = 0, working = 0, failed = 0;
        for (int i = 0; i < g_pickCount; i++)
        {
            if (g_picks[i].failed)    failed++;
            else if (g_picks[i].done) done++;
            else                      working++;
        }
        if (working || failed)
            ImGui::TextDisabled("%d drawn, %d working, %d gave up", done, working,
                                failed);
        else
            ImGui::TextDisabled("%d drawn", done);

        // The reason, in full, where there is room for it to wrap. The cell in
        // the table has one word and a tooltip; a tooltip you have to find is not
        // where the only copy of an error message should live.
        if (failed)
        {
            const char *why = "";
            for (int i = 0; i < g_pickCount; i++)
                if (g_picks[i].failed && g_picks[i].why[0])
                    why = g_picks[i].why;
            if (*why)
                ImGui::TextWrapped("Gave up on %d: %s.", failed, why);
        }
    }

    // OUTSIDE the row test. What the page is doing is most worth saying when
    // there is nothing in the table, which is exactly the state the old
    // placement could not reach.
    if (g_note[0])
        ImGui::TextDisabled("%s", g_note);

    DrawColourRow();

    ImGui::End();

    if (!open)
        WrSetPanelOpen(WR_PANEL_QUICK, false);
}
