// wr_fetch.h  --  downloading demos, which is the only thing here that costs
// somebody else bandwidth.
//
// Everything in wr_api.h about manners applies to this file too and is not
// repeated. What is different is what a request costs: a leaderboard page is a
// hundred kilobytes of JSON, and a demo is anything up to fifty megabytes. So
// the rules get one more clause -- WORK OUT WHAT IS MISSING BEFORE ASKING FOR
// ANYTHING -- and that is not politeness, it is the difference between "fetch
// the top fifty" costing one file and costing fifty.
//
// DEDUPE BY HASH, AND WHY IT IS EXACT RATHER THAN A GUESS
//
// The leaderboard hands back a `replayHash` per run, and that hash IS the .mtv
// filename the game itself stores. So "do I already have this run" is a
// filename lookup across the three trees a demo can be in, with no request, no
// header read and no ambiguity. Ask for the top fifty of a map you have
// forty-nine of and it downloads one file.
//
// The set is built once per fetch by walking those trees, because the
// alternative -- GetFileAttributes on three candidate paths per row -- is three
// syscalls per run and this is asked about every row of every board.
//
// WHY --into-game COPIES RATHER THAN MOVES
//
// A demo we downloaded lives in wrlines_data\demos\<map>\, which is ours. With
// --into-game it is ALSO placed in the game's own replay folder so the game can
// play it. A copy and not a move, because the game's folder is the game's: a
// cache clear, a verify, or the game deciding to tidy up takes anything in
// there with it, and the lines drawn from that demo would go with it. Ours is
// the copy that has to survive.
//
// WHY THE COPY CARRIES THE SOURCE'S WRITE TIME, WHICH IS THE SUBTLE ONE
//
// The reference uses shutil.copy2, and the 2 is load-bearing: it carries the
// modification time across, so the copy in the game's folder has a timestamp
// identical to ours down to the tick. CopyFileA does NOT do that on its own --
// it gives the destination a fresh write time -- so this does it explicitly
// with GetFileTime and SetFileTime.
//
// That timestamp is the DLL's proof of ownership. wr_intogame.h has to decide
// whether a file sitting in the game's folder is one of ours (removable) or one
// the game downloaded by itself (absolutely not), and "our tree holds a file
// with the same hash" does not answer it -- the game can download the same run
// afterwards, and then both are true. Identical size AND identical write time
// is an answer, because the game stamps its downloads when IT fetched them.
//
// If this breaks, nothing looks wrong. The download works, the demo plays, the
// lines appear. What stops working is "take out", quietly, for every file
// fetched after the day it broke. That is why the parity driver asserts the
// timestamp rather than trusting the copy.
//
// A DEMO THAT IS NOT A DEMO IS SKIPPED, NOT WRITTEN
//
// A downloadURL can answer with an error page, a redirect to a login, or an
// empty body, and all three are plausible bytes. Anything under 0x100 bytes or
// not beginning "MMTV" is refused before it reaches the disk, because a
// half-file named after a replay hash would then be counted as "we have that
// run" for ever by the dedupe above.

#ifndef WR_FETCH_H
#define WR_FETCH_H

#include "wr_common.h"
#include "wr_api.h"

// Places named on one command line, e.g. "5,9,120-140".
//
// The reference's parse_ranks, quirks included, because this parses a string a
// user typed and the two implementations must agree on what a typo means:
//
//   - a range is detected by a '-' at index 1 OR LATER, so "-5" is a single
//     negative number and not a malformed range. int("-5") succeeds there.
//   - a reversed range is swapped rather than dropped: "140-120" is the same
//     as "120-140".
//   - a range is capped at 4096 places FROM ITS START, so "1-999999" is
//     1..4097 and not an error. Silently, which is the reference's choice.
//   - anything that will not parse is skipped without a word.
//
// Returns how many were written, sorted ascending and deduplicated. `out` may
// be NULL to count first.
int WrFetchParseRanks(const char *spec, int *out, int maxOut);

// The same thing from a file, one place or range per line, '#' comments
// skipped. The panel does not need this -- it passes an int array straight
// through now -- but wrextract.exe does, because the reference has
// --ranks-file and the parity driver hands the same argv to both.
int WrFetchParseRanksFile(const char *path, int *out, int maxOut, char *err,
                          int errCap);

// Every .mtv basename on disk, lowercased, sorted and DEDUPLICATED, across the
// game's tree and ours. For a downloaded run that basename is a replay hash, so
// this IS the set of runs held.
//
// A set, not a list, and not a fixed-width one. Both were bugs and both were
// silent:
//
//   - the same name is in both trees whenever --into-game has been used, which
//     is exactly what this file does, so a list counts one demo twice. The
//     count is printed ("N demos on disk across every map"), so it is wrong in
//     front of the user as well as in the arithmetic.
//   - the names are not all replay hashes. A downloaded one is 40 hex
//     characters, but momtv\local\ holds the player's OWN recordings under
//     whatever the game called them, and on this machine those run to 64
//     characters. A 48-byte field collapsed four of six thousand into
//     duplicates of each other -- and the visible consequence of a false "we
//     already have that" is a demo that never downloads, with no message.
//
// So the whole stem is kept, however long it is.
struct WrFetchHeld
{
    char **hash;
    int n, cap;
};

void WrFetchHeldBuild(WrFetchHeld *h, const char *gameDir);
bool WrFetchHeldHas(const WrFetchHeld *h, const char *hash);
void WrFetchHeldFree(WrFetchHeld *h);

// shutil.copy2 followed by os.replace: copy, carry the source's write time
// across, then swap it into place atomically.
//
// Exposed rather than left static because that timestamp is the whole of the
// ownership test in wr_intogame.h, and a contract whose failure has no symptom
// has to be assertable. tests\test_fetch.exe backdates a source by a year and
// checks the copy landed with the same FILETIME to the tick -- which is the
// only way to tell a carried stamp from a fresh one that happens to fall in
// the same second.
bool WrFetchCopyWithTime(const char *from, const char *to, char *err, int errCap);

// --fetch.
struct WrFetchArgs
{
    const char *gameDir;
    const char *map;
    int mapId;
    int gamemode, trackType, trackNum;

    // Which runs. Exactly one of these decides:
    //   ranks    -- named places out of the CACHED board, costing no
    //               leaderboard request at all
    //   slowest  -- the last `count`
    //   window   -- `count` from `fromRank`
    const int *ranks;
    int rankCount;
    bool slowest;
    int fromRank, count, top;

    bool dryRun;                // list what would be downloaded, download none
    bool intoGame;              // also place each one where the game looks
};

int WrFetchRun(const WrFetchArgs *a, WrEmitFn emit,
               WrApiAbortFn abort, void *abortUser);

#endif // WR_FETCH_H
