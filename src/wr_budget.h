// wr_budget.h  --  "how much did I actually gain?", without counting noise.
//
// Pure float arithmetic, no Windows and no engine types, so a scripted
// trajectory in a test can drive it. Same reason as wr_smooth.h and wr_stress.h.
//
// WHAT THIS IS FOR
//
// The headline energy figure is a NET: it falls for everyone on a descending
// surf map, which reads as "you are doing badly" when it means "the map goes
// down". The obvious fix is to accumulate the gains and the losses separately,
// so there is a number that only ever rises when the strafing works.
//
// THE OBVIOUS WAY TO DO THAT IS A TRAP
//
// The obvious way is to rectify the per-sample change:
//
//     gained += max(0, E_now - E_prev)
//
// which is wrong, and wrong in a way that a plausible test does not catch. The
// live energy is built from a camera-differenced velocity, so E carries noise;
// rectifying noise accumulates it. On a trajectory where energy is exactly
// constant -- true answer zero -- that sum reads in the THOUSANDS over a minute.
//
// And the natural test for it passes anyway. A rectified EMA has a noise floor
// of roughly T*sigma/(tau*sqrt(2*pi)), which does not contain dt at all, so the
// result is the same at 60 fps as at 500 and a frame-rate-independence check
// says "stable" about a number that is 100% noise. tests\test_energy.cpp runs
// exactly that comparison so the trap stays documented rather than remembered.
//
// WHAT WORKS: BANK A LEG ONLY ONCE IT HAS REVERSED
//
// Track the extremes since the last confirmed turning point and bank a leg only
// when the signal comes back by `h`. An excursion smaller than `h` contributes
// EXACTLY ZERO -- not "a little", zero -- and the number of legs is bounded by
// the shape of the signal rather than by how many times it was sampled. That is
// the property the rectifier lacks and the reason this is not a filter setting.
//
// The identity still holds exactly, because every pivot is a real sample value
// and the legs telescope:
//
//     gained - lost == E_now - E_at_seed
//
// Measured over 50 clean surf_demise runs it is exact to the last float bit.
//
// CHOOSING h
//
// It costs little and buys a lot. On the surf_demise world record, gained comes
// out at 4365 / 4189 / 3837 / 3720 / 3381 units for h = 25 / 50 / 100 / 150 /
// 200 -- so raising it from 25 to 150 loses 15% of a real signal.
//
// Against the null trajectory in the harness (energy exactly constant, 2 units
// of camera noise, 60 s) the worst case over six speed/frame-rate combinations:
//
//     h = 25     4253        h = 150      0.0
//     h = 50        4.4      rectifier   12801
//
// h = 50 looks fine there and is not. Re-run it against a binary built with
// unrelated code changed and it moves between 0 and 203: the excursions sit
// right at the threshold, so which ones confirm turns on the last bits of a
// float. h = 150 reads exactly zero and stays there, which is the only version
// of this that can be trusted.
//
// WHAT THE NUMBER IS WORTH
//
// Discrimination gets BETTER at the larger threshold, because what it rejects in
// a slow run is mostly noise-scale wobble. Across 50 clean surf_demise runs, in
// quartiles by time, median gained at h = 150:
//
//     Q1  37.2-38.0 s     2338        Q3  39.0-40.9 s       72
//     Q2  38.1-39.0 s      793        Q4  41.0-57.8 s       14
//
// Monotone, and a factor of 167 end to end -- against 13 at h = 50. The plain
// correlation with run time is only -0.325, and that is not a contradiction:
// the relationship collapses over the first two seconds and is flat after, so a
// linear coefficient understates it badly. Read it as a threshold -- 30 of the
// 50 runs gain under 500 in total and every one of those is slower than 37.8 s
// -- rather than as a score.

#ifndef WR_BUDGET_H
#define WR_BUDGET_H

// How far the signal must come back before a leg is banked, in energy units.
#define WR_SWING_HYSTERESIS 150.0f

struct WrSwing
{
    float h;
    float pivot;        // value at the last confirmed turning point
    float hi, lo;       // extremes since that pivot
    float cur;          // most recent value, for closing the open leg
    int dir;            // +1 rising, -1 falling, 0 not yet decided
    float gained;
    float lost;
    bool have;
};

static inline void WrSwingReset(WrSwing *s, float h)
{
    s->h = (h > 0.0f) ? h : WR_SWING_HYSTERESIS;
    s->pivot = s->hi = s->lo = s->cur = 0.0f;
    s->dir = 0;
    s->gained = 0.0f;
    s->lost = 0.0f;
    s->have = false;
}

// Start measuring again from here WITHOUT banking anything, and keep the totals
// so far. This is what a teleport needs: a save-loc load a thousand units down
// the map is not a thousand units of energy thrown away, and stepping it would
// record exactly that.
static inline void WrSwingSeed(WrSwing *s, float value)
{
    s->pivot = s->hi = s->lo = s->cur = value;
    s->dir = 0;
    s->have = true;
}

static inline void WrSwingStep(WrSwing *s, float value)
{
    if (!s->have)
    {
        WrSwingSeed(s, value);
        return;
    }

    s->cur = value;
    if (value > s->hi) s->hi = value;
    if (value < s->lo) s->lo = value;

    // The first sample past the threshold is necessarily the extreme in its own
    // direction -- everything between the pivot and it was inside the band --
    // so resetting both extremes to it here loses nothing.
    if (s->dir >= 0 && value < s->hi - s->h)
    {
        float leg = s->hi - s->pivot;
        if (leg > 0.0f) s->gained += leg;
        s->pivot = s->hi;
        s->hi = s->lo = value;
        s->dir = -1;
    }
    else if (s->dir <= 0 && value > s->lo + s->h)
    {
        float leg = s->pivot - s->lo;
        if (leg > 0.0f) s->lost += leg;
        s->pivot = s->lo;
        s->hi = s->lo = value;
        s->dir = 1;
    }
}

// Totals including the leg still open, closed at the CURRENT value rather than
// at its extreme. That choice is what makes gained - lost equal the net change
// exactly; closing at the extreme would be a more flattering number and a wrong
// one.
static inline void WrSwingTotals(const WrSwing *s, float *gained, float *lost)
{
    float g = s->gained, l = s->lost;
    if (s->have)
    {
        float d = s->cur - s->pivot;
        if (d > 0.0f) g += d;
        else          l += -d;
    }
    if (gained) *gained = g;
    if (lost) *lost = l;
}

#endif // WR_BUDGET_H
