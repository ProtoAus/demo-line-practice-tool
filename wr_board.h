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
// WHY THE READING IS SPLIT FROM THE FETCHING
//
// Same fence as wr_maps.h: wrpath_extract.py does the network and writes a
// tab-separated file, and this reads it. The DLL links no HTTP client, and
// every build checks that with dumpbin. The file is
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
int WrBoardParseFile(const char *path, WrBoardRow *out, int maxRows,
                     int *total, long long *fetched, int *mapId);

void WrBoardShutdown(void);

// The names, from Momentum's own Gamemode enum rather than guessed. The map
// index cannot supply them: Momentum gives nearly every map a leaderboard in
// nearly every mode -- all 546 surf maps in the local catalogue list twelve --
// and most of those boards are empty.
#define WR_GAMEMODE_COUNT 13
const char *WrGamemodeName(int mode);   // 1-based; "?" outside the range

#endif // WR_BOARD_H
