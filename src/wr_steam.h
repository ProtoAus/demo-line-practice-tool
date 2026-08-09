// wr_steam.h  --  player names and avatars, straight from the Steam client.
//
// The .wrpath files carry a SteamID64 and the player's name at the time of the
// run. The name is free. The avatar is not: Steam only keeps pictures on disk
// for people it has recently had reason to know about, and on this machine that
// is exactly one file -- the user's own. So the avatars have to be asked for.
//
// WHY THIS IS NOT THE THING THAT KEPT CRASHING THE GAME
//
// The engine work in wr_engine/wr_probe had to guess vtable indices, and a wrong
// guess took the process down a second later with nothing to blame. None of that
// applies here. steam_api64.dll exports a flat C API by name, with published and
// stable signatures, and GetProcAddress either returns the function we asked for
// or NULL. There is nothing to guess.
//
// What can still go wrong is version drift -- the interface accessor is named
// after its version, e.g. SteamAPI_SteamFriends_v018 -- so the accessor is
// resolved from a short list and the result is logged. A game update that moves
// to v019 produces a line in the log, not a crash.
//
// WHAT THIS COSTS, AND WHY IT IS OPTIONAL
//
// RequestUserInformation asks the Steam client to go and fetch that user's
// persona and avatar. That is a network lookup -- the same one a scoreboard
// triggers when it shows names -- and it is the only outward-facing thing
// WrLines does. It is behind a toggle, and with it off nothing here runs.

#ifndef WR_STEAM_H
#define WR_STEAM_H

#include "wr_common.h"

// Resolve steam_api64.dll and the two interfaces. Safe to call repeatedly; only
// the first call does work. Returns false if Steam is not usable, in which case
// every accessor below degrades quietly.
bool WrSteamInit(void);

// Call once per frame from the render thread. Advances a small number of
// pending avatar lookups so a map with forty runs never stalls a frame.
void WrSteamTick(void);

// Ask for this player's details. Cheap and idempotent -- the first call queues
// the lookup, later ones are a cache probe.
void WrSteamWant(unsigned long long steamId);

// The avatar as an ImGui texture id (an ID3D11ShaderResourceView), or NULL if it
// is not available yet, was never requested, or Steam could not supply it.
// `size` receives the square edge length in pixels.
void *WrSteamAvatar(unsigned long long steamId, int *size);

// Steam's current persona name, or NULL to fall back to the name recorded in
// the .wrpath at the time of the run.
const char *WrSteamPersona(unsigned long long steamId);

// Diagnostics
bool WrSteamAvailable(void);
const char *WrSteamStatus(void);
int WrSteamAvatarCount(void);
int WrSteamPendingCount(void);

// Your Steam friends, as SteamID64s.
//
// Momentum's own leaderboard filter=friends answers 401 without an account, so
// the site does not hand this out -- but asking its API for specific SteamID64s
// is not gated. This is the half only the DLL can do: it is injected into the
// game, so it has a live ISteamFriends, and the fetch script does not.
//
// Reads local client state, so it is synchronous and costs nothing. Enumerated
// on first use and cached; Refresh re-reads it.
int WrSteamFriendCount(void);
unsigned long long WrSteamFriendAt(int i);
bool WrSteamIsFriend(unsigned long long id);    // sorted, binary searched
void WrSteamRefreshFriends(void);

// False when steam_api64 is too old to export the two enumeration functions.
// They are deliberately not part of the mandatory export set, so an old DLL
// loses this and keeps its avatars.
bool WrSteamCanListFriends(void);

// Master switch. Turning it off stops all lookups immediately; avatars already
// fetched stay usable.
void WrSteamSetEnabled(bool on);
bool WrSteamEnabled(void);

void WrSteamShutdown(void);

#endif // WR_STEAM_H
