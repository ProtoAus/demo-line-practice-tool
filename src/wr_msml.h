// wr_msml.h  --  the map catalogue, out of the game's own cache.
//
// WHY THERE IS A CATALOGUE TO READ AT ALL
//
// Momentum keeps every map it knows about in momentum\_cache\*.dat, written
// whenever the game fetches the map list. That is the whole catalogue -- two
// thousand-odd maps -- already on disk, already paid for. Listing maps
// therefore costs nothing and asks nobody's server anything, which is why the
// Maps tab can show you every map in the game while the Board tab has to be
// asked before it touches the network.
//
// WHAT THE FORMAT IS
//
// Four bytes of magic, "MSML". Then two little-endian u32s: the decompressed
// size and the number of maps. Then a raw zlib stream, and inside it a JSON
// array of map objects.
//
// The two counts are advisory here. They are used to size the output buffer
// exactly instead of guessing, and they are checked rather than trusted -- this
// file is written by somebody else's program and a wrong length is a wrong
// length, not an invitation to allocate four gigabytes.
//
// Of each map object we want three things: the id, the name, and from its
// leaderboards the tier (the one on trackType 0) and the set of gamemodes it
// has a board in. Everything else -- credits, images, submission history,
// version lists -- is skipped without being parsed.
//
// TIER IS OFTEN NULL, AND MODES ARE NOT A FILTER
//
// A submitted map that has not been rated has "tier": null, which reads as 0.
// And nearly every map lists a board in nearly every mode -- all 546 surf maps
// here claim twelve -- because Momentum creates them whether or not anyone has
// ever run the map that way. So the mode list says what boards EXIST, not which
// have runs, and it cannot be used to guess what mode a map is for.
//
// WHY THIS USED TO BE PYTHON
//
// It needed zlib, and linking zlib into an injected DLL was the thing this
// project spent a release avoiding. That is settled differently now: the
// inflate is thirty lines of miniz called from one place, committed under
// third_party\ where you can diff it against upstream, and it buys not needing
// an interpreter installed to see a list of maps.

#ifndef WR_MSML_H
#define WR_MSML_H

#include "wr_common.h"

// One map, as much of it as anything here cares about.
struct WrMsmlMap
{
    int id;
    char name[72];
    int tier;                   // 0 when unrated, which is also null
    unsigned int modes;         // bit N set = a board exists in gamemode N

    // How many legs the map is cut into: the highest trackNum this map lists a
    // board for, per track type. 0 means it has none of that kind.
    //
    // The one thing here that is NOT in maps.txt, because maps.txt is compared
    // byte for byte against what the reference wrote and cannot gain a column.
    // It goes to wrlines_data\tracks.txt instead -- see WrMapsWriteTracks.
    //
    // WHY IT IS WORTH READING AT ALL
    //
    // "Which stages does this map have" has no cheap answer anywhere else. The
    // run store only knows the legs you have already extracted, the board cache
    // only the legs you have already fetched, and the leaderboard API answers
    // per leg rather than listing them -- so a map you have never touched offers
    // you the main track and nothing else, on a map with nine stages. The
    // catalogue the game itself keeps on disk has the exact answer for every map
    // at once, for free, offline. It was simply not being read out.
    //
    // A HIGHEST rather than a set, because Momentum numbers stages 1..N and
    // bonuses 1..M with no gaps in anything seen. A map that breaks that would
    // offer a leg with no runs on it, which is a wasted chip and not a wrong
    // answer.
    unsigned char stages;       // trackType 1
    unsigned char bonuses;      // trackType 2

    bool approved;              // from the FILENAME, not the map. See below.
};

// Read every .dat under <game>\momentum\_cache into `out`.
//
// Files are read in sorted filename order and a later file WINS on a duplicate
// map name -- the same rule the reference implementation gets from assigning
// into a dict, and it matters: approved_*.dat and submission_*.dat overlap by
// around eighty maps, and a map that has been approved since the submission
// cache was written should read as approved.
//
// `approved` comes from the filename beginning with "approved". That is not an
// inference about the map; it is which cache file the game put it in.
//
// Returns the number filled in, or -1 if the cache directory is not there.
// Never partially fills: a .dat that will not decompress or will not parse is
// skipped whole, exactly as the reference does.
int WrMsmlRead(const char *gameDir, WrMsmlMap *out, int maxOut, int *skippedFiles);

// Where the cache lives, for the error message when it is missing.
void WrMsmlCacheDir(const char *gameDir, char *out, int cap);

#endif // WR_MSML_H
