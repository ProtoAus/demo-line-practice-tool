// wr_gametimer.h  --  Momentum's authoritative, networked run clock.
//
// The game exposes one primary timer entity for the local player.  Its receive
// tables contain both the timer state and the current run time; reading those
// values means WrLines starts on the same tick as Momentum instead of guessing
// from movement or from a fitted start circle.

#ifndef WR_GAMETIMER_H
#define WR_GAMETIMER_H

struct WrGameTimerSample
{
    int state;                  // Momentum's TimerState: 0 disabled, 1 primed,
                                // 2 running, 3 finished
    double seconds;
    unsigned int identity;      // the primary timer handle; changes on replace
    int tick;                   // game tick used to extrapolate `seconds`
    float tickInterval;
};

// Read the local player's primary timer. False is a clean, visible failure:
// callers must not fall back to a movement stopwatch and pretend it is the
// game's timer.
bool WrGameTimerRead(WrGameTimerSample *out);

void WrGameTimerReset(void);    // map change / entity-list rebuild
const char *WrGameTimerStatus(void);

#endif // WR_GAMETIMER_H
