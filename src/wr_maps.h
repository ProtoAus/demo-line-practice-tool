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

void WrMapsShutdown(void);

#endif // WR_MAPS_H
