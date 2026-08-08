// wr_intogame.h  --  put one specific demo where the game's replay viewer can
// find it, and be able to take it out again.
//
// WHY THIS EXISTS
//
// Momentum's in-game demo viewer lists ten local demos. It finds them by
// scanning momentum\momtv\online\<mapID>\ -- there is no index file next to them
// -- so which ten it lists is decided entirely by which files are there. That
// makes the cap something you can work around by choosing rather than a wall:
// put the run you want to watch in, take the others out.
//
// The --into-game flag has been able to write that directory since 0.2, but only
// as "and also copy everything I fetch". This is the same act, one demo at a
// time, from a button next to the run.
//
// WHY THERE IS A MANIFEST
//
// Because the alternative is a delete button that guesses. That directory is the
// game's, not ours: it holds replays the game downloaded by itself, and on this
// machine 4268 of them. A "clear" that removed *.mtv would take all of it.
//
// So every file this puts there is recorded in wrlines_data\into_game.txt first,
// and removal only ever touches paths that are in that file AND still match the
// exact shape this writes. A demo the game downloaded is not in the manifest and
// cannot be reached by anything here. The record is written BEFORE the copy, so
// a crash between the two leaves a removable file rather than an orphan.
//
// WHAT THIS IS NOT
//
// It does not make the game play anything. No console command, no cvar, no call
// into the game at all -- it copies a file and writes a text file beside the
// DLL, and that is the whole of it. Playing the demo is still done from the
// game's own menu.

#ifndef WR_INTOGAME_H
#define WR_INTOGAME_H

#include "wr_common.h"

// Where a demo would go, and whether it is already there. `mapId` is Momentum's
// numeric id, which is what names the directory -- without it there is nothing
// to write to, and both return false.
bool WrIntoGamePath(int mapId, const char *hash, char *out, int outLen);
bool WrIntoGameHasFile(int mapId, const char *hash);

// Find the .mtv for this replay hash among the places one can be: our own
// downloads, the game's online tree, the game's local tree. False if none of
// them has it, which is the normal answer for a board row you have not fetched.
bool WrIntoGameFindSource(const char *map, int mapId, const char *hash,
                          char *out, int outLen);

// Copy it in and record that we did. Already-present is success and not a copy.
bool WrIntoGameSend(const char *map, int mapId, const char *hash);

// Everything we have put there, across every map. Refresh drops entries whose
// file has since gone -- a game cache clear, or a hand deletion -- so the count
// on screen is what is actually on disk.
void WrIntoGameRefresh(void);
int WrIntoGameCount(void);
bool WrIntoGameMine(int mapId, const char *hash);

// Remove only what the manifest says we put there. Returns how many files went.
int WrIntoGameRemoveAll(void);
int WrIntoGameRemoveOne(int mapId, const char *hash);

// The last thing that happened, for the panel. Empty when nothing has.
const char *WrIntoGameStatus(void);

void WrIntoGameShutdown(void);

#endif // WR_INTOGAME_H
