// wr_timer.h  --  how long you have taken, and how that compares.
//
// WrLines reads Momentum's networked primary timer. The clock starts only when
// the game's TimerState becomes RUNNING and uses the game's own run-time value;
// there is deliberately no movement/anchor stopwatch fallback.
//
// The energy anchor and fitted start zone do not control this clock. They still
// anchor the energy budget and split live recordings into attempts, but timing
// comes only from Momentum's timer entity.
//
// SAVE-LOCS
//
// Momentum's save-locs do not record a time -- the field exists in
// savedlocs.txt and is "-1" in every one of the 3213 entries on this machine.
// So loading a save-loc would otherwise leave this clock reading whatever it
// happened to say. wr_savelocs.h keeps our own note of the elapsed time at each
// save-loc. A rewrite of the game's file supplies a load event and its `cur`
// field identifies the exact cps slot, including duplicate positions; the
// displayed practice clock then resumes from our stored value.

#ifndef WR_TIMER_H
#define WR_TIMER_H

#include "wr_common.h"

struct WrRun;

void WrTimerReset(void);

// Once per frame, with the camera. Handles starting, teleports and save-locs.
void WrTimerTick(const Vec3 &cam, float dt);

bool WrTimerRunning(void);
float WrTimerElapsed(void);

// The current game's interval_per_tick, read alongside Momentum's timer. The
// physics HUD uses this rather than borrowing the nearest downloaded run's
// tickrate. Returns Momentum surf's usual 0.015 while the timer entity is not
// available, which is also the historical fallback everywhere in WrLines.
float WrTimerTickInterval(void);

// Apply a stored save-loc time as an offset from Momentum's live run time.
void WrTimerSet(float seconds, const char *why);

// Legacy test seams. The production UI does not expose manual clock controls;
// a displayed run can only be started by Momentum's own timer.
void WrTimerStart(void);
void WrTimerStop(void);
void WrTimerZero(void);
bool WrTimerManual(void);

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
