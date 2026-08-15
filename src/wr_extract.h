// wr_extract.h  --  run the extractor and the fetchers without leaving the game.
//
// The .wrpath files come from wrpath_extract.py, which until now meant alt-tab,
// find a terminal, remember the command. Worse, nothing told you a map had demos
// you had never processed -- you found out by seeing fewer lines than you
// expected.
//
// Both halves live in the DLL now. There is no wrpath_extract.py in the release
// zip, no interpreter to find, and no child process: pressing the button starts
// a worker pool inside the game, which is what wr_jobs.h is about and why it
// carries a longer essay than its two hundred lines would suggest.
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
// button, the same "something is running" disable. So there is one latch, and
// it is claimed BEFORE any work starts. That shape was built so a ported verb
// and an unported one could share this file while the port was in progress; it
// is still right, because every failure path has to end the run through the
// same EndRun or the panel is left with an error and no full stop.

#ifndef WR_EXTRACT_H
#define WR_EXTRACT_H

#include "wr_common.h"

// Bumped whenever anything that decides whether a demo can be extracted
// changes.
//
// Failures are recorded per map, in paths\<map>\_failed.txt, so that re-running
// a map costs seconds instead of minutes -- surf_colin_blaster_69000 has 66
// demos that all fail, and re-deriving that takes four and a half minutes every
// time. The record carries this number, and a record written by a different
// revision is ignored, so improving the extractor automatically retries
// everything it previously gave up on. Nobody has to remember to delete a file.
//
// It is also stamped into every .wrpath at offset 0xFC, which means bumping it
// does not only retry the recorded failures: it marks everything already
// written as out of date, so a fix that changes what gets extracted actually
// reaches the files that were extracted wrongly. Without that, the 75
// surf_colin_blaster_69000 runs produced under the old +-16384 world limit
// would have been skipped for ever as "already done", which is exactly what
// they looked like -- present, and mostly wrong.
//
// 3: records where the RUN starts, matched against the demo's own
//    effectiveStartVelocity. Every file written before this one has a zero
//    there, which reads as "unknown" and falls back to the DLL's own estimate.
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

    // Where the game is installed. "" means WrGameDir(), which is how the DLL
    // always leaves it: it works the answer out from the path of the running
    // executable, which inside the game is exactly right.
    //
    // It is NOT right for tests\wrextract.exe, whose running executable is
    // itself -- and finding that out cost a parity run. The port silently
    // walked a game tree that did not exist, found only the demos under
    // wrlines_data, and reported a clean extraction of the wrong set. A field
    // that is empty in the shipped path and filled in by the front end that
    // needs it is the difference between "the same code" and "the same code
    // looking somewhere else".
    char gameDir[MAX_PATH];

    char map[72];                   // "" means "the map you are standing in"
    int mapId;
    int gamemode, trackType, trackNum;

    // EXTRACT
    //
    // The panel only ever sets three of these. The rest exist because
    // tests\wrextract.exe drives the same function with the reference's own
    // flag surface, which is what lets tests\parity.ps1 run one argv against
    // both implementations -- and a request the console front end can express
    // but the panel cannot is not a second code path, it is the same one with
    // different numbers in it.
    bool retryFailed;
    bool skipExisting;              // the panel: always. --file: never.
    bool verify;                    // compute everything, write nothing
    int timeoutSeconds;             // 0 for no limit
    int jobs;                       // 0 decides; 1 is serial and comparable
    int limit;                      // 0 for all of them
    char file[MAX_PATH];            // one demo, whatever is known about it

    // BOARD
    WrBoardFetchMode boardMode;
    int fromRank, count, spread;

    // FETCH
    bool dryRun;                    // list what would be downloaded, download nothing
    bool intoGame;                  // also copy each demo where the game can play it
    int top;                        // fetch the top N (the Maps tab)
    const int *ranks;               // or exactly these places (the Board tab)
    int rankCount;                  // COPIED by WrExtractSubmit; may be a stack array

    // PRESENTATION, and the one field that exists for the PANEL rather than for
    // the work.
    //
    // Building the work list opens every .mtv in three trees, because the map
    // name lives inside the file and nothing else knows it. Cold, that is twenty
    // to thirty seconds on a real library, and until this field existed the
    // extractor printed NOTHING for the whole of it -- so the first press of
    // Extract in a session looked exactly like a hang, and the second looked
    // instant because the filesystem cache was warm. It was never slow at the
    // thing it says it is doing; it was silent at the thing it does first.
    //
    // OFF BY DEFAULT, AND THAT IS THE LOAD-BEARING PART. tests\parity.ps1
    // compares stdout CHARACTER FOR CHARACTER against tests\reference\
    // wrpath_extract.py, which prints nothing here and cannot be changed -- it is
    // frozen at revision 3 and is the oracle by definition. An unconditional
    // progress line would therefore fail the release gate for a reason that has
    // nothing to do with whether the port is correct.
    //
    // So: WrExtractRun sets it, because that is the panel's path and the panel
    // has a human watching it. tests\wrextract_main.cpp never does, because
    // `= {0}` leaves it false and the console front end is the one being
    // compared. Parity is safe by construction rather than by remembering.
    bool progressLines;
};

// Start a job. Returns false, having done nothing, if one is already running.
//
// It used to return void, and "already running" was therefore SILENT. That was
// fine while every caller was a button that had just disabled itself, and it is
// not fine now that one caller is a state machine: the quick menu advances a
// chain of jobs across frames and shares this one slot with every button in the
// full panel, so "did my submit take" is a question it has to be able to ask.
// Guessing from WrExtractRunning() beforehand cannot answer it -- the slot can
// go between the test and the call -- and the answer has to come from the latch
// itself.
//
// `ranks` is copied, so the caller can pass a stack array or free its own
// straight after. There is deliberately no cap: the ticked-places count is
// bounded only by how big a board is, and the old 64-pick limit existed solely
// because the selection travelled on a command line.
bool WrExtractSubmit(const WrExtractRequest *req);

// The same work, synchronously, on the calling thread: no latch, no thread, no
// panel, and the exit code as a return value.
//
// This is the seam that makes the port checkable from outside the game.
// WrExtractSubmit is this plus the one slot; tests\wrextract.exe is this plus
// stdout, which is how one argv can be run against both implementations and
// their output diffed line for line. A console front end with its own copy of
// the dispatch would agree with itself and say nothing about what ships.
int WrExtractRunRequest(const WrExtractRequest *req);

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

// What became of the most recent SINGLE-DEMO extraction -- the --file path, and
// the one the quick menu uses for every ticked run.
//
// It exists because "that demo could not be read" is three different sentences
// wearing one coat, and only one of them is worth a second press. A demo whose
// coordinate stream cannot be found will never be readable; a demo that ran out
// of time in the chain search will be readable given more of it, and 403 of the
// 415 recorded failures on this machine are that second kind. A page that
// cannot tell them apart has to give the same dead end to both.
//
// `base` is the demo's basename without the extension, which is the replay hash
// and therefore what the caller ticked. Returns false when nothing single-demo
// has run yet, or when the last one succeeded -- a success needs no reason.
//
// `timedOut` is derived from the message rather than carried as a flag, and
// the coupling is deliberate but narrow: the one place that sentence is written
// is Timeout() in wr_dp.cpp, which spells it that way because _failed.txt
// records written by the REFERENCE spell it that way. Threading a bool up
// through WrDpExtract and WrDemoProcess would have touched the parity path to
// learn something the parity path already states.
bool WrExtractLastFileFailure(char *base, int baseCap, char *why, int whyCap,
                              bool *timedOut);

// Seconds to allow one demo before giving up on it, or 0 for no limit.
//
// 30 rather than the extractor's old 180: measured across 4388 demos, the
// median is 58 KB and extracts in about a second, while the 6.5% over 700 KB
// are what actually hit the limit. Three minutes of silence per bad demo read
// as a hang.
#define WR_EXTRACT_TIMEOUT_DEFAULT 30

// ...and a different number for ONE demo somebody asked for by name.
//
// The 30 above is a good number for a batch and a bad one for a tick, and the
// failure records say so out loud. Of the 415 recorded failures on this
// machine, 403 are this timeout -- not corrupt demos, not unreadable ones,
// demos that ran out of time. Their median size is 1.1 MB and only 12 of the
// 403 are under the 700 KB the 30 was reasoned about.
//
// The reason the two populations differ is not subtle. The library that 58 KB
// median came from is mostly your own local demos. The quick menu on Delete
// downloads from a LEADERBOARD, and a leaderboard is nothing but complete runs
// of whole maps: median 186 KB where the body is LZMA and 3,082 KB where it is
// zstd. The old number was never wrong about the library. It was wrong about
// downloads, and downloads are what the front door does.
//
// So a tick gets four times as long, and the difference is justified by what
// the two jobs are rather than by taste. A tick is one demo, explicitly asked
// for, on a background thread, with a Stop button and a page that says what it
// is doing. A whole-map extraction is thousands of demos with a human watching,
// where four times as long to fail is four times the wait for every bad one.
#define WR_EXTRACT_TIMEOUT_TICK 120
void WrExtractSetTimeout(int seconds);
int WrExtractTimeout(void);

// The same number, addressable, so wr_settings.cpp can put it in settings.cfg
// beside everything else the panel remembers. It had a slider and no entry in
// that table, so raising it lasted until you quit -- the same defect
// line.autoScale had, found the same way.
//
// A pointer rather than another setter, which is what WrScanFrozenLimit does
// for the same reason: the settings table binds to storage.
int *WrExtractTimeoutPtr(void);

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

// One line for under the buttons when nothing has run yet. It used to name the
// Python interpreter that had been found, or say why none had been; there is
// nothing left to look for.
const char *WrExtractStatus(void);

// ---------------------------------------------------------------------------
// Walking the demos
// ---------------------------------------------------------------------------

// Every .mtv the extractor would consider, in the reference's iter_demos order:
// the game's momtv\online tree, then momtv\local, then the demos --fetch put
// under wrlines_data. Within a directory its FILES come first and its
// subdirectories after, which is os.walk's order and not the obvious one.
//
// Exposed for tests\wrextract.exe's --dump-info, which selects the same demos
// by a different rule. One walker, because two would be two chances to disagree
// about which demos a parity run actually covered.
typedef void (*WrDemoWalkFn)(void *user, const char *path, long long size);
void WrExtractWalkDemos(const char *gameDir, WrDemoWalkFn visit, void *user);

// There is no WrExtractShutdown. It was an empty body with no call sites, and
// the move in-process is exactly when it looked like it should acquire one --
// see the note in the .cpp for why the answer is still nothing, and why a
// function with a body and no reachable caller would be worse than its absence.

#endif // WR_EXTRACT_H
