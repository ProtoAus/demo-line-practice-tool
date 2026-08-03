// wr_pacing.h  --  when the next frame is allowed to be presented.
//
// Just the schedule arithmetic, with no timers, no Windows and no D3D, so it can
// be run against a script of frame timings instead of only against a game. The
// waiting itself is in wr_limit.cpp; this decides what to wait for.
//
// THE SCHEDULE IS ABSOLUTE, not "now plus a frame". Ordinary jitter then cancels
// instead of accumulating: a frame that lands 0.2 ms late is followed by one
// that waits 0.2 ms less, and the long-run cadence is exactly the target.
//
// WHAT HAPPENS AFTER A LATE FRAME is the interesting part, and the first answer
// was wrong for the display this is meant to serve.
//
// A frame that overruns leaves the schedule in debt. Repaying that debt means
// releasing the next frame EARLY -- and on a variable-refresh display an early
// frame is not a correction, it is a second artefact. The panel refreshes when
// the frame arrives, so a short interval is just as visible as a long one. The
// original code repaid up to a quarter of a frame at a time, which turned one
// 6 ms frame into a 6 ms frame followed by a 3.1 ms frame: two wrong intervals
// instead of one, and a 1.9x step between consecutive frames.
//
// So there is no catch-up at all. A late frame is absorbed and the schedule
// resynchronises from where it actually landed. The long-run average then sits
// slightly under the target whenever frames overrun -- which is correct, because
// the alternative is manufacturing an early frame, and time already spent cannot
// be given back.

#ifndef WR_PACING_H
#define WR_PACING_H

struct WrPacing
{
    long long nextTarget;
    bool started;
};

static inline void WrPacingReset(WrPacing *p)
{
    p->nextTarget = 0;
    p->started = false;
}

// When the frame currently being held may be presented. Returns 0 for "present
// it now" -- either the cadence has not started yet, or the target has passed.
//
// The first frame sets the phase to NOW, not now+period, because Advance runs
// straight afterwards and adds the period itself. Setting it a period ahead here
// as well made the second frame of every session wait two full periods -- an
// 8.3 ms interval against a 4.2 ms target, and a 2.0x step between consecutive
// frames, on the very first thing anyone sees.
static inline long long WrPacingTargetFor(WrPacing *p, long long now,
                                          long long period)
{
    if (period <= 0)
        return 0;
    if (!p->started)
    {
        p->started = true;
        p->nextTarget = now;
        return 0;
    }
    return p->nextTarget;
}

// Called once the frame has actually been released, with the time it happened.
//
//   max(nextTarget + period, released + period)
//
// The first term keeps the absolute cadence when frames are on time. The second
// is the whole of the late-frame policy: never schedule the next frame less than
// a full period after the one that just went out, however far behind we are.
static inline void WrPacingAdvance(WrPacing *p, long long released,
                                   long long period)
{
    if (period <= 0)
        return;
    p->nextTarget += period;
    if (p->nextTarget < released + period)
        p->nextTarget = released + period;
}

#endif // WR_PACING_H
