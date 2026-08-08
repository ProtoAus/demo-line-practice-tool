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

// Everything we know about one save-loc. `seconds` is negative when we have
// never timed it, which is the normal case -- 3098 of the 3239 on this machine.
//
// `vel` is the game's own record and is present for effectively all of them, so
// the two useful fields here have very different coverage on purpose: the clock
// restores rarely, the velocity restores always.
struct WrSavelocHit
{
    Vec3 pos;
    Vec3 vel;
    bool haveVel;
    bool fromCps;       // a real save-loc, not an entry from "startmarks"
    float seconds;
    int ordinal;        // among entries sharing this position, in file order
    bool suspect;       // time came from a v1 sidecar and may be wrong
    bool byHand;        // time was typed in rather than stamped on creation
    bool haveEnergy;    // gained/lost/peak below mean something
    float gained, lost, peak;
};

// The save-loc nearest `pos`, timed or not, within a few units. ONE lookup, so
// the clock and the energy readout can never restore from two different
// save-locs -- see the comment on the implementation.
bool WrSavelocMatch(const Vec3 &pos, WrSavelocHit *out);

// Our recorded time for the save-loc nearest `pos`, if there is one within a few
// units. False when that save-loc has no recorded time -- which is the normal
// case for every save-loc made before this tool ever ran. A thin wrapper over
// WrSavelocMatch, kept because most callers want only this.
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
    bool byHand;        // typed in, so its provenance is neither stamp nor bug
    bool haveVel;       // the game recorded a velocity we can restore
    float speed;        // |vel|, for the panel -- 0 when haveVel is false
};
bool WrSavelocAt(int index, WrSavelocRow *out);

void WrSavelocForget(int index);    // drop one time, keep the save-loc
void WrSavelocForgetAll(void);      // drop every time for this map

// Set a time by hand, for a save-loc made before this tool existed. Refused if
// not positive: a zero could only mean the clock was not running, which is the
// bug the stamping rules above exist to prevent. Writes the sidecar.
void WrSavelocSetTime(int index, float seconds);

// --- for the harness ---------------------------------------------------------

// Parse any savedlocs.txt, not just the game's. The rules being tested -- which
// section an entry came from, and which entry a velocity belongs to -- are
// properties of Momentum's file format, so the test drives a fixture through the
// real parser rather than re-implementing the format and agreeing with itself.
bool WrSavelocParseFile(const char *path, const char *map,
                        WrSavelocHit *out, int maxOut, int *count);

// The last thing that happened, for a moment, so a stamp or a restore is visible
// somewhere other than the log. Empty when nothing recent.
const char *WrSavelocRecent(float *ageSeconds);
void WrSavelocNoteRestore(float seconds);

void WrSavelocShutdown(void);

#endif // WR_SAVELOCS_H
