// wr_maps.h  --  every map Momentum knows about, and what we have for it.
//
// WHY THIS DOES NOT TOUCH THE NETWORK
//
// The game already keeps the whole catalogue on disk. momentum\_cache holds an
// approved list and a submission list -- an MSML header, then raw zlib from
// offset 12, decompressing to JSON with each map's id, name and leaderboards.
// 2049 maps on this machine. Browsing needs nothing from anybody's server.
//
// WHY THE READING IS DONE IN PYTHON
//
// That cache is zlib and JSON, and this DLL links neither. Its import list is
// five system DLLs and every build checks that with dumpbin, which is what makes
// "it reads memory and two files" something you can verify rather than believe.
// So wrpath_extract.py --index-maps writes wrlines_data\maps.txt, a tab-separated
// list, and this reads that. The heavy lifting was already on the Python side of
// the fence; this keeps it there.
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

void WrMapsShutdown(void);

#endif // WR_MAPS_H
