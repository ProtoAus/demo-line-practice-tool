// wr_smooth.h  --  filters that measure time in seconds instead of in frames.
//
// Pure float arithmetic, no Windows, no engine types, so it can be driven by a
// scripted trajectory in a test rather than only by a running game.
//
// WHY THIS EXISTS AT ALL
//
// Every filter in the energy readout used to be frame-count based: a velocity
// baseline of "4 frames", an EMA of "alpha = 0.25 per frame", a ground debounce
// of "3 frames", a trend window of "8 history samples". At 200 fps that EMA has
// a time constant of 15 ms; at 60 fps it is 50 ms. The readout therefore behaved
// like a different instrument depending on the frame rate, and the frame rate on
// a surf map varies with how many lines are drawn -- so turning lines on changed
// how the number moved. Everything here takes dt and a time constant in seconds.
//
// The EMA is the exact continuous solution, not the usual `alpha = dt / tau`
// approximation:
//
//     alpha = 1 - exp(-dt / tau)
//
// which is stable for any dt (a 2-second hitch gives alpha ~ 1, i.e. "just take
// the new value") where the linear form would overshoot and ring.

#ifndef WR_SMOOTH_H
#define WR_SMOOTH_H

#include <math.h>

// ---------------------------------------------------------------------------
// Exponential moving average
// ---------------------------------------------------------------------------

static inline float WrEmaAlpha(float dt, float tau)
{
    if (tau <= 0.0f)
        return 1.0f;
    if (dt <= 0.0f)
        return 0.0f;
    float a = 1.0f - expf(-dt / tau);
    return (a < 0.0f) ? 0.0f : (a > 1.0f ? 1.0f : a);
}

struct WrEma
{
    float v;
    bool has;
};

static inline void WrEmaReset(WrEma *e)
{
    e->v = 0.0f;
    e->has = false;
}

static inline float WrEmaStep(WrEma *e, float x, float dt, float tau)
{
    if (!e->has)
    {
        e->v = x;
        e->has = true;
        return e->v;
    }
    float a = WrEmaAlpha(dt, tau);
    e->v += (x - e->v) * a;
    return e->v;
}

// Start from a value that is already known to be right.
//
// This is the same thing the `!has` branch above does on the first sample after
// a reset, exposed so a caller who KNOWS the answer can supply it instead of
// waiting to measure one. There is exactly one such caller: loading a save-loc,
// where the velocity on the far side of the discontinuity is written down in
// the game's own file. Everything else has to measure, and should.
//
// After this the filter behaves normally -- the next real sample is blended in
// at the usual rate, from the truth rather than from nothing.
static inline void WrEmaSeed(WrEma *e, float x)
{
    e->v = x;
    e->has = true;
}

// ---------------------------------------------------------------------------
// Velocity over a fixed time window
// ---------------------------------------------------------------------------
//
// A finite difference across a fixed number of FRAMES measures a different
// interval at every frame rate: 4 frames is 20 ms at 200 fps and 67 ms at 60.
// Since the noise being rejected is view bob -- a fixed amplitude in units, not
// in frames -- the rejection has to be specified in seconds too.
//
// The window is a ring of timestamped positions; the estimate is the difference
// between the newest sample and the oldest one still inside the window.

#define WR_VEL_RING 128

struct WrVelWindow
{
    float x[WR_VEL_RING], y[WR_VEL_RING], z[WR_VEL_RING], t[WR_VEL_RING];
    int head;       // next write slot
    int count;
    float clock;
};

static inline void WrVelReset(WrVelWindow *w)
{
    w->head = 0;
    w->count = 0;
    w->clock = 0.0f;
}

static inline void WrVelPush(WrVelWindow *w, float x, float y, float z, float dt)
{
    w->clock += dt;
    w->x[w->head] = x;
    w->y[w->head] = y;
    w->z[w->head] = z;
    w->t[w->head] = w->clock;
    w->head = (w->head + 1) % WR_VEL_RING;
    if (w->count < WR_VEL_RING)
        w->count++;
}

// How much of the requested window must actually be spanned before an estimate
// is reported at all. Below 1.0 so that a frame rate whose dt does not divide
// the window evenly is not made to wait an extra frame.
#define WR_VEL_MIN_SPAN 0.75f

// The oldest sample at least `window` seconds old, or the oldest we have.
// Returns false until the window is genuinely spanned -- see the gate below.
//
// It also hands back the position at the MIDDLE of the window, and that is not a
// convenience -- it is what makes the energy figure correct while accelerating.
//
// A finite difference over a window returns the velocity at the window's
// midpoint exactly, for any constant acceleration. Pairing that velocity with
// the position at the window's END mismatches the two by half a window, and
// under gravity that is a systematic error, not noise: the first version of the
// test showed the energy of a ballistic arc decaying by 46 units over 1.6 s of
// free fall, purely because v was measured 20 ms before z. Taking both at the
// midpoint makes E exact under constant acceleration, at the cost of a 20 ms
// display lag that is invisible next to the 300 ms output filter.
static inline bool WrVelEstimate(const WrVelWindow *w, float window,
                                 float *vx, float *vy, float *vz,
                                 float *mx, float *my, float *mz)
{
    if (w->count < 2)
        return false;

    int newest = (w->head - 1 + WR_VEL_RING) % WR_VEL_RING;
    float tNew = w->t[newest];

    // Walk back until far enough, stopping at the oldest sample we hold.
    int pick = newest;
    for (int i = 1; i < w->count; i++)
    {
        int idx = (w->head - 1 - i + 2 * WR_VEL_RING) % WR_VEL_RING;
        pick = idx;
        if (tNew - w->t[idx] >= window)
            break;
    }

    float span = tNew - w->t[pick];
    if (span <= 1e-5f)
        return false;

    // REFUSE A PARTIAL WINDOW. This is not defensive tidying -- it is the whole
    // fix for a reported defect.
    //
    // Whenever the window has just been emptied (a teleport, a save-loc load, a
    // map change) it holds one sample, then two. Without this gate the very next
    // frame reports a velocity differenced over a SINGLE FRAME -- 5 ms at 200
    // fps, against a window designed to be 40 -- and the caller's filters have
    // just been reset too, so an unseeded EMA returns that value verbatim rather
    // than filtering it. The entire readout was therefore decided by one 5 ms
    // position difference. A 20-unit camera step in that 5 ms reads as 4000 u/s,
    // which is 10,000 energy units, and it then decayed away over the following
    // third of a second: the reported "loading a save-loc shows a crazy value
    // and takes half a second to come down".
    //
    // Reporting nothing until there is a real window to measure over costs 40 ms
    // of held display and removes the spike entirely.
    //
    // The ring-full case is the honest exception: past about 3000 fps the ring
    // cannot hold a whole window, and a slightly short baseline beats no readout.
    if (span < window * WR_VEL_MIN_SPAN && w->count < WR_VEL_RING)
        return false;

    float inv = 1.0f / span;
    if (vx) *vx = (w->x[newest] - w->x[pick]) * inv;
    if (vy) *vy = (w->y[newest] - w->y[pick]) * inv;
    if (vz) *vz = (w->z[newest] - w->z[pick]) * inv;

    if (mx || my || mz)
    {
        // The stored sample nearest the midpoint time. A real sample, not an
        // interpolation between the ends -- the chord midpoint of a curve is not
        // on the curve, which is the error this exists to avoid.
        float tMid = w->t[pick] + span * 0.5f;
        int best = pick;
        float bestErr = 1e30f;
        for (int i = 0; i < w->count; i++)
        {
            int idx = (w->head - 1 - i + 2 * WR_VEL_RING) % WR_VEL_RING;
            float err = w->t[idx] - tMid;
            if (err < 0.0f) err = -err;
            if (err < bestErr) { bestErr = err; best = idx; }
            if (w->t[idx] < tMid - span)
                break;
        }
        if (mx) *mx = w->x[best];
        if (my) *my = w->y[best];
        if (mz) *mz = w->z[best];
    }
    return true;
}

// ---------------------------------------------------------------------------
// Trend over a fixed time window
// ---------------------------------------------------------------------------
//
// The old trend indexed a 20 Hz history by a fixed number of SAMPLES, so its
// window was 0.4 s at 200 fps and 0.53 s at 60, and it stepped discretely as the
// index advanced. This holds real timestamps and interpolates nothing -- it just
// takes the oldest sample inside the window, which is a stable comparison as
// long as the ring is denser than the window.

#define WR_TREND_RING 256

struct WrTrendWindow
{
    float v[WR_TREND_RING], t[WR_TREND_RING];
    int head;
    int count;
    float clock;
};

static inline void WrTrendReset(WrTrendWindow *w)
{
    w->head = 0;
    w->count = 0;
    w->clock = 0.0f;
}

static inline void WrTrendPush(WrTrendWindow *w, float value, float dt)
{
    w->clock += dt;
    w->v[w->head] = value;
    w->t[w->head] = w->clock;
    w->head = (w->head + 1) % WR_TREND_RING;
    if (w->count < WR_TREND_RING)
        w->count++;
}

// The change over `window` seconds, AND how long a span that actually covered.
//
// The second output exists because the ring holds 256 samples and no more. At
// 60 fps that is 4.2 seconds and any window fits; at 300 fps it is 0.85, and a
// request for more than that quietly returns the change over 0.85 seconds
// instead. That is harmless for the arrow, which only wants a sign -- and wrong
// for anything that divides by the window to get a RATE, which then reads low
// by exactly the ratio it was short by. WrEnergyPower does divide.
//
// `actual` may be NULL; WrTrendOver is the same call without it.
static inline float WrTrendOverSpan(const WrTrendWindow *w, float window,
                                    float *actual)
{
    if (actual)
        *actual = 0.0f;
    if (w->count < 2)
        return 0.0f;
    int newest = (w->head - 1 + WR_TREND_RING) % WR_TREND_RING;
    float tNew = w->t[newest];

    int pick = newest;
    for (int i = 1; i < w->count; i++)
    {
        int idx = (w->head - 1 - i + 2 * WR_TREND_RING) % WR_TREND_RING;
        pick = idx;
        if (tNew - w->t[idx] >= window)
            break;
    }
    if (actual)
        *actual = tNew - w->t[pick];
    return w->v[newest] - w->v[pick];
}

static inline float WrTrendOver(const WrTrendWindow *w, float window)
{
    return WrTrendOverSpan(w, window, 0);
}

// ---------------------------------------------------------------------------
// An arrow that does not strobe
// ---------------------------------------------------------------------------
//
// A bare sign test on a noisy trend flips every frame near the threshold. This
// requires the new state to hold continuously for `hold` seconds before it is
// allowed on screen, which converts a flicker into a slightly late but stable
// indicator -- the right trade for something read out of the corner of the eye.

struct WrArrow
{
    int shown;      // -1, 0, +1
    int pending;
    float held;
};

static inline void WrArrowReset(WrArrow *a)
{
    a->shown = 0;
    a->pending = 0;
    a->held = 0.0f;
}

static inline int WrArrowStep(WrArrow *a, float trend, float band, float dt,
                              float hold)
{
    int want = (trend > band) ? 1 : (trend < -band ? -1 : 0);
    if (want == a->shown)
    {
        a->pending = want;
        a->held = 0.0f;
        return a->shown;
    }
    if (want != a->pending)
    {
        a->pending = want;
        a->held = 0.0f;
    }
    a->held += dt;
    if (a->held >= hold)
    {
        a->shown = want;
        a->held = 0.0f;
    }
    return a->shown;
}

// ---------------------------------------------------------------------------
// Display quantisation with hysteresis
// ---------------------------------------------------------------------------
//
// Even a well-smoothed value printed at %.0f churns its last digit forever.
// Rounding to a step fixes that but introduces a new flicker exactly on the
// boundary, so a value must travel a whole step plus a margin before the shown
// figure moves.

static inline float WrQuantise(float shown, float value, float step,
                               bool *has)
{
    if (step <= 0.0f)
        return value;
    if (has && !*has)
    {
        if (has) *has = true;
        return floorf(value / step + 0.5f) * step;
    }
    if (fabsf(value - shown) < step * 0.75f)
        return shown;
    return floorf(value / step + 0.5f) * step;
}

#endif // WR_SMOOTH_H
