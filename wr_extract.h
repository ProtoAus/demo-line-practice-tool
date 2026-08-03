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

// Start counting demos for this map, on a background thread. Cheap to call.
void WrExtractOnMapChanged(const char *map);

// Results of that count. Returns false while it is still running.
bool WrExtractCounts(int *forThisMap, int *alreadyDone, int *notYetDone);

// Start the extractor for the current map. No-op if one is already running or no
// python could be found.
void WrExtractRun(void);
bool WrExtractRunning(void);

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
