// wr_scale.h  --  one colour range per leg, rather than one for the picture.
//
// WHAT THIS IS FOR
//
// "Scale to what is on" fits the ends of the colour ramp to the runs actually
// enabled, instead of to two constants that were chosen for the top of a board.
// That was one range across everything on screen, and it had a hole in it the
// moment you looked at more than one leg at a time.
//
// Momentum cuts a map into legs -- a main track, some number of stages, some
// number of bonuses -- and they are not comparable. A stage of surf_utopia lives
// four thousand units above the one before it; a bonus is thirty seconds where
// the main track is ninety. Pool their energies into one min and max and the
// range is the union of two disjoint bands, so each leg gets a fraction of the
// ramp and every line on it comes out one flat colour. Which is the exact
// failure auto-scaling exists to fix, reappearing as soon as a second leg is
// enabled.
//
// The old code did not do that. It did something quieter and worse: it noticed
// there was more than one leg and gave up, falling back to pooling everything --
// so turning the feature on and enabling a stage produced no visible change and
// no explanation.
//
// So: a range per leg. Two stages on screen are two ramps, each fitted to its
// own runs, and a line is coloured against the leg it was actually run on.
//
// WHY IT IS ALSO THE RIGHT ANSWER FOR RANK
//
// It already was. wr_render.cpp's on-screen key has carried the line "placed
// within each leg -- a bonus cannot out-place a main run" since rank colouring
// shipped, and test_rank.cpp exists because a 34-second bonus taking first place
// from a 52-second main track is entirely plausible right up until you notice it
// is on the wrong line. Grouping by leg here is the same rule, applied to the
// same set, in one place.
//
// WHAT THE CALLER MUST KNOW
//
// A colour stops meaning one thing across the whole picture. WR_LINE_ENERGY's
// key promises "absolute, so the same colour means the same energy on every
// line", and that promise is FALSE once two legs are scaled separately -- so the
// key has to stop making it whenever more than one leg is on. A mode that
// quietly changes what its own legend says is worse than either behaviour.
//
// static inline in a header, with the rest of this project's pure logic --
// wr_pacing.h, wr_matrixlife.h, wr_smooth.h, wr_budget.h, wr_stress.h. That is
// what lets tests\test_scale.exe link nothing at all.

#ifndef WR_SCALE_H
#define WR_SCALE_H

// How many legs can be scaled separately at once.
//
// Sixteen is past any real map -- surf_utopia is nine, and the enabled set is
// what matters rather than the map. Past it the caller falls back to one pooled
// range for everything, which is exactly the old behaviour and is visibly
// coarse rather than silently wrong. It is not a run limit: a leg that does not
// fit still draws, it simply shares the pooled ends.
#define WR_SCALE_MAX_LEGS 16

struct WrLegScale
{
    unsigned char type, num;    // the leg: trackType, trackNum
    bool used;

    // The range of whatever was accumulated into it. lo > hi means nothing has
    // been, which is what makes "did this leg get any data" a comparison rather
    // than a second flag that can disagree with it.
    float lo, hi;

    int members;                // runs counted into this leg
    int placed;                 // running count, for assigning a place
};

static inline void WrLegScaleReset(WrLegScale *t, int cap)
{
    for (int i = 0; i < cap; i++)
    {
        t[i].type = 0;
        t[i].num = 0;
        t[i].used = false;
        t[i].lo = 1e30f;
        t[i].hi = -1e30f;
        t[i].members = 0;
        t[i].placed = 0;
    }
}

// The slot for a leg, creating it if there is room. -1 when the table is full,
// which the caller must treat as "use the pooled range" rather than as an error.
static inline int WrLegScaleSlot(WrLegScale *t, int cap, unsigned char type,
                                 unsigned char num)
{
    for (int i = 0; i < cap; i++)
        if (t[i].used && t[i].type == type && t[i].num == num)
            return i;
    for (int i = 0; i < cap; i++)
        if (!t[i].used)
        {
            t[i].used = true;
            t[i].type = type;
            t[i].num = num;
            return i;
        }
    return -1;
}

// The slot for a leg if it already has one, without creating it. For the reading
// side, which must not grow the table while walking it.
static inline int WrLegScaleFind(const WrLegScale *t, int cap,
                                 unsigned char type, unsigned char num)
{
    for (int i = 0; i < cap; i++)
        if (t[i].used && t[i].type == type && t[i].num == num)
            return i;
    return -1;
}

static inline void WrLegScaleAdd(WrLegScale *t, int slot, float lo, float hi)
{
    if (slot < 0)
        return;
    if (lo < t[slot].lo) t[slot].lo = lo;
    if (hi > t[slot].hi) t[slot].hi = hi;
}

// Is this leg's range worth using?
//
// The span test is the important half. A leg with one run whose speed never
// varies, or a leg that collected nothing at all, would otherwise hand the
// renderer lo == hi and every point of it would land at one end of the ramp --
// a whole run in a single colour, which reads as "this measurement is broken"
// rather than as "there was nothing to measure". The caller falls back to the
// user's own numbers, which at least mean something.
static inline bool WrLegScaleUsable(const WrLegScale *t, int slot)
{
    if (slot < 0 || !t[slot].used)
        return false;
    return t[slot].lo <= t[slot].hi && (t[slot].hi - t[slot].lo) > 1.0f;
}

// How many legs got any data. What the on-screen key needs, because one leg can
// still print real numbers and two cannot.
static inline int WrLegScaleCount(const WrLegScale *t, int cap)
{
    int n = 0;
    for (int i = 0; i < cap; i++)
        if (t[i].used)
            n++;
    return n;
}

#endif // WR_SCALE_H
