// wr_peek.h  --  remembering which map a demo is for, so we stop asking the disk.
//
// WHY THIS EXISTS
//
// A demo's map name is inside the file. There is no index, the filename is a
// replay hash, and the directory a demo sits in says nothing -- momtv\online\
// is keyed by map ID but momtv\local\ is not, and wrlines_data\demos\ is ours.
// So "which of my demos are for surf_utopia" has always been answered the only
// way it can be: open all of them and read the header.
//
// That is 6,416 files on this machine. Warm, it is about a second. COLD, it is
// twenty to thirty seconds of random small reads, and the extractor printed
// nothing for the whole of it -- which is why the first Extract of a session
// looked like a hang and every one after it looked instant. Two separate walks
// do it (CountInTree on every map change, CollectVisit on every extract), and a
// finished job starts the first one again, so pressing Extract shortly after
// loading a map ran two of them against one cold disk.
//
// The answer never changes unless the file does. So: remember it.
//
// WHY size AND mtime, AND WHY THAT IS ENOUGH
//
// The key is (path, size, last-write-time). A demo is written once by the game
// or by our own fetcher and then never touched, so in practice the only events
// are "created" and "deleted" -- but a re-download that produced a DIFFERENT
// file must not be believed, and it is the one case where the path repeats.
// Size catches a truncated or replaced download; mtime catches a same-size
// replacement. Both come out of the directory enumeration we are already doing,
// at no extra I/O -- which is the whole point, because a check that cost a file
// open would be the thing it is replacing.
//
// It is a CACHE, not a database. A miss is not an error, it is a peek; a
// corrupt or truncated line is dropped and re-peeked; deleting the file costs
// one slow run and nothing else. Nothing here is ever the only copy of
// anything.
//
// WHY NOT IN wr_mtv.cpp
//
// wr_mtv.cpp knows the container format and nothing about where files live or
// what a library is. This knows about paths, staleness and a file under
// wrlines_data, and knows nothing about MMTV beyond calling WrMtvPeek when it
// has to. Putting the cache next to the parser would give the parser a lock, a
// disk file and a threading rule for the benefit of two callers in a third
// module.
//
// STALE ROWS ARE FINE, AND ARE BOUNDED
//
// A deleted demo leaves a row nothing ever looks up again. Pruning it would
// mean stat-ing every row we did not touch, which is exactly the I/O this
// exists to avoid. Instead the table is capped: past WR_PEEK_MAX entries it
// stops inserting and says so once, and the run is merely as slow as it used to
// be. At ~150 bytes a row the file tops out around 3 MB, and a library that
// large has bigger problems than this file.
//
// THREADING
//
// CountThread and the extractor's coordinator can both walk at once -- a map
// change starts one while the other is mid-extract. Everything below is under
// one critical section, and the path to the file is resolved ONCE at load and
// copied immediately, because WrDataPath hands out one of four rotating static
// buffers with no lock of its own (see wr_log.cpp).

#ifndef WR_PEEK_H
#define WR_PEEK_H

#include "wr_common.h"

// Past this many demos the cache stops growing and the walk goes back to
// opening files. Chosen as roughly three times the largest library seen.
#define WR_PEEK_MAX 20000

// Which map this demo is for.
//
// Answers from the cache when the file has not changed since it was last
// looked at, and from WrMtvPeek when it has -- inserting the answer either way.
// `size` and `mtime` come straight out of the WIN32_FIND_DATA the caller
// already has; mtime is the FILETIME as a 64-bit integer.
//
// `mapOut` is set to "" when the file is not an MMTV at all. `ok` receives
// whether the header parsed cleanly, which is a different question: a demo
// whose tick interval is not a tick interval still names its map, and is still
// a demo FOR that map. Pass NULL if you do not care.
//
// Returns true if this call read the file rather than the cache, which is what
// a harness needs in order to tell a hit from a miss.
bool WrPeekMapOf(const char *path, long long size, long long mtime,
                 char *mapOut, int mapCap, bool *ok);

// Write the table back if anything changed. Called at the end of a walk; a
// no-op when nothing was inserted, so calling it after a fully cached walk
// costs nothing.
void WrPeekSave(void);

// How many rows are held, and how many of this session's lookups were answered
// without touching the disk. For the Diagnostics tab and for the harness.
int WrPeekCount(void);
void WrPeekStats(int *hits, int *misses);

// Drop everything, in memory and on disk. The "it is a cache" escape hatch.
void WrPeekForget(void);

void WrPeekShutdown(void);

#endif // WR_PEEK_H
