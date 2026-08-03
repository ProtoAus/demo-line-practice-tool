// wr_savelocs.h  --  read Momentum's save-locs, and remember a time for each.
//
// Momentum keeps them in <game>\momentum\savedlocs.txt, a plain Valve
// KeyValues file: one block per map, a numbered "cps" entry per save-loc, each
// with pos, vel, ang, zone, track -- and a "time" field.
//
// That time field is always "-1". Verified across all 3213 entries of 260 maps
// on this machine: the field exists in the format and is never populated. So a
// save-loc tells you where you were and nothing about how long it took to get
// there, which is exactly the gap this fills.
//
// WHAT THIS DOES NOT DO
//
// It never writes to savedlocs.txt, or to anything else in the game install.
// Our times live in wrlines_data\savelocs\<map>.txt and nowhere else. The game's
// file is opened read-only, shared, and only re-read when its timestamp changes.
//
// KEYED ON POSITION, NOT INDEX
//
// Save-loc indices move: delete the second of five and the rest renumber. A
// position rounded to a tenth of a unit is stable under that, and two save-locs
// close enough to collide are close enough that either time is the right answer.

#ifndef WR_SAVELOCS_H
#define WR_SAVELOCS_H

#include "wr_common.h"

// Re-read the game's file for this map if it has changed, on a background
// thread. Cheap to call every frame.
void WrSavelocRefresh(const char *map);

// Once per frame. Stamps any save-loc the player is standing on that we have no
// time for yet -- which is what "the user just made one" looks like from here,
// with no need to know what command they typed. Writes our sidecar only when
// that actually happens.
void WrSavelocTick(const Vec3 &cam, float elapsed, bool running);

// Our recorded time for the save-loc nearest `pos`, if there is one within a few
// units. False when that save-loc has no recorded time -- which is the normal
// case for every save-loc made before this tool ever ran.
bool WrSavelocTimeAt(const Vec3 &pos, float *seconds);

// For the panel.
int WrSavelocCount(void);          // save-locs the game has for this map
int WrSavelocTimedCount(void);     // how many of those we have a time for
const char *WrSavelocStatus(void);

void WrSavelocShutdown(void);

#endif // WR_SAVELOCS_H
