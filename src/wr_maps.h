// wr_maps.h  --  every map Momentum knows about, and what we have for it.
//
// WHY THIS DOES NOT TOUCH THE NETWORK
//
// The game already keeps the whole catalogue on disk. momentum\_cache holds an
// approved list and a submission list -- an MSML header, then raw zlib from
// offset 12, decompressing to JSON with each map's id, name and leaderboards.
// 2049 maps on this machine. Browsing needs nothing from anybody's server.
//
// WHY THERE IS STILL A maps.txt IN BETWEEN
//
// The reading used to be done in Python, because the cache is zlib and JSON and
// this DLL linked neither. It does now -- see wr_msml.cpp and third_party\ --
// so the obvious question is why the flat file survives when one module both
// writes and reads it.
//
// Because they are not the same operation. Rebuilding the index inflates twelve
// megabytes and parses a million JSON nodes; reading it is a 100 KB fgets loop.
// One happens when you press a button, the other on every panel open, and the
// file is what keeps the second one cheap. It also survives a restart, which is
// the difference between the Maps tab being instant and it costing a second
// every time the game launches.
//
// So: WrMsmlRead + WrMapsWriteIndex do the expensive half, and everything below
// this comment reads the result. Keeping the writer in this file rather than in
// wr_msml.cpp is deliberate -- the format has exactly one writer and one reader
// and they are forty lines apart, so they cannot drift.
//
// The index is a cache of a cache and can be stale. It says when it was written
// and there is a button to rewrite it.

#ifndef WR_MAPS_H
#define WR_MAPS_H

#include "wr_common.h"

struct WrMapInfo
{
    int id;                 // Momentum's map id; momtv\online\<id>\ matches it
    char name[72];
    int tier;               // main track, 0 when unknown
    bool approved;          // as opposed to still in submission
    int demos;              // .mtv files we hold for it
    int extracted;          // .wrpath files we have made from them
};

// Read wrlines_data\maps.txt, then count what is on disk for each. Background
// thread; cheap to call.
void WrMapsRefresh(void);
bool WrMapsReady(void);
const char *WrMapsStatus(void);

int WrMapsCount(void);
const WrMapInfo *WrMapsAt(int index);

// Index into the table, or -1. Used to show the map you are standing in first.
int WrMapsFind(const char *name);

// Rebuild wrlines_data\maps.txt from the game's own cache. Returns how many maps
// were written, or -1 if the cache is not there. `emit` gets the progress lines,
// or NULL for none.
//
// Byte-for-byte what wrpath_extract.py --index-maps wrote, sorted by name, and
// the parity harness holds it to that. Text mode, so the newlines are CRLF --
// the reader below opens the file in text mode and would otherwise see a stray
// carriage return on the end of every last field.
typedef void (*WrMapsEmitFn)(const char *line);
int WrMapsWriteIndex(const char *gameDir, WrMapsEmitFn emit);

// ---------------------------------------------------------------------------
// How a map is cut up
// ---------------------------------------------------------------------------
//
// wrlines_data\tracks.txt: one line per map, "name<TAB>stages<TAB>bonuses",
// written by the same catalogue pass that writes maps.txt and read by the quick
// menu to know which legs to offer.
//
// A SEPARATE FILE, and the reason is a promise maps.txt makes. That file is
// byte-for-byte what wrpath_extract.py --index-maps wrote, and the reference is
// frozen, so it cannot gain a column without the two implementations disagreeing
// about a file they both write. tracks.txt is ours alone and there is nothing to
// diverge from.
//
// WHY THE ANSWER IS NOT AVAILABLE ANYWHERE CHEAPER
//
// Nothing else knows which stages a map has until you have already asked for
// them. The run store knows the legs you have extracted; the board cache knows
// the legs you have fetched; the leaderboard API answers per leg rather than
// listing them. So on a map you have never touched, every one of those sources
// says "main", on a map that has nine stages -- and the quick menu would offer
// you one chip and no way to find the other nine. The game's own catalogue has
// the exact answer for all two thousand maps at once, offline, and the only
// thing it costs is being read.
//
// Written silently by WrMapsWriteIndex: no progress line, nothing on stdout.

// False when there is no tracks.txt, which is the ordinary state before the
// catalogue has been read once. Leaves both counts at 0.
bool WrMapsTracksFor(const char *map, int *stages, int *bonuses);

// Has the file been read this session? For the one line the quick menu shows
// when it is working from guesses rather than from the catalogue.
bool WrMapsTracksKnown(void);

// Re-read tracks.txt. Cheap -- it is a 60 KB fgets loop -- and called when the
// catalogue pass finishes so the answer is not a restart away.
void WrMapsTracksReload(void);

void WrMapsShutdown(void);

#endif // WR_MAPS_H
