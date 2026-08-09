// wr_limit.h  --  frame pacing, so a second overlay is not needed just for a cap.
//
// WHY THIS IS HERE AT ALL
//
// Running WrLines alongside another overlay whose only job was a frame cap was
// costing more than the cap was worth. The two do not see each other: the other
// tool wraps the swapchain in a proxy object rather than hooking Present, so
// neither detection nor cooperation is possible, and both end up drawing into a
// backbuffer whose presentation the other controls. Doing the cap here removes
// the second tool from the picture entirely, which is a better answer than
// trying to win a fight over the same swapchain.
//
// WRITTEN FROM SCRATCH, ON PURPOSE
//
// Nothing here is derived from any other frame limiter's source. Frame pacing is
// standard technique -- an absolute schedule, a high-resolution wait for the
// bulk of it and a short spin for the tail -- and reproducing a GPL-licensed
// implementation would have quietly relicensed this whole project.
//
// HOW IT PACES
//
// The target is an ABSOLUTE schedule, not "now plus a frame". Advancing from the
// previous target means a frame that runs 0.3 ms late is followed by one that
// waits 0.3 ms less, so error does not accumulate; the naive version drifts and
// the interval it produces is always a little longer than asked for.
//
// The wait itself is a high-resolution waitable timer for everything except the
// last fraction of a millisecond, then a spin. A timer alone lands within about
// half a millisecond, which at 160 fps is 8% of a frame -- visible as judder on
// a variable-refresh display. A spin alone is exact but burns a core. The split
// gets the accuracy where it matters for a few percent of one core.
//
// WHAT IT CANNOT DO
//
// It paces Present calls. It cannot make a frame that took 20 ms of GPU work
// arrive sooner, so a cap above what the machine can sustain does nothing, and
// the statistics below will say so rather than pretend.

#ifndef WR_LIMIT_H
#define WR_LIMIT_H

#include "wr_common.h"

// Wide enough for anything real at either end: 30 for a deliberately slow cap,
// 1000 because some competitive players run uncapped-but-bounded on 500 Hz-class
// panels and there is no reason to stop them.
#define WR_LIMIT_MIN_FPS 30.0f
#define WR_LIMIT_MAX_FPS 1000.0f

struct WrLimitSettings
{
    bool enabled;
    bool autoTarget;        // derive the cap from the display's refresh rate
    float targetFps;        // used when autoTarget is off
    float headroomHz;       // subtracted from the refresh rate in auto mode
    float spinMs;           // length of the busy-wait tail
};

extern WrLimitSettings g_limit;

void WrLimitDefaults(void);

// Called from inside Present, immediately before the real one. Returns without
// doing anything when disabled.
void WrLimitTick(void);

// The refresh rate of the monitor the game window is on, or 0 if unknown.
float WrLimitRefreshHz(void);

// The cap actually in force, resolved from the settings above.
float WrLimitTargetFps(void);

// --- measured, for the UI ----------------------------------------------------

float WrLimitFrameMs(void);     // smoothed present-to-present interval
float WrLimitJitterMs(void);    // worst |error| against the schedule, recent
float WrLimitSpinPercent(void); // share of wall time spent spinning
bool WrLimitCpuBound(void);     // true when frames arrive later than the cap
                                // asks for, i.e. the cap is not the limit

#endif // WR_LIMIT_H
