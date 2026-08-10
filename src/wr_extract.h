// wr_extract.h  --  run the extractor and the fetchers without leaving the game.
//
// The .wrpath files come from wrpath_extract.py, which until now meant alt-tab,
// find a terminal, remember the command. Worse, nothing told you a map had demos
// you had never processed -- you found out by seeing fewer lines than you
// expected.
//
// Two halves:
//
//   COUNTING is done here, in C, and is exact rather than a guess. Two facts
//   make that cheap. The extractor names its output after the source demo's
//   basename (process_one calls the variable sha1, but it is
//   os.path.splitext(name)[0]), and everything a .mtv says about itself sits in
//   its first 512 bytes. So "is this demo for this map, can it be read at all,
//   and has it already been done" is one 512-byte read and one file-exists
//   check -- no decompression. Four thousand demos on a background thread.
//
//   RUNNING is one slot: one job at a time, one line ring, one Stop button, for
//   all four verbs. See below.
//
// Never automatic. Any of these would compete with the game for CPU while the
// user plays, or reach the network. They happen when a button is pressed and not
// before.
//
// WHY THE REQUEST IS A STRUCT
//
// It used to be a command line. Every caller formatted a fragment of argv with
// _snprintf_s and this file glued the fragments together, which had three costs
// that a struct removes outright:
//
//   - A selection of leaderboard places had to travel through a FILE, because a
//     command line is 2048 bytes and "tick all" is not bounded by anything. That
//     file existed only to work around the transport.
//   - A map name is user-typed free text going into "%s" inside a command line.
//     Nobody is going to type a quote into a map name, but nothing stopped them.
//   - "--track-type" mistyped as "--tracktype" is a silent no-op at runtime.
//     req.trackType is a compile error.
//
// WHY ONE SLOT AND ONE LATCH
//
// Extraction, the map index, a board fetch and a demo fetch are four different
// jobs, but to the panel they are one: the same output pane, the same Stop
// button, the same "something is running" disable. So there is one latch, and it
// is claimed BEFORE the backend is chosen. That is what lets a job that has been
// ported to C and a job that is still a python child share this file while the
// port is in progress -- see NativeHandles() in the .cpp, which is the only
// place that knows how far the port has got.

#ifndef WR_EXTRACT_H
#define WR_EXTRACT_H

#include "wr_common.h"

// Must match EXTRACTOR_REVISION in wrpath_extract.py. A recorded failure from a
// different revision is ignored here, which reports the demo as unprocessed --
// the safe direction, since the worst case is offering to redo work.
#define WR_EXTRACTOR_REVISION 3

// ---------------------------------------------------------------------------
// Counting
// ---------------------------------------------------------------------------

// Start counting demos for this map, on a background thread. Cheap to call.
void WrExtractOnMapChanged(const char *map);

// Results of that count. Returns false while it is still running.
//
// knownBad is demos that cannot be extracted, and it has two sources. Most are
// demos the extractor has already tried and given up on, recorded in
// paths\<map>\_failed.txt. They are counted separately from notYetDone on
// purpose: surf_colin_blaster_69000 has 66 of them, they take four and a half
// minutes to fail again, and without this the panel offered "66 new" forever
// and each press of the button spent those minutes reaching the same answer.
//
// The rest are demos whose header does not survive WrMtvPeek's gates. Those
// need no record and no previous attempt: a file whose tick interval is not a
// tick interval has had its layout moved under it, every field read out of it
// is a number from the wrong place, and no amount of re-running changes that.
// Counting them as work waiting to be done was a small lie the counter could
// not previously avoid telling, because it only read one field.
bool WrExtractCounts(int *forThisMap, int *alreadyDone, int *notYetDone,
                     int *knownBad);

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

enum WrJobKind
{
    WR_JOB_NONE = 0,
    WR_JOB_EXTRACT,         // demos on disk -> .wrpath
    WR_JOB_INDEX_MAPS,      // the game's own map cache -> maps.txt
    WR_JOB_BOARD,           // a window of a leaderboard -> boards\*.tsv
    WR_JOB_FETCH            // leaderboard rows -> .mtv files on disk
};

// The four board fetches were four command lines built in four places. They are
// four values of one field: the parameters are the same, only the window
// differs.
enum WrBoardFetchMode
{
    WR_BOARD_WINDOW = 0,    // fromRank .. fromRank+count
    WR_BOARD_SLOWEST,       // the last `count`
    WR_BOARD_SPREAD,        // `spread` single rows, evenly spaced
    WR_BOARD_FRIENDS        // whoever is on your Steam friends list
};

// One request, flat, with `= {0}` at every call site. Not a union: 150 bytes of
// mostly-unused fields is cheaper to read than a tag you have to check before
// you know which arm is live, and this is filled in nine places and read in one.
struct WrExtractRequest
{
    WrJobKind kind;

    char map[72];                   // "" means "the map you are standing in"
    int mapId;
    int gamemode, trackType, trackNum;

    // EXTRACT
    bool retryFailed;
    int timeoutSeconds;             // 0 for no limit

    // BOARD
    WrBoardFetchMode boardMode;
    int fromRank, count, spread;

    // FETCH
    bool dryRun;                    // list what would be downloaded, download nothing
    bool intoGame;                  // also copy each demo where the game can play it
    int top;                        // fetch the top N (the Maps tab)
    const int *ranks;               // or exactly these places (the Board tab)
    int rankCount;                  // COPIED by WrExtractSubmit; may be a stack array
};

// Start a job. No-op if one is already running.
//
// `ranks` is copied, so the caller can pass a stack array or free its own
// straight after. There is deliberately no cap: the ticked-places count is
// bounded only by how big a board is, and the old 64-pick limit existed solely
// because the selection travelled on a command line.
void WrExtractSubmit(const WrExtractRequest *req);

// Extraction for the current map -- the two buttons on the Runs tab. retryFailed
// is the only way to make it reconsider the recorded failures.
void WrExtractRun(bool retryFailed);

bool WrExtractRunning(void);

// Stop whatever is running.
//
// Safe at any time; does nothing when nothing is running. Completed .wrpath
// files always survive -- every write is a temp file plus an atomic replace --
// and the failure record is flushed as failures happen rather than at the end,
// so a stop no longer throws away the expensive part of what was learned.
void WrExtractStop(void);

// Which job most recently ENDED, and a counter bumped once per ending.
//
// The panel used to derive "a board fetch finished" from WrExtractRunning()
// going false, which has two holes: a job that starts and finishes inside one
// frame is never observed, and an EXTRACTION finishing reloaded the board for no
// reason. A counter cannot miss an edge, and a kind says whose edge it was.
unsigned int WrExtractRunGeneration(void);
WrJobKind WrExtractLastKind(void);

// Set once a run finishes so the caller can reload the run store; reading it
// clears it.
bool WrExtractTakeFinished(void);

// Seconds to allow one demo before giving up on it, or 0 for no limit.
//
// 30 rather than the extractor's old 180: measured across 4388 demos, the
// median is 58 KB and extracts in about a second, while the 6.5% over 700 KB
// are what actually hit the limit. Three minutes of silence per bad demo read
// as a hang.
#define WR_EXTRACT_TIMEOUT_DEFAULT 30
void WrExtractSetTimeout(int seconds);
int WrExtractTimeout(void);

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

// Live output from the running job, oldest first.
int WrExtractLineCount(void);
const char *WrExtractLine(int i);

// Where progress lines go. NULL is the panel ring, which is the default and what
// the DLL uses.
//
// The indirection is three lines and it buys the only thing that makes this code
// checkable: tests\wrextract.exe points it at stdout and runs the same functions
// the DLL runs, so its output can be diffed against wrpath_extract.py's line for
// line. Without it, "does the port print the same thing" has no answer.
typedef void (*WrEmitFn)(const char *line);
void WrExtractSetEmit(WrEmitFn fn);

// What we know about the backend -- which interpreter was found, or why none
// was. Shown under the buttons when nothing has run yet.
const char *WrExtractStatus(void);

void WrExtractShutdown(void);

// Build the argv the python backend would be launched with, for
// tests\test_seam.exe.
//
// Here rather than in the harness because the point is to prove that a typed
// request produces EXACTLY the command line the call sites used to format by
// hand. A harness with its own copy of the formatting would agree with itself
// and say nothing about what ships.
//
// Writes only the trailing flags -- the part that used to be the `extraArgs`
// argument -- because everything before it (interpreter path, script path, game
// directory) varies per machine and was never in doubt. Returns false if the
// request cannot be expressed, which after the port is complete will be every
// request; this function and the backend behind it go at the same time.
bool WrExtractTestPythonArgs(const WrExtractRequest *req, char *out, int cap,
                             bool *needsMap);

#endif // WR_EXTRACT_H
