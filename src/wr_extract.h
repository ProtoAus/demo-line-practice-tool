// wr_extract.h  --  run the offline extractor without leaving the game.
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
//   os.path.splitext(name)[0]), and a demo's map name sits at offset 0x10 of the
//   MMTV header, inside the first 0x50 bytes. So "is this demo for this map, and
//   has it already been done" is one 80-byte read and one file-exists check --
//   no decompression, no parsing. Four thousand demos on a background thread.
//
//   RUNNING is a plain CreateProcess on the python script, with stdout on a pipe
//   and a reader thread feeding the panel. Python is started with -u, because
//   otherwise its output is block-buffered when it is not talking to a terminal
//   and the panel would sit empty for a minute and then print everything at once.
//
// Never automatic. Spawning a python process off a map change would launch a
// program behind the user's back and then compete with the game for CPU while
// they play. It happens when the button is pressed and not before.

#ifndef WR_EXTRACT_H
#define WR_EXTRACT_H

#include "wr_common.h"

// Must match EXTRACTOR_REVISION in wrpath_extract.py. A recorded failure from a
// different revision is ignored here, which reports the demo as unprocessed --
// the safe direction, since the worst case is offering to redo work.
#define WR_EXTRACTOR_REVISION 3

// Start counting demos for this map, on a background thread. Cheap to call.
void WrExtractOnMapChanged(const char *map);

// Results of that count. Returns false while it is still running.
//
// knownBad is demos the extractor has already tried and given up on, recorded
// in paths\<map>\_failed.txt. They are counted separately from notYetDone on
// purpose: surf_colin_blaster_69000 has 66 of them, they take four and a half
// minutes to fail again, and without this the panel offered "66 new" forever
// and each press of the button spent those minutes reaching the same answer.
bool WrExtractCounts(int *forThisMap, int *alreadyDone, int *notYetDone,
                     int *knownBad);

// Start the extractor for the current map. No-op if one is already running or no
// python could be found. retryFailed passes --retry-failed, which is the only
// way to make it reconsider the recorded failures.
void WrExtractRun(bool retryFailed);
bool WrExtractRunning(void);

// Stop whatever is running. Kills the interpreter AND its worker pool, which is
// why the child is put in a kill-on-close job object at launch: the pool is
// cores-minus-two grandchildren we hold no handles for, and killing only the
// parent leaves them burning a core each.
//
// Safe at any time; does nothing when nothing is running. Completed .wrpath
// files always survive -- every write is a temp file plus an atomic replace --
// and the failure record is flushed as failures happen rather than at the end,
// so a stop no longer throws away the expensive part of what was learned.
void WrExtractStop(void);

// Seconds to allow one demo before giving up on it, or 0 for no limit. Passed
// to the extractor as --timeout.
//
// 30 rather than the extractor's old 180: measured across 4388 demos, the
// median is 58 KB and extracts in about a second, while the 6.5% over 700 KB
// are what actually hit the limit. Three minutes of silence per bad demo read
// as a hang.
#define WR_EXTRACT_TIMEOUT_DEFAULT 30
void WrExtractSetTimeout(int seconds);
int WrExtractTimeout(void);

// The same launcher with different flags, for the map index and for fetching.
// `needsMap` adds --map for the map you are standing in; indexing does not want
// it. One process at a time, same as extraction, and never automatic.
void WrExtractRunArgs(const char *extraArgs, bool needsMap);

// Which interpreter we found, or why we could not find one.
const char *WrExtractInterpreter(void);

// Live output from the running script, oldest first.
int WrExtractLineCount(void);
const char *WrExtractLine(int i);

// Set once a run finishes so the caller can reload the run store; reading it
// clears it.
bool WrExtractTakeFinished(void);

void WrExtractShutdown(void);

#endif // WR_EXTRACT_H
