// wr_savelocs.h  --  read Momentum's save-locs, and remember a time for each.
//
// Momentum keeps them in <game>\momentum\savedlocs.txt, a plain Valve
// KeyValues file: one block per map, a numbered "cps" entry per save-loc, each
// with pos, vel, ang, zone, track -- and a "time" field.
//
// That time field is always "-1". Verified across all 3213 entries of 260 maps
// on this machine: the field exists in the format and is never populated. So a
// save-loc tells you where you were and nothing about how long it took to get
// there, which is exactly the gap this fills. Chain them and the clock carries
// across, which is what makes practising a map section by section possible.
//
// WHAT THIS DOES NOT DO
//
// It never writes to savedlocs.txt, or to anything else in the game install.
// Our times live in wrlines_data\savelocs\<map>.txt and nowhere else. The game's
// file is opened read-only, shared, and only re-read when its timestamp changes.
//
// STAMPED WHEN ONE IS CREATED, NOT WHEN YOU WALK PAST ONE
//
// The first version of this stamped "the nearest save-loc within 24 units that
// has no time yet", on the theory that being at one you have never timed means
// you just made it. That is wrong, and it corrupted its own data: merely walking
// past an untimed save-loc stamped it with whatever the clock said. On this
// machine it produced 111 stamps, and surf_hades2's sidecar ended up with twenty
// entries clustered at the spawn, each written by a different lap.
//
// The signal for "just created" was already available and unused. The game's
// file is polled for changes; a re-read triggered by that change which turns up
// an entry matching nothing we knew about IS a save-loc that was just made. So
// the clock is captured at the moment the change is noticed, and only genuinely
// new entries are stamped with it.
//
// It was also stamping on the frame a save-loc was LOADED, because the timer ran
// before this did and so the restore had not happened yet. That wrote the
// pre-load clock -- and often 0.000, which then restored as 0.000 for ever.
//
// KEYED ON POSITION AND ORDINAL
//
// Save-loc indices move: delete the second of five and the rest renumber. A
// position rounded to a tenth of a unit is stable under that. But positions
// collide -- a respawn point accumulates several save-locs at the same spot --
// so the key carries an ordinal among entries sharing a position, in file order.

#ifndef WR_SAVELOCS_H
#define WR_SAVELOCS_H

#include "wr_common.h"

// Re-read the game's file for this map if it has changed, on a background
// thread. Cheap to call every frame. `elapsed`/`running` are the run clock as it
// stands at the moment a change is noticed, which is what a newly created
// save-loc gets stamped with.
void WrSavelocRefresh(const char *map, float elapsed, bool running);

// Our recorded time for the save-loc nearest `pos`, if there is one within a few
// units. False when that save-loc has no recorded time -- which is the normal
// case for every save-loc made before this tool ever ran.
bool WrSavelocTimeAt(const Vec3 &pos, float *seconds);

// --- for the panel -----------------------------------------------------------

int WrSavelocCount(void);          // save-locs the game has for this map
int WrSavelocTimedCount(void);     // how many of those we have a time for
const char *WrSavelocStatus(void);

// One row of the list. `seconds` is negative when untimed. `suspect` marks a
// time read from a sidecar written before the stamping bug was fixed, which
// cannot be told apart from a good one and so is shown rather than deleted.
struct WrSavelocRow
{
    Vec3 pos;
    float seconds;
    bool suspect;
};
bool WrSavelocAt(int index, WrSavelocRow *out);

void WrSavelocForget(int index);    // drop one time, keep the save-loc
void WrSavelocForgetAll(void);      // drop every time for this map

// The last thing that happened, for a moment, so a stamp or a restore is visible
// somewhere other than the log. Empty when nothing recent.
const char *WrSavelocRecent(float *ageSeconds);
void WrSavelocNoteRestore(float seconds);

void WrSavelocShutdown(void);

#endif // WR_SAVELOCS_H
