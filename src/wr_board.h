// wr_board.h  --  a map's leaderboard, as much of it as you have asked for.
//
// WHY THIS IS A WINDOW AND NOT THE WHOLE THING
//
// Momentum's API caps a page at 100 entries -- take=200 is a 400 Bad Request --
// so a board is paged. surf_demise is 9108 runs, which is 92 requests;
// surf_boreas is 16993, which is 170. That is a minute or more of sustained
// requests per map, per refresh, against infrastructure somebody else pays for,
// and nobody needs seventeen thousand rows on screen.
//
// So the cache is whatever windows you asked for, and it ACCUMULATES. Fetch the
// top hundred, then the slowest hundred, then ranks 4000-4020, and this shows
// all three with the gaps between them visible. You browse as much of the board
// as you actually looked at and never pay for the rest.
//
// WHY YOU WOULD WANT THE SLOW END AT ALL
//
// Because the fastest runs are the hardest to follow. A 37-second surf_demise
// world record is not a line a learner can trace; the 79-second run at rank
// 9108 is. Reaching it costs two requests -- totalCount comes back with every
// page, so one request gets the size and the next lands on the tail.
//
// WHY THE FILE SURVIVED THE DLL LEARNING TO FETCH
//
// Until v0.6.0 the split was forced: wrpath_extract.py did the network and
// wrote a tab-separated file, and this read it, because the DLL linked no HTTP
// client and every build checked that with dumpbin. That is over. The fetching
// is in src\wr_api.cpp now, WINHTTP.dll is the sixth name in the import list,
// and the claim it replaced is written out in wr_http.h -- one file, one host,
// no identifier, never on a timer.
//
// The .tsv did not go with it, and the reason is worth writing down because
// "one process writes and reads this, so why is there a file" is the obvious
// question. Three answers:
//
//   - It ACCUMULATES ACROSS SESSIONS. That is the whole design of the Board tab
//     (see above): you pay for the windows you look at, and they are still
//     there next week. An in-memory table would make every restart cost the
//     requests again.
//   - It is the offline artefact. With no network, no account and the game shut
//     down, wrlines_data\boards\*.tsv is still a readable record of what you
//     browsed, in a format you can open in anything.
//   - It is the only thing that makes the fetcher checkable. tests\parity.ps1
//     compares this file byte for byte against what the reference wrote from
//     the same recorded responses; if the rows only existed in memory there
//     would be nothing to compare.
//
// So the fence moved rather than vanished. wr_api.cpp writes the file and
// everything below this comment reads it, and the two halves live in the same
// .cpp so the format has exactly one writer and one reader and they cannot
// drift. The path is
// wrlines_data\boards\<map>_g<gamemode>_t<tracktype><tracknum>.tsv.

#ifndef WR_BOARD_H
#define WR_BOARD_H

#include "wr_common.h"

// 20 000 rows is 2.6 MB and more of one leaderboard than anyone will page
// through. A board bigger than this is truncated, and WrBoardStatus says so
// rather than quietly stopping.
#define WR_BOARD_MAX 20000

struct WrBoardRow
{
    int rank;
    float time;                 // seconds
    unsigned long long steamId;
    char alias[64];             // free text from the server; may be any script
    char hash[48];              // the replay hash, which IS the .mtv filename
    long long dateEpoch;        // when the run was set
    char url[112];              // the download URL the SERVER gave us
    bool have;                  // resolved here, never stored -- see below
};

// Read a board cache. Background thread, single-flight, cheap to call.
//
// `have` is worked out on this side rather than read from the file, so it can
// never be stale: the three demo trees are walked once into a sorted hash list
// and each row is a binary search against it. A file that recorded it would go
// wrong the moment you downloaded anything.
void WrBoardLoad(const char *map, int gamemode, int trackType, int trackNum);
void WrBoardRefresh(void);      // re-read whatever was last asked for
bool WrBoardReady(void);
bool WrBoardBusy(void);
const char *WrBoardStatus(void);

// What the cache says about itself, from the file's own header lines.
int WrBoardTotal(void);         // runs on the server's board, 0 if unknown
long long WrBoardFetched(void); // unix seconds of the last fetch, 0 if unknown
const char *WrBoardMap(void);
int WrBoardMapId(void);

int WrBoardCount(void);
const WrBoardRow *WrBoardAt(int index);

// Parse one file directly, for the test harness. Returns rows parsed, or -1 if
// the file will not open. Does not resolve `have`; that needs the demo trees.
// Any of the three out-params may be NULL.
//
// A thin adapter over WrBoardReadCache below, not a second parser. It applies
// the two rules that belong to DISPLAY and not to the format -- a row with no
// rank or no hash is not shown, and the strings are cut to what the table can
// hold -- and nothing else.
int WrBoardParseFile(const char *path, WrBoardRow *out, int maxRows,
                     int *total, long long *fetched, int *mapId);

// ---------------------------------------------------------------------------
// The cache file itself
// ---------------------------------------------------------------------------
//
// The reader and the writer, together, because a format with one of each forty
// lines apart cannot drift and a format with them in two files always does.
//
// WHY THERE IS A SECOND ROW TYPE
//
// WrBoardRow above is what the TABLE needs; this is what the FILE holds, and
// the differences are all in the direction of not losing anything:
//
//   - `time` is a double. The API sends a double and the file records it to
//     six decimal places; a float has about seven significant digits, so a
//     4-figure time loses the sixth place on the way through. That is invisible
//     in a table and fatal to a byte-for-byte comparison with the reference.
//   - alias, url and steamid are held at their full length rather than at the
//     width the table draws. A truncated alias written back would silently
//     rewrite somebody's name in the file; a truncated URL would break the
//     download it exists for.
//
// WHY IT IS AN ARRAY AND NOT A MAP, AND WHY ORDER IS LOAD-BEARING
//
// The reference keeps the rows in a dict keyed by lowercased replay hash and
// writes `sorted(rows.values(), key=rank)`. Python dicts iterate in insertion
// order and sorted() is stable, so two rows with the SAME RANK come out in the
// order they were first seen -- file order, then the order the API returned
// them -- and a row that is re-fetched keeps its original position while taking
// the new value. qsort is not stable and would put those two rows either way
// round, which is a one-line difference in a two-hundred-row file and an
// afternoon to find. So: an array in insertion order, and the sort breaks a
// rank tie by insertion index.
struct WrBoardCacheRow
{
    int rank;
    double time;
    char steamId[24];
    char alias[192];            // free text, UTF-8, already through _clean
    char hash[48];
    long long epoch;
    char url[320];
};

// The header lines, held as TEXT.
//
// Not parsed into an int and reprinted, because the reference does not parse
// them either: read_board puts the tab-separated remainder of each "# key" line
// into a list and write_board joins it back with tabs. A value it does not
// understand survives a round trip unchanged, and so does one this does not.
// Empty means the line was absent, which is different from present and zero.
//
// `track` holds both of its fields with the tab still in it, for the same
// reason.
struct WrBoardCache
{
    WrBoardCacheRow *rows;
    int count, cap;
    bool truncated;             // hit maxRows; the file held more

    char map[72];
    char mapId[24];
    char gamemode[16];
    char track[40];
    char total[32];
    char fetched[32];
};

// wrlines_data\boards\<map>_g<gamemode>_t<tracktype><tracknum>.tsv.
// One place spells this, because the fetcher and the reader must agree on it.
void WrBoardCachePath(char *out, int cap, const char *map, int gamemode,
                      int trackType, int trackNum);

// Read one for merging. False when there is no file, which is not an error --
// it is the ordinary state of a board nobody has fetched yet, and leaves an
// empty cache the caller can add to. `maxRows` of 0 means no limit but memory.
bool WrBoardReadCache(const char *path, WrBoardCache *c, int maxRows);

// Add a row, or replace the one with the same hash IN PLACE. True if it was
// new. This is `held[hash.lower()] = row`, insertion order and all.
bool WrBoardCacheMerge(WrBoardCache *c, const WrBoardCacheRow *r);

// Where a hash already sits, or -1. The hash is folded ASCII-lowercase, which
// is the whole of str.lower() on a hex string.
int WrBoardCacheFindRow(const WrBoardCache *c, const char *hash);

// Rank-sorted, tab separated, CRLF, via a temp file and an atomic replace.
// Creates the directory. False having logged if anything went wrong.
bool WrBoardWriteCache(const char *path, const WrBoardCache *c);

void WrBoardCacheFree(WrBoardCache *c);

void WrBoardShutdown(void);

// The names, from Momentum's own Gamemode enum rather than guessed. The map
// index cannot supply them: Momentum gives nearly every map a leaderboard in
// nearly every mode -- all 546 surf maps in the local catalogue list twelve --
// and most of those boards are empty.
#define WR_GAMEMODE_COUNT 13
const char *WrGamemodeName(int mode);   // 1-based; "?" outside the range

#endif // WR_BOARD_H
