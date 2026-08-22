// wr_quick.cpp  --  see wr_quick.h.

#include "wr_quick.h"
#include "wr_board.h"
#include "wr_extract.h"
#include "wr_hook.h"
#include "wr_log.h"
#include "wr_maps.h"
#include "wr_path.h"
#include "wr_render.h"
#include "wr_scan.h"            // the rescan button; see DrawScanRow
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

// Which leaderboard this map is read on, resolved once per map. See the essay
// over WrQuickGamemodeGuess in wr_quick.h: this was g_quick.gamemode read
// directly, which asked the surf board of every map in the game.
static int g_mode = 1;

// A mode chosen by hand for this map, or 0. Kept apart from g_mode because it is
// the only part of the resolution worth writing to disk -- the other three steps
// re-derive themselves for free next time.
static int g_modePicked = 0;

static Leg g_legs[MAX_LEGS];
static int g_legCount = 0;
static int g_leg = 0;               // index into g_legs, DERIVED from the pair below

// The leg being looked at, as an identity rather than a position.
//
// An index into a list that is rebuilt whenever the store moves is not a
// selection, it is a guess about a list -- and the list is momentarily empty
// during every reload. See RebuildLegs.
static unsigned char g_selType = 0, g_selNum = 1;

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
static bool g_wantBoardSlowest = false; // the last `count`, not from `from`

// How many places the table is showing. Starts at the setting and grows by the
// same step each time "show more" is pressed, up to the row array.
//
// Per session and per leg rather than persisted: it is where you have scrolled
// to, not a preference, and coming back to a map tomorrow wanting the top twenty
// again is the ordinary case.
static int g_showTop = WR_QUICK_TOP_DEFAULT;

// Which end of the board the table is showing.
//
// WHY THE SLOW END IS WORTH A CONTROL. The fastest runs are the hardest to
// follow: a 37-second surf_demise world record is not a line a learner can
// trace, and the 79-second run at rank 9108 is. wr_board.h has said so since the
// Board tab was written, and this page -- the one aimed at somebody who has just
// installed the thing -- could only ever show the top.
//
// It is a VIEW and not just a fetch, because the cache accumulates: after asking
// for both ends, one rank-sorted file holds ranks 1-20 and 9089-9108, and a
// reader that takes the first twenty lines will never show the second group no
// matter how many times it is fetched.
enum { END_FAST = 0, END_SLOW };
static int g_end = END_FAST;

// Which legs this session has already offered to read, so that switching back
// and forth across the chips is not one request each way.
//
// A leg that came back EMPTY is in here too, and that is the point: a stage with
// no runs would otherwise be asked about every single time it is selected, for
// ever, and the answer would be the same every time.
//
// Keyed by (leg, end): the two ends of a board are two different questions, and
// having asked about the top of a stage says nothing about whether its tail has
// been looked at.
static unsigned short g_asked[MAX_LEGS_ASKED];
static int g_askedCount = 0;

// The display order. A separate array rather than sorting g_rows, and that is
// load-bearing: the ticks are keyed by hash, the status cells and g_rowState are
// keyed by ROW INDEX, and a sort applied to the rows but not to one of those
// would put "gave up" on somebody else's run.
static int g_order[WR_QUICK_TOP_MAX];
static int g_orderCount = 0;
static bool g_resort = true;

static unsigned int g_lastExtractGen = 0;
static unsigned int g_lastStoreGen = 0;
static unsigned long long g_nextPollAt = 0;

// What the chain is doing, for the line under the table.
static char g_note[160] = "";

// How many runs are drawn that this page did not tick.
//
// CACHED, for the reason g_rowState is: EnabledNotPicked walks the whole store
// against the whole pick list, which is sixty-four thousand string compares on
// a thousand-run map, and the draw path would spend it three hundred times a
// second to answer a question whose answer changes when a job finishes or a
// tick moves.
static int g_extraOn = 0;

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

// ---------------------------------------------------------------------------
// Which leaderboard
// ---------------------------------------------------------------------------

// The gamemode of a board we already hold for this map, or 0.
//
// A useful signal for names that do not identify one discipline, and it costs
// one directory listing with no file opened, because the filename carries it:
// boards\<map>_g<mode>_t<type><num>.tsv. It is not a guess and not a preference
// -- it is the mode somebody already fetched this map in, from the Board tab or
// from this page. A conventional map prefix wins over it: an old accidental
// surf cache beside bhop_hades must not redefine that map's default discipline.
//
// The main track wins over a stage when both are cached, since that is the leg
// this page opens on; a lower mode number breaks any remaining tie, so the answer
// cannot depend on the order the filesystem happens to enumerate in.
static int ModeFromCache(void)
{
    if (!g_map[0])
        return 0;

    char pat[MAX_PATH];
    char rel[224];
    _snprintf_s(rel, sizeof(rel), _TRUNCATE, "boards\\%s_g*_t*.tsv", g_map);
    strcpy_s(pat, sizeof(pat), WrDataPath(rel));

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;

    int best = 0;
    bool bestIsMain = false;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        int mode = 0, type = 0, num = 0;
        if (!WrQuickParseBoardName(fd.cFileName, g_map, &mode, &type, &num))
            continue;

        const bool isMain = (type == 0);
        if (best == 0 || (isMain && !bestIsMain) ||
            (isMain == bestIsMain && mode < best))
        {
            best = mode;
            bestIsMain = isMain;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return best;
}

// The four steps, in order of how much they know. See the essay over
// WrQuickGamemodeGuess in wr_quick.h.
//
// A HAND-MADE CHOICE COMES FIRST, ahead of the cache, and it has to: most maps
// worth looking at already have a board cached, so a cache that outranked the
// picker would be a picker that did nothing on every map where it mattered. The
// remembered choice IS the correction, and a correction that can be overruled by
// the thing it is correcting is not one.
static void ResolveMode(void)
{
    int mode = g_modePicked;
    if (mode < 1)
        mode = WrQuickGamemodeGuess(g_map);
    if (mode < 1)
        mode = ModeFromCache();
    if (mode < 1 || mode > WR_GAMEMODE_COUNT)
        mode = g_quick.gamemode;
    if (mode < 1 || mode > WR_GAMEMODE_COUNT)
        mode = 1;

    // Every map change, not only when the answer differs from the last map's.
    // This is the line that would have answered the bhop_hades report in one
    // run instead of by inference from job timings, and one line per level load
    // is not a busy log.
    WrLogf("quick: %s reads as %s (gamemode %d)", g_map, WrGamemodeName(mode),
           mode);
    g_mode = mode;
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
                g_mode);
    strcpy_s(pat, sizeof(pat), WrDataPath(rel));

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    // WrQuickParseBoardName, shared with ModeFromCache above, and shared rather
    // than written twice because the version that used to live here was WRONG
    // for two releases and added nothing at all -- see the essay over it in
    // wr_quick.h. Two readers of one filename should not be two chances to get
    // that filename wrong.
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        int mode = 0, type = 0, num = 0;
        if (!WrQuickParseBoardName(fd.cFileName, g_map, &mode, &type, &num))
            continue;
        if (mode != g_mode)
            continue;               // the glob is per mode, but say so anyway
        LegAdd((unsigned char)type, (unsigned char)num);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void RebuildLegs(void)
{
    const unsigned char wasType = g_selType;
    const unsigned char wasNum = g_selNum;

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

    // The leg being LOOKED AT is kept whether or not anything currently vouches
    // for it, which is the second half of the bug above.
    //
    // WrPathLoadMap bumps the store generation the moment it frees the old runs,
    // so the first rebuild after a download runs against an EMPTY store. Every
    // source of legs is empty at that instant, the selected stage is not in the
    // list, and a selection held as an index into that list has nowhere to go
    // but back to the main track -- where it then stays, because the next
    // rebuild has already forgotten what it was. Reported as "ticking a run on a
    // stage sends me back to main when it downloads".
    //
    // So the selection is (type, num) and the index is derived from it. A leg
    // nothing else knows about is added rather than dropped.
    LegAdd(wasType, wasNum);

    qsort(g_legs, (size_t)g_legCount, sizeof(Leg), LegOrder);

    g_leg = 0;
    for (int i = 0; i < g_legCount; i++)
        if (g_legs[i].type == wasType && g_legs[i].num == wasNum)
            g_leg = i;
    g_selType = g_legs[g_leg].type;
    g_selNum = g_legs[g_leg].num;
}

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

static void BoardPathForLeg(char *out, int cap, int leg)
{
    const unsigned char type = (leg >= 0 && leg < g_legCount) ? g_legs[leg].type : 0;
    const unsigned char num = (leg >= 0 && leg < g_legCount) ? g_legs[leg].num : 1;
    WrBoardCachePath(out, cap, g_map, g_mode, type, num);
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

    // Which end. The cache holds every window ever asked for in one rank-sorted
    // file, so this is the only thing that decides whether the tail is reachable
    // at all.
    int mapId = 0;
    int n = (g_end == END_SLOW)
                ? WrBoardParseTail(path, g_rows, top, &g_boardTotal,
                                   &g_boardFetched, &mapId)
                : WrBoardParseFile(path, g_rows, top, &g_boardTotal,
                                   &g_boardFetched, &mapId);
    if (n < 0)
        return;                     // nothing cached; not an error
    g_rowCount = n;
    if (mapId > 0)
        g_mapId = mapId;

    // The order is about these rows and they have just been replaced.
    g_resort = true;
}

static WrRun *RunWithHash(const char *hash);
static bool DemoPath(const char *hash, char *out, int cap);
static int FindPick(const char *hash);

// ---------------------------------------------------------------------------
// Asking for a board
// ---------------------------------------------------------------------------

// The leg and which end of it, in one value. The end is the top bit of the type
// byte, which is free: trackType is 0, 1 or 2.
static unsigned short LegKey(unsigned char type, unsigned char num)
{
    const unsigned short t = (unsigned short)(type | (g_end == END_SLOW ? 0x80 : 0));
    return (unsigned short)((t << 8) | num);
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
    g_wantBoardSlowest = false;
    g_nextPollAt = 0;
}

// The tail, which is one request rather than a walk down to it: every page the
// API returns carries totalCount, so the pager reads the size and then lands on
// the end. WR_BOARD_SLOWEST is that, and it has been in the job dispatch since
// the Board tab was written.
static void WantSlowest(unsigned char type, unsigned char num, int count)
{
    if (!g_quick.network)
        return;
    g_wantBoard = true;
    g_wantBoardType = type;
    g_wantBoardNum = num;
    g_wantBoardFrom = 0;
    g_wantBoardCount = count < 1 ? SHOW_MORE_STEP : count;
    g_wantBoardSlowest = true;
    g_nextPollAt = 0;
}

// The automatic half: the leg you are looking at, at the end you are looking
// at, once, when it is empty.
//
// IT FOLLOWS THE END, and originally it did not -- the tail was a press even
// after you had chosen to look at it. That was the wrong reading of "one request
// per leg": having selected the slow end, a leg switch that leaves the table
// blank until you press a button is the same complaint this page was built to
// answer, one view along. The traffic argument does not survive either, because
// the gate is what you are LOOKING at: it is still one request per leg per end
// per session, and it is still only for an end with nothing cached.
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
    if (g_end == END_SLOW)
        WantSlowest(type, num, g_quick.top);
    else
        WantBoard(type, num, 1, g_quick.top);
}

// How far along the chain each row is, as a number the status column can be
// sorted on. Exactly the order the cells are chosen in below, so what you read
// and what you sort by cannot disagree.
static int g_pickedRank[WR_QUICK_TOP_MAX];

static void RefreshPickedRank(void)
{
    for (int i = 0; i < g_rowCount; i++)
    {
        const int pi = FindPick(g_rows[i].hash);
        if (pi >= 0 && g_picks[pi].failed)      g_pickedRank[i] = 5;  // gave up
        else if (pi >= 0 && g_picks[pi].done)   g_pickedRank[i] = 0;  // drawn
        else if (pi >= 0)                       g_pickedRank[i] = 1;  // working
        else if (g_rowState[i] == ROW_READY)    g_pickedRank[i] = 2;
        else if (g_rowState[i] == ROW_ON_DISK)  g_pickedRank[i] = 3;
        else                                    g_pickedRank[i] = 4;
    }
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
    // The status column can be sorted on, and it has just changed under it.
    RefreshPickedRank();
    g_resort = true;
}

// ---------------------------------------------------------------------------
// The order the rows are drawn in
// ---------------------------------------------------------------------------
//
// Same shape as the Board tab's -- an order array, a file-static pointer to the
// specs for the duration of the qsort, and rank breaking every tie so the order
// cannot wobble between frames. Two tables sorting rows of the same type should
// not have two different sorts.
//
// EVERY COLUMN, INCLUDING THE TWO WITH NO HEADING. Those two are the reason this
// exists: the tick column sorts what you have asked for to the top, and the
// status column sorts by how far along the chain each row is. Neither has a name
// to click, so both get a heading of one space rather than nothing at all --
// ImGui gives an empty header no width to hit.

enum
{
    QCOL_TICK = 1,
    QCOL_RANK,
    QCOL_TIME,
    QCOL_RUNNER,
    QCOL_STATE,
};

static const ImGuiTableSortSpecs *g_specs = NULL;

// The name the table draws, and the name it sorts by.
//
// THE LEADERBOARD'S OWN ALIAS, not the live Steam persona, and that is a change
// from the first version of this page. The persona was preferred on the grounds
// that it is "the name written on the line in the world" -- but it is only
// available for a player somebody has already ASKED Steam about, and the only
// thing that asks is a name tag being drawn. So the name in this table changed
// the moment you ticked the row: alias until the line existed, persona
// afterwards, and back to alias for anybody Steam had nothing for. Reported as
// "when I tick some names, the names change".
//
// Asking here instead would be worse. WrSteamWant feeds a 96-slot cache that
// wr_render.cpp's tag code is careful to leave room in -- twelve tags by
// default, thirty-two at most -- and a hundred rows of leaderboard would take
// every slot and starve the tags for the rest of the session.
//
// So: the alias, which the board cache always carries, which never moves, and
// which is exactly what the full panel's Board tab shows in the same column.
// Two pages that show the same row now show the same name.
static const char *RowName(const WrBoardRow *r)
{
    return r->alias;
}

static int CompareQuickColumn(int ia, int ib, ImGuiID col)
{
    const WrBoardRow *a = &g_rows[ia], *b = &g_rows[ib];
    switch (col)
    {
    case QCOL_TICK:
    {
        // Ticked first, ascending -- "show me the ones I asked for".
        const int ta = (FindPick(a->hash) >= 0) ? 0 : 1;
        const int tb = (FindPick(b->hash) >= 0) ? 0 : 1;
        return ta - tb;
    }
    case QCOL_RANK:
        return a->rank == b->rank ? 0 : (a->rank < b->rank ? -1 : 1);
    case QCOL_TIME:
        return a->time == b->time ? 0 : (a->time < b->time ? -1 : 1);
    case QCOL_RUNNER:
        return _stricmp(RowName(a), RowName(b));
    case QCOL_STATE:
        // How far along: drawn, working, ready, on disk, nothing, gave up. Ties
        // fall through to rank below, so a page of untouched rows still reads
        // as a leaderboard.
        return g_pickedRank[ia] - g_pickedRank[ib];
    default:
        return 0;
    }
}

static int __cdecl CompareQuickRows(const void *pa, const void *pb)
{
    const int ia = *(const int *)pa, ib = *(const int *)pb;
    if (ia < 0 || ia >= g_rowCount || ib < 0 || ib >= g_rowCount || !g_specs)
        return 0;
    for (int s = 0; s < g_specs->SpecsCount; s++)
    {
        const ImGuiTableColumnSortSpecs *spec = &g_specs->Specs[s];
        const int c = CompareQuickColumn(ia, ib, spec->ColumnUserID);
        if (c != 0)
            return spec->SortDirection == ImGuiSortDirection_Ascending ? c : -c;
    }
    // Rank breaks every tie, so the order is total and cannot wobble.
    const int ra = g_rows[ia].rank, rb = g_rows[ib].rank;
    return ra == rb ? 0 : (ra < rb ? -1 : 1);
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

    if (g_pickCount > 0 || g_modePicked > 0)
    {
        fprintf(out, "[%s]\n", g_map);
        // Before the rows, so a hand-read file says which leaderboard the hashes
        // under it came off. An older build drops this line on the floor -- its
        // reader wants four fields and this has two -- so the file stays
        // readable in both directions, which is the same promise settings.cfg
        // makes.
        if (g_modePicked > 0)
            fprintf(out, "mode %d\n", g_modePicked);
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
    g_modePicked = 0;
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
        if (!ours)
            continue;

        // The leaderboard this map was corrected to, if it ever was. Read before
        // the row parse rather than after, because "mode 2" reads as a row with
        // two fields and the row parse would drop it either way -- but only this
        // order makes that a fact about the format instead of a coincidence.
        int picked = 0;
        if (sscanf_s(line, "mode %d", &picked) == 1)
        {
            if (picked >= 1 && picked <= WR_GAMEMODE_COUNT)
                g_modePicked = picked;
            continue;
        }

        if (g_pickCount >= WR_QUICK_MAX_PICKS)
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
    g_nextPollAt = 0;   // the counts under the table are about to be wrong
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
    req.gamemode = g_mode;
    req.trackType = g_wantBoardType;
    req.trackNum = g_wantBoardNum;
    req.boardMode = g_wantBoardSlowest ? WR_BOARD_SLOWEST : WR_BOARD_WINDOW;
    req.fromRank = g_wantBoardFrom;
    req.count = g_wantBoardCount;

    if (WrExtractSubmit(&req))
    {
        g_wantBoard = false;
        _snprintf_s(g_note, sizeof(g_note), _TRUNCATE,
                    "reading the %s of the %s leaderboard...",
                    g_wantBoardSlowest ? "slow end" : "top",
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
    req.gamemode = g_mode;
    req.trackType = type;
    req.trackNum = num;
    req.ranks = ranks;
    req.rankCount = n;

    if (!WrExtractSubmit(&req))
        return;                     // somebody took the slot; try next tick

    // `p->rank <= 0` again, and the omission was a real one: the loop above
    // skips a pick with no rank -- there is nothing to ask a leaderboard for --
    // while this one used to mark it fetched anyway. A pick recorded as having
    // had its one attempt without one being made goes straight to
    // WQ_GIVE_UP_NO_DEMO and reports "the download did not arrive", about a
    // download nobody ever asked for.
    for (int i = 0; i < g_pickCount; i++)
    {
        WrQuickPick *p = &g_picks[i];
        if (p->done || p->failed || p->inStore || p->fetched || p->rank <= 0)
            continue;
        if (p->trackType == type && p->trackNum == num)
            p->fetched = true;
    }
    _snprintf_s(g_note, sizeof(g_note), _TRUNCATE,
                "downloading %d run%s...", n, n == 1 ? "" : "s");
}

// How long ONE ticked demo may take. See WR_EXTRACT_TIMEOUT_TICK: the global
// default is fitted to a batch of thousands and is four times too tight for a
// single run off a leaderboard, which is where 403 of this machine's 415
// recorded failures came from.
//
// It takes the LARGER of the two, so somebody who has already raised the slider
// in the full panel is not quietly lowered to 120 -- and 0 there means "no
// limit", which has to win rather than lose a max().
static int TickTimeout(bool retrying)
{
    if (retrying)
        return 0;                   // the second press is the deliberate one
    const int global = WrExtractTimeout();
    if (global == 0)
        return 0;
    return global > WR_EXTRACT_TIMEOUT_TICK ? global : WR_EXTRACT_TIMEOUT_TICK;
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
    req.timeoutSeconds = TickTimeout(p->retried);
    req.jobs = 1;                   // one demo; a pool would be four idle threads

    if (!WrExtractSubmit(&req))
        return;

    p->extracted = true;
    if (req.timeoutSeconds == 0)
        _snprintf_s(g_note, sizeof(g_note), _TRUNCATE,
                    "reading a demo, with no time limit this time...");
    else
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
    if (on == 0)
        return;

    // EXACTLY the picks, which means turning the others OFF as well.
    //
    // Turning them on was only half of it, and the missing half was reported as
    // "ticking a run also loads the first place run". FinishLoad sets
    // `enabled = (i == 0)` on a store that is sorted by time -- so run 0 IS
    // first place, and every reload switched it on. This function then added the
    // ticked run beside it and cancelled the auto-enable, which is the one thing
    // that would have cleared it. Two lines, one tick, and the Runs tab and this
    // page reporting different pictures because they were reading different
    // truths.
    //
    // Nothing is lost by being absolute here: a reload has already cleared any
    // selection made in the Runs tab, so at this instant the enabled set is
    // entirely machine-chosen. If this page has been told what to show, it is
    // the only thing that has been told anything.
    for (int i = 0; i < WrRunCount(); i++)
    {
        WrRun *r = WrRunAt(i);
        if (!r || !r->enabled)
            continue;
        bool picked = false;
        for (int k = 0; k < g_pickCount && !picked; k++)
            picked = WrRunIsFrom(r, g_picks[k].hash);
        if (!picked)
            r->enabled = false;
    }

    // The auto-enable runs LATER in the frame, from inside the renderer, and
    // clears every run it did not choose. Somebody has said what they want;
    // that is exactly when guessing has to stop.
    WrPathCancelAutoEnable();
}

// Runs drawn that this page did not ask for: the auto-enable's choice on a map
// load, or anything ticked in the Runs tab.
//
// Counted rather than suppressed. With no picks at all, guessing is right --
// loading a map and seeing the nearest fastest line is the whole of the first
// impression -- but a line on screen with an empty tick list reads as this page
// lying about what is on. So it says so, and offers to clear them.
static int EnabledNotPicked(void)
{
    int n = 0;
    for (int i = 0; i < WrRunCount(); i++)
    {
        const WrRun *r = WrRunAt(i);
        if (!r || !r->enabled)
            continue;
        bool picked = false;
        for (int k = 0; k < g_pickCount && !picked; k++)
            picked = WrRunIsFrom(r, g_picks[k].hash);
        if (!picked)
            n++;
    }
    return n;
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

    g_extraOn = EnabledNotPicked();

    // A read that finished and left the table empty. Said out loud, because the
    // alternative is a page that quietly stays blank and looks broken.
    //
    // AND IT NAMES THE MODE, because "it may simply have no runs" was true of
    // every empty stage board and wrong about the one case that actually
    // happened: bhop_hades read on the SURF leaderboard, which is empty and
    // always will be. Both answers look identical from here -- a successful
    // request that returned nothing -- so the line has to give the user the one
    // fact that separates them, and the alternative to try.
    if (boardJustRan && g_rowCount == 0)
    {
        const int guess = WrQuickGamemodeGuess(g_map);
        if (guess >= 1 && guess != g_mode)
            _snprintf_s(g_note, sizeof(g_note), _TRUNCATE,
                        "nothing on the %s board for %s -- it reads like a %s "
                        "map, so try %s above",
                        WrGamemodeName(g_mode), g_map, WrGamemodeName(guess),
                        WrGamemodeName(guess));
        else
            _snprintf_s(g_note, sizeof(g_note), _TRUNCATE,
                        "nothing on the %s board for this leg -- it may simply "
                        "have no runs",
                        WrGamemodeName(g_mode));
    }

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
        g_picks[idx].done = true;
        // ApplyPicks rather than one r->enabled, because this is the path a
        // run you ALREADY HOLD takes: no download, no extraction, and therefore
        // no store reload to clean up after. Setting one bool here left
        // whatever the last reload had enabled -- run 0, which is first place
        // on a time-sorted store -- switched on beside it, with nothing coming
        // later to notice. Ticking one run, two lines.
        ApplyPicks();
        g_extraOn = 0;
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
    {
        WrQuickPick *p = &g_picks[idx];
        p->failed = true;
        strcpy_s(p->why, sizeof(p->why), WrQuickGiveUpReason(act));

        // "That demo could not be read" was true of three different things and
        // useful about none of them. The extractor knows which, so ask it --
        // and when the answer is the clock rather than the file, say so and
        // offer the second press. See WrExtractLastFileFailure.
        if (act == WQ_GIVE_UP_NO_PATH && !p->retried)
        {
            char base[64], why[192];
            bool timedOut = false;
            // SameHash and not WrRunIsFrom: both of these are FULL forty-
            // character hashes. The truncation WrRunIsFrom exists for is a
            // property of a .wrpath, and neither of these came out of one --
            // `base` is the .mtv's own basename, which the fetcher wrote from
            // the same leaderboard row this pick came from.
            if (WrExtractLastFileFailure(base, sizeof(base), why, sizeof(why),
                                         &timedOut) &&
                SameHash(base, p->hash))
            {
                strcpy_s(p->why, sizeof(p->why), why);
                p->canRetry = timedOut;
            }
        }

        WrLogf("quick: giving up on %s -- %s%s", p->hash, p->why,
               p->canRetry ? " (offering a second go with no time limit)" : "");
        g_nextPollAt = 0;
        break;
    }
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
    g_selType = 0;              // a new map opens on its main track
    g_selNum = 1;
    g_legCount = 0;
    g_rowsStale = true;
    g_rowCount = 0;
    g_wantBoard = false;
    g_showTop = g_quick.top;
    g_askedCount = 0;           // a new map is a new set of legs to ask about
    g_end = END_FAST;
    g_note[0] = '\0';
    g_nextPollAt = 0;

    // LoadPicks first: it is what recovers a mode chosen for this map before,
    // and ResolveMode reads that ahead of everything else. RebuildLegs last,
    // because the legs it lists come partly from the board cache and the board
    // cache is per gamemode -- rebuilding against the previous map's mode would
    // list the wrong stages for one frame and re-read the wrong board.
    LoadPicks();
    ResolveMode();
    RebuildLegs();
}

// Read this map on a different leaderboard, and remember it.
//
// Everything derived from the mode has to go, and the list is longer than it
// looks: the rows are from one board file, the legs come partly from which board
// files exist, and the asked-set records questions put to a leaderboard that is
// no longer the one being asked. Keeping any of them would show surf's stages
// over bhop's rows.
static void SetMode(int mode)
{
    if (mode < 1 || mode > WR_GAMEMODE_COUNT || mode == g_mode)
        return;

    g_modePicked = mode;
    g_mode = mode;
    g_mapId = 0;                // resolved from a board header; that board is gone
    g_rowsStale = true;
    g_rowCount = 0;
    g_wantBoard = false;
    g_showTop = g_quick.top;
    g_askedCount = 0;
    g_note[0] = '\0';
    g_nextPollAt = 0;

    RebuildLegs();
    SavePicks();
    WrLogf("quick: %s set to %s (gamemode %d) by hand", g_map,
           WrGamemodeName(mode), mode);
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
            g_selType = g_legs[i].type;     // the selection, not its position
            g_selNum = g_legs[i].num;
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

// TWO ROWS OF CIRCLES, not one row and a tick box, and not one row of six.
//
// The tick box was reported as looking wrong beside the radio buttons, and it
// did -- but the fix is not to fold rank in with them, because they are not the
// same kind of thing. The row below varies colour ALONG a line: speed at this
// point, energy at this point. Rank colours a whole run by where it placed. One
// is a measurement down the line and the other is an identity for the line, so
// they COMPOSE: rank sets what a run's base colour is, and efficiency shades
// from that base.
//
// Merging them into one exclusive list would have destroyed that -- and quietly,
// since nothing on screen would say the combination had stopped being available.
// Two rows of radio buttons answer the actual complaint (no tick box among the
// circles) and cost nothing.
//
// The second row also reaches WR_RANK_BY_TIME, which this page could not select
// at all before: not an even spread across the field, but shaded by how far off
// the best each run is -- so on a board where everyone is within a second of the
// record, everyone stays green, because they are all nearly as fast.
static void DrawColourRow(void)
{
    ImGui::SeparatorText("Colour");

    // Sized by the initialiser, NOT by the enum, with the count checked below.
    //
    // It was written the other way round -- kNames[WR_LINE_MODE_COUNT] with five
    // strings in it -- and when WR_LINE_PHASE was added the sixth slot became a
    // silent NULL. The loop under it counts to the enum, so it handed that NULL
    // to RadioButton, which hashes its label to make an ID, and ImHashStr walks
    // the string without a null check. The quick panel crashed the game on the
    // first frame it drew, every single time, from inside imgui.cpp.
    //
    // Both halves of the fix are here on purpose: WR_TABLE_IS_FULL makes the
    // next omission a build error, and WrLabel means that if one ever gets past
    // it anyway the widget reads "?" instead of closing the game.
    static const char *kNames[] = {
        "off", "speed", "energy", "energy (relative)", "efficiency", "phase"
    };
    WR_TABLE_IS_FULL(kNames, WR_LINE_MODE_COUNT);
    ImGui::TextDisabled("along the line");
    ImGui::SameLine();
    for (int i = 0; i < WR_LINE_MODE_COUNT; i++)
    {
        ImGui::SameLine();
        if (ImGui::RadioButton(WrLabel(kNames[i]), g_render.lineColour == i))
            g_render.lineColour = i;
    }

    static const char *kRank[] = {
        "off", "by placing", "by time"
    };
    WR_TABLE_IS_FULL(kRank, WR_RANK_MODE_COUNT);
    ImGui::TextDisabled("whole run");
    ImGui::SameLine();
    for (int i = 0; i < WR_RANK_MODE_COUNT; i++)
    {
        ImGui::SameLine();
        char id[48];
        _snprintf_s(id, sizeof(id), _TRUNCATE, "%s##rank", WrLabel(kRank[i]));
        if (ImGui::RadioButton(id, g_render.rankColour == i))
            g_render.rankColour = i;
    }
    ImGui::SameLine();
    Marker("Colours each whole line by where it placed on its own leg, rather "
           "than colouring ALONG the line the way the row above does.\n\n"
           "The two compose, which is why they are two rows: rank sets what a "
           "run's base colour is and efficiency shades from it. Turning one on "
           "does not turn the other off.\n\n"
           "by placing spreads the field evenly. by time shades by how far off "
           "the best each run is, so a board where everybody is within a second "
           "of the record stays green -- because they are all nearly as fast.");

    ImGui::Checkbox("scale to what is on", &g_render.autoScale);
    ImGui::SameLine();
    Marker("Takes the ends of the colour range from the runs you actually have "
           "on, instead of from two fixed numbers.\n\n"
           "It matters most on the slow end of a board. A 250-3500 speed ramp "
           "spends most of its length on speeds a learner's run never reaches, "
           "so every line looks the same colour; scaled to the runs on screen, "
           "the ramp covers what those runs actually did.\n\n"
           "Each LEG is scaled to its own runs. Turn on a stage as well as the "
           "main track and they get a ramp each, rather than one range covering "
           "the gap between them -- which would leave both of them in a fraction "
           "of it. The key on screen says so when it is happening.\n\n"
           "Turning it off puts your own sliders back exactly as they were.");

    ImGui::SetNextItemWidth(220.0f);
    ImGui::SliderFloat("line height", &g_render.lineHeightOffset,
                       -128.0f, 128.0f, "%+.0f u");
    ImGui::SameLine();
    Marker("Raises or lowers every drawn path without changing its physics or "
           "real position. Runs are recorded at the player's feet: 0 is feet "
           "level, +28 is roughly crouched eye level and +64 is standing eye "
           "level.");
}

// The one recovery this page offers for a thing that is not about runs at all.
//
// It is here rather than only in the Diagnostics tab because the symptom is
// visual and the person seeing it is a beginner: lines welded to the screen,
// lines in the wrong place, lines drifting further out towards the edges. All
// three are the same cause -- the wrong sixteen floats -- and all three are one
// press from fixed. Somebody who has to be told to open the other panel, find
// the ninth tab and scroll to a button called "Rescan" does not get told.
//
// Deliberately at the bottom, under the colour rows: it is the least likely
// thing to be needed and the most alarming to read, so it should not be the
// first thing the page says.
static void DrawScanRow(void)
{
    const bool busy = WrScanBusy();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Lines in the wrong place?"))
        WrScanRestart();
    ImGui::EndDisabled();
    ImGui::SameLine();
    Marker("Looks for the game's camera again, from scratch.\n\n"
           "This tool finds where the game keeps its world-to-screen matrix by "
           "reading its own memory -- it never calls into the game, which is "
           "what makes it safe -- and on a bad day it can settle on the wrong "
           "one. Two symptoms: lines that are right at the crosshair and drift "
           "further out towards the edges of the screen, which is a matrix "
           "belonging to a different field of view; and lines that stick to the "
           "screen while you move, which is one belonging to a different render "
           "pass.\n\n"
           "It is worth pressing after injecting at the main menu rather than "
           "in a map, which is the usual way to end up with a poor choice. "
           "Walking around for a couple of seconds afterwards is what lets it "
           "tell the candidates apart.\n\n"
           "The full panel's Diagnostics tab has the same button, plus the list "
           "of what it found and a way to pin one for good.");

    ImGui::SameLine();
    if (busy)
        ImGui::TextDisabled("looking...");
    else if (WrScanResolved())
        ImGui::TextDisabled("camera found");
    else
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f),
                           "no camera yet -- walk around for a second");
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
    Marker("Everything on this page is about the map you are standing in. The "
           "full panel on Insert can look at any map, and has every setting "
           "this one leaves out.");
    ImGui::SameLine();

    // The leaderboard, and it is a control rather than a label because the
    // resolution behind it is partly a guess. See WrQuickGamemodeGuess: a map
    // name says which discipline it is nearly always and not quite always, and
    // the climb family it cannot answer at all.
    {
        static const char *kModes[WR_GAMEMODE_COUNT];
        for (int i = 0; i < WR_GAMEMODE_COUNT; i++)
            kModes[i] = WrGamemodeName(i + 1);

        int idx = g_mode - 1;
        if (idx < 0 || idx >= WR_GAMEMODE_COUNT)
            idx = 0;
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("defrag CPM").x +
                                ImGui::GetFrameHeight() +
                                ImGui::GetStyle().FramePadding.x * 4.0f);
        if (ImGui::Combo("##mode", &idx, kModes, WR_GAMEMODE_COUNT))
            SetMode(idx + 1);
    }
    ImGui::SameLine();
    Marker("Which leaderboard this map is read on. Momentum keeps a separate one "
           "per gamemode, and nearly every map has a board in nearly every mode "
           "with nothing on it -- so asking the wrong one comes back empty and "
           "looks exactly like a map nobody has run.\n\n"
           "It is worked out for you: a remembered manual choice first, then "
           "the map-name convention, then an existing cache for ambiguous map "
           "families, and finally the fallback setting. Changing it here is "
           "remembered for this map.");

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

    // Which end of it. Two radio buttons rather than a tick box, because they
    // are two views of one board and not a modifier on one view.
    {
        int end = g_end;
        if (ImGui::RadioButton("fastest", end == END_FAST))
            end = END_FAST;
        ImGui::SameLine();
        if (ImGui::RadioButton("slowest", end == END_SLOW))
            end = END_SLOW;
        ImGui::SameLine();
        Marker("The slow end of the same leaderboard.\n\n"
               "Worth having because the fastest runs are the hardest to "
               "follow: a 37-second surf_demise world record is not a line "
               "anybody can trace, and the 79-second run at rank 9,108 is.\n\n"
               "It costs one request -- every page the server sends carries the "
               "size of the board, so reaching the end is not a walk down to "
               "it. Nothing down here is read automatically; the top of a leg "
               "is.");

        if (end != g_end)
        {
            g_end = end;
            g_showTop = g_quick.top;
            g_rowsStale = true;
            g_nextPollAt = 0;       // decide about the new end on the next tick
        }
    }

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
            //
            // On the slow end it is the ONLY way in: nothing is read down there
            // automatically, so the first visit always lands here.
            char lbl[64];
            if (g_end == END_SLOW)
            {
                _snprintf_s(lbl, sizeof(lbl), _TRUNCATE, "Read the slowest %d",
                            g_quick.top);
                if (ImGui::Button(lbl))
                    WantSlowest(legType, legNum, g_quick.top);
            }
            else
            {
                _snprintf_s(lbl, sizeof(lbl), _TRUNCATE, "Read the top %d",
                            g_quick.top);
                if (ImGui::Button(lbl))
                    WantBoard(legType, legNum, 1, g_quick.top);
            }
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
        // How much room to leave BELOW the table, counted rather than guessed.
        //
        // It was a flat 5.5 rows, which was right for what sat under the table
        // when it was written and silently wrong the moment anything was added.
        // By v0.9.0 there were eight rows down there -- show-more, the buttons,
        // a wrapped give-up reason, the status note, a separator and three rows
        // of colour controls -- so the last of them was pushed off the bottom of
        // the window and "scale to what is on" could not be clicked without
        // resizing. Reported as "load 20 more pushes the bottom down".
        //
        // A count of what is actually about to be drawn cannot drift the same
        // way: add a row below and this grows with it.
        const bool willShowMore = (g_showTop < WR_QUICK_TOP_MAX &&
                                   g_rowCount >= g_showTop &&
                                   (g_boardTotal <= 0 || g_boardTotal > g_rowCount));
        int failed = 0;
        for (int i = 0; i < g_pickCount; i++)
            if (g_picks[i].failed)
                failed++;

        float rows = 1.0f;                          // the buttons
        if (willShowMore)     rows += 1.0f;
        if (failed)           rows += 2.0f;         // a wrapped sentence
        if (g_note[0])        rows += 1.0f;
        if (g_extraOn > 0)    rows += 1.0f;
        rows += 0.6f;                               // the "Colour" separator
        rows += 4.0f;                    // two radio rows, scale and line height
        rows += 1.0f;                               // and DrawScanRow's button

        // Count every widget below the table, without exception. The last
        // report was "'load 20 more' pushes the bottom of the window down so
        // 'scale to what is on' is hard to click", and it was one uncounted
        // row doing it.

        float footer = ImGui::GetFrameHeightWithSpacing() * rows;
        // Never more than half the room. A count can still be beaten -- a very
        // narrow window wraps a sentence further than budgeted -- and the way
        // that must NOT fail is by leaving no table at all.
        const float avail = ImGui::GetContentRegionAvail().y;
        if (footer > avail * 0.5f)
            footer = avail * 0.5f;

        // "quickboard", NOT the "quickrows" this table was called until v0.9.2.
        //
        // ImGui keys a table's saved settings on its ID and its column count,
        // and imgui.ini files in the wild already carry a "Column 0 Sort=0v"
        // for the old name -- written while the table was not sortable at all.
        // Keeping the name would restore that on the first run of the sortable
        // version: the page would open sorted by the TICK column, descending,
        // which puts the runs you have not asked for at the top and reads as a
        // sort that does not work. A new ID starts from the DefaultSort below,
        // which is rank ascending, which is what a leaderboard looks like.
        if (ImGui::BeginTable("quickboard", 5,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_Sortable |
                              ImGuiTableFlags_SortMulti,
                              ImVec2(0.0f, -footer)))
        {
            // A single space, not "", for the two unnamed columns: an empty
            // header has no width to click and the whole point of making them
            // sortable is that they are the two most useful sorts here -- what
            // you ticked, and how far along it is.
            ImGui::TableSetupColumn(" ", ImGuiTableColumnFlags_WidthFixed, 26.0f,
                                    QCOL_TICK);
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed |
                                         ImGuiTableColumnFlags_DefaultSort,
                                    40.0f, QCOL_RANK);
            ImGui::TableSetupColumn("time", ImGuiTableColumnFlags_WidthFixed,
                                    78.0f, QCOL_TIME);
            ImGui::TableSetupColumn("runner", ImGuiTableColumnFlags_WidthStretch,
                                    0.0f, QCOL_RUNNER);
            ImGui::TableSetupColumn("  ", ImGuiTableColumnFlags_WidthFixed,
                                    130.0f, QCOL_STATE);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            if (g_orderCount != g_rowCount)
            {
                for (int i = 0; i < g_rowCount; i++)
                    g_order[i] = i;
                g_orderCount = g_rowCount;
                g_resort = true;
            }

            ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs();
            if (specs && specs->SpecsCount > 0 && g_rowCount > 1 &&
                (specs->SpecsDirty || g_resort))
            {
                g_specs = specs;
                qsort(g_order, (size_t)g_rowCount, sizeof(int), CompareQuickRows);
                g_specs = NULL;
                specs->SpecsDirty = false;
            }
            g_resort = false;

            for (int k = 0; k < g_rowCount; k++)
            {
                const int i = g_order[k];
                if (i < 0 || i >= g_rowCount)
                    continue;
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
                // RowName, so the column is sorted on the string it displays.
                ImGui::TextUnformatted(RowName(r));

                // ONE WORD, and the sentence on hover and underneath.
                //
                // It used to print the whole reason here and the column clipped
                // it -- "that demo could no", with no way to widen the column and
                // no way to reach the rest. A status column has to fit in a
                // status column; anything longer belongs somewhere it can wrap.
                ImGui::TableSetColumnIndex(4);
                if (pi >= 0 && g_picks[pi].failed)
                {
                    // The one failure worth a button. Everything else here is a
                    // property of the file and pressing again would put the same
                    // question to the same bytes; running out of time is a
                    // property of the attempt, and it is most of the failures
                    // there are. See WrQuickPick::canRetry.
                    if (g_picks[pi].canRetry)
                    {
                        if (ImGui::SmallButton("more time"))
                        {
                            WrQuickPick *p = &g_picks[pi];
                            p->failed = false;
                            p->extracted = false;
                            p->canRetry = false;
                            p->retried = true;
                            p->why[0] = '\0';
                            g_nextPollAt = 0;
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(
                                "%s\n\n"
                                "That is the clock, not the demo -- it would "
                                "read given longer. Pressing this reads it with "
                                "no time limit at all.\n\n"
                                "It can be a long wait. Most are seconds; a "
                                "forty-minute marathon run took an hour on this "
                                "machine, and it holds the one job slot while "
                                "it works, so the other panel's buttons wait "
                                "too. Stop, in the full panel, ends it.",
                                g_picks[pi].why);
                    }
                    else
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1.0f),
                                           "gave up");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", g_picks[pi].why[0]
                                              ? g_picks[pi].why
                                              : "no reason was recorded");
                    }
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
                    // not cached and this is where a request is owed. Which
                    // places depends on which way the table is going: forwards
                    // it is the next window down, and at the slow end it is a
                    // deeper tail, which comes back in one request for the whole
                    // of it rather than a walk backwards.
                    if (g_rowCount < g_showTop)
                    {
                        if (g_end == END_SLOW)
                            WantSlowest(legType, legNum, g_showTop);
                        else
                            WantBoard(legType, legNum, g_rowCount + 1,
                                      g_showTop - g_rowCount);
                    }
                }
                ImGui::SameLine();
                ImGui::TextDisabled("showing %d", g_rowCount);
                ImGui::SameLine();
                Marker("Adds the next twenty places at whichever end you are "
                       "looking at. If they are already cached -- from the Board "
                       "tab, or from pressing this before -- it costs no request "
                       "at all.\n\n"
                       "The full panel is the place to go past a hundred, or to "
                       "land somewhere in the middle of a board.");
            }
        }
    }

    // --- the buttons --------------------------------------------------------
    if (g_rowCount > 0)
    {
        // THE FIRST FIVE AS DISPLAYED, not the five fastest, and the label says
        // so. It used to be "Top 5" over an unsorted rank list, where the two
        // meant the same thing; with a sort and a slow end they do not, and a
        // button that quietly ignored both would be the one control on the page
        // that did not act on the table in front of it.
        if (ImGui::Button("Tick first 5"))
            for (int k = 0; k < 5 && k < g_rowCount; k++)
            {
                const int i = (k < g_orderCount) ? g_order[k] : k;
                if (i >= 0 && i < g_rowCount)
                    PickAdd(&g_rows[i], legType, legNum);
            }
        ImGui::SameLine();
        Marker("The first five rows as the table is currently sorted -- so at the "
               "slow end, sorted by rank, it is the five slowest runs on the "
               "board.");
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

    // Lines that are on which this page did not ask for.
    //
    // With picks, ApplyPicks makes the store agree exactly and this is zero. It
    // is reachable when there are none -- loading a map turns on the nearest
    // fastest run, which is the right first impression and worth keeping -- and
    // when runs have been ticked in the full panel's Runs tab, which this page
    // has no business overruling.
    //
    // Said out loud either way, because the alternative is what was reported:
    // two panels describing different pictures, and a line on screen with an
    // empty tick list here. This page is not the only thing that can draw, and
    // pretending otherwise is what made the disagreement look like a bug rather
    // than like two panels.
    {
        const int extra = g_extraOn;
        if (extra > 0)
        {
            ImGui::TextDisabled("%d line%s on that you did not tick here", extra,
                                extra == 1 ? " is" : "s are");
            ImGui::SameLine();
            if (ImGui::SmallButton("turn off"))
            {
                for (int i = 0; i < WrRunCount(); i++)
                {
                    WrRun *r = WrRunAt(i);
                    if (!r || !r->enabled)
                        continue;
                    bool picked = false;
                    for (int k = 0; k < g_pickCount && !picked; k++)
                        picked = WrRunIsFrom(r, g_picks[k].hash);
                    if (!picked)
                        r->enabled = false;
                }
                // Or the next frame's auto-enable puts one straight back.
                WrPathCancelAutoEnable();
                g_extraOn = 0;
                g_nextPollAt = 0;
            }
            ImGui::SameLine();
            Marker("Chosen for you when the map loaded -- the fastest run that "
                   "passes near where you are standing -- or ticked in the full "
                   "panel's Runs tab.\n\n"
                   "Nothing is wrong: this page is not the only thing that can "
                   "put a line on screen. It is said here so the two panels "
                   "cannot quietly describe different pictures.");
        }
    }

    // OUTSIDE the row test. What the page is doing is most worth saying when
    // there is nothing in the table, which is exactly the state the old
    // placement could not reach.
    if (g_note[0])
        ImGui::TextDisabled("%s", g_note);

    DrawColourRow();
    DrawScanRow();

    ImGui::End();

    if (!open)
        WrSetPanelOpen(WR_PANEL_QUICK, false);
}
