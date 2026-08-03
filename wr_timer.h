// wr_timer.h  --  how long you have taken, and how that compares.
//
// WrLines does not read the game's run timer. It could -- another memory scan --
// but it does not need to: it already knows where you are every frame, and the
// anchor (wr_energy.h) gives a place to start counting from. So this is our own
// clock, started when you leave the anchor.
//
// WHY THE ANCHOR DEFAULTS TO THE REFERENCE RUN'S FIRST POINT
//
// A .wrpath stores a time per point, measured from that run's own first sample.
// Comparing your elapsed time against theirs is only meaningful if both clocks
// start in the same place -- so by default the anchor IS their first point, and
// then their t = 0 and your t = 0 coincide by construction. Anchor somewhere
// else and the comparison silently acquires an offset, which is why the panel
// says which anchor is in use rather than just showing a number.
//
// SAVE-LOCS
//
// Momentum's save-locs do not record a time -- the field exists in
// savedlocs.txt and is "-1" in every one of the 3213 entries on this machine.
// So loading a save-loc would otherwise leave this clock reading whatever it
// happened to say. wr_savelocs.h keeps our own note of the elapsed time at each
// save-loc, and on a teleport into one this clock is set back to it.

#ifndef WR_TIMER_H
#define WR_TIMER_H

#include "wr_common.h"

struct WrRun;

void WrTimerReset(void);

// Once per frame, with the camera. Handles starting, teleports and save-locs.
void WrTimerTick(const Vec3 &cam, float dt);

bool WrTimerRunning(void);
float WrTimerElapsed(void);

// Force the clock, for a save-loc load or a manual set.
void WrTimerSet(float seconds, const char *why);

// Your time against the reference run's time at the point of its line nearest
// you. Returns false when there is nothing honest to compare:
//
//   - no reference run near you
//   - the clock has not started
//   - that run's stored timing could not be trusted (see WrRun::timingTrusted)
//
// The last one matters more than it sounds: the extractor's per-point clock can
// be out by a factor of ten on a badly-recovered run, and a confident wrong
// delta is worse than no delta.
bool WrTimerDelta(const WrRun *ref, float *ours, float *theirs, float *delta);

// Why the comparison is unavailable, for the UI to say plainly. Empty when it
// is available.
const char *WrTimerWhyNot(const WrRun *ref);

#endif // WR_TIMER_H
