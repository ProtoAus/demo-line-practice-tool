// wr_intogame.h  --  put one specific demo where the game's replay viewer can
// find it, and be able to take it out again.
//
// WHY THIS EXISTS
//
// The game keeps replays in two trees, and the leaderboard panel has a tab for
// each -- "local replays" and "downloaded replays", by the names in
// momentum\resource\momentum_english.txt:
//
//   momentum\momtv\online\<numeric map id>\<40-hex replay hash>.mtv
//   momentum\momtv\local\<map name>\<user id>-<map>-<unix time>-<track>-<style>-<time>.mtv
//
// On the machine this was written on those hold 1,675 and 2,591 files across 290
// and 447 directories. Every single one of the 2,591 local files follows that
// name exactly, which is what a directory listing looks like when the names have
// to carry the metadata; and engine.dll contains the literals "momtv/local",
// "momtv/online" and a "momtv/local/*%s" glob.
//
// WHAT IS AND IS NOT KNOWN
//
// This code writes the ONLINE tree, which is where the game puts what it
// downloads. Whether the Downloaded tab lists a file that appears there without
// the game having downloaded it is NOT known and cannot be checked from this
// side. It is worth being plain about the doubt: a local replay has no server
// record of any kind, so the Local tab must enumerate from disk, while the
// Downloaded tab may well be listing the game's own record of what it fetched --
// in which case a file dropped into the online tree is present (the send button
// will say "already has that one") and never listed.
//
// That is why WrIntoGameSend takes a destination. The default is the online tree.
// The local tree is one deliberate button press per demo, because it is somebody
// else's run being put among your own recordings, and that should never happen
// by itself.
//
// The --into-game flag has been able to write the online directory since 0.2,
// but only as "and also copy everything I fetch" -- and, until now, without
// telling this file, so those copies were unremovable. See ADOPTION below.
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
// ADOPTION
//
// The Python's --into-game writes the identical destination and never told this
// file. Measured on the machine this was written on: into_game.txt held ZERO
// entries while seven of the sixteen files in momtv\online\104 had been put
// there by that path. The panel therefore said "none of ours", the "Remove ours"
// button never appeared at all, and pressing send answered "the game already has
// that one" -- every word of which was true and none of which was any use.
//
// So a file already at the destination is ADOPTED into the manifest, but only
// when wrlines_data\demos\<map>\<hash>.mtv exists as well. That file is the
// proof: it is there because we fetched it, and the hash IS the content, so the
// copy in the game's folder is a copy of ours. A demo the game downloaded by
// itself has no counterpart in our tree, is never adopted, and stays as
// untouchable as it was. The promise is unchanged; only the bookkeeping is
// fixed.
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

// Which of the game's two replay trees.
enum WrIntoGameWhere
{
    WR_INTO_ONLINE = 0,     // momtv\online\<mapId>\<hash>.mtv -- "downloaded"
    WR_INTO_LOCAL  = 1      // momtv\local\<map>\<hash>.mtv    -- "local"
};

// Where a demo would go, and whether it is already there. `mapId` is Momentum's
// numeric id, which is what names the online directory; the local tree is named
// by the map instead, so which argument matters depends on `where`.
bool WrIntoGamePathAt(WrIntoGameWhere where, const char *map, int mapId,
                      const char *hash, char *out, int outLen);
bool WrIntoGameHasFileAt(WrIntoGameWhere where, const char *map, int mapId,
                         const char *hash);

// Where this run's .mtv actually is, which is a different question from where it
// would go, and the one the Runs list needs: 202 of the 1,749 .wrpath files on
// this machine came from the player's OWN recordings in the game's local tree,
// and 35 have no .mtv anywhere at all because the demo was deleted in game and
// the path cache outlived it.
enum WrIntoGameSource
{
    WR_DEMO_NONE = 0,        // nowhere -- an orphaned .wrpath
    WR_DEMO_OURS,            // wrlines_data\demos\<map>\<hash>.mtv
    WR_DEMO_GAME_LOCAL,      // the game's local tree: one of your own recordings
    WR_DEMO_GAME_ONLINE      // already sitting at the online destination
};
WrIntoGameSource WrIntoGameSourceOf(const char *map, int mapId, const char *hash,
                                    char *out, int outLen);

// Kept for callers that only want a path. Equivalent to WrIntoGameSourceOf
// returning anything but WR_DEMO_NONE.
bool WrIntoGameFindSource(const char *map, int mapId, const char *hash,
                          char *out, int outLen);

// What a send did, so the row that asked can be told rather than the tab.
//
// It used to be one shared 192-byte string printed by both tabs, which is why
// two different runs' answers looked like one run contradicting itself.
enum WrIntoGameResult
{
    WR_SEND_OK = 0,         // copied, and recorded
    WR_SEND_ALREADY,        // already there; adopted if the file was ours
    WR_SEND_ALREADY_LOCAL,  // your own recording -- the game can already see it
    WR_SEND_NO_MAPID,       // no numeric id for this map
    WR_SEND_BAD_NAME,       // the source .mtv is not named like a replay hash
    WR_SEND_NO_SOURCE,      // no .mtv on disk anywhere
    WR_SEND_FULL,           // the manifest is at its cap
    WR_SEND_FAILED          // the copy itself failed
};

// Copy it in and record that we did. `detail`, when given, is filled with a
// sentence for that row -- including the absolute path on success, because
// checking in Explorer is the only way to see this feature work.
WrIntoGameResult WrIntoGameSendTo(WrIntoGameWhere where, const char *map,
                                  int mapId, const char *hash,
                                  char *detail, int detailLen);

// Everything we have put there, across every map. Refresh drops entries whose
// file has since gone -- a game cache clear, or a hand deletion -- so the count
// on screen is what is actually on disk. Pass the map you are standing in and
// its id and it will also ADOPT anything of ours already at the destination (see
// the header comment); pass NULL/0 for the prune alone.
void WrIntoGameRefresh(const char *map, int mapId);
int WrIntoGameCount(void);
bool WrIntoGameMine(int mapId, const char *hash);

// Remove only what the manifest says we put there. Returns how many files went.
// RemoveOne takes out every copy of that run, in either tree.
int WrIntoGameRemoveAll(void);
int WrIntoGameRemoveOne(int mapId, const char *hash);

// The last thing that happened, for the panel. Empty when nothing has.
const char *WrIntoGameStatus(void);

void WrIntoGameShutdown(void);

#endif // WR_INTOGAME_H
