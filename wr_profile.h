// wr_profile.h  --  a run's energy against how far, or how long, it has gone.
//
// WHAT THIS ANSWERS
//
// The readout beside the crosshair says what your energy is now. It cannot say
// whether the surfer you are chasing bled theirs away steadily over a whole
// stage or dumped it all at one ramp -- and those want completely different
// practice. A profile is the whole run at once, so the shape is the answer.
//
// WHY IT IS CACHED WHEN wr_path.cpp DELIBERATELY CACHES NO ENERGY ARRAY
//
// That comment is right and still stands: the LINE reads energy one point at a
// time, at the point nearest the camera, and caching 38 751 floats to serve one
// lookup per frame would go stale the moment sv_gravity moved. A plot reads
// every point at once, every frame it is open. So this caches -- and pays the
// staleness debt explicitly, by recording the gravity it was built for and
// rebuilding when that changes. Nothing else may invalidate it, because nothing
// else is an input: the points never change once loaded.
//
// WHY BUCKETS WITH A MIN AND A MAX, NOT EVERY NTH POINT
//
// A 38 751-point run has to become a few hundred columns of pixels somehow.
// Taking every Nth point is the obvious way and it is wrong. Measured against
// the full-resolution curve across the whole 396-run library on this machine, a
// single sample per bucket hides a MEDIAN of 1324 units of excursion, and tens
// of thousands on the worst runs -- entire ramp exits fall between samples, and
// the plot then says a surfer was smooth exactly where they were not.
//
// Keeping the min and the max of each bucket cannot miss an excursion, because
// an excursion is by definition one of the two. The band it draws is not
// decoration; it is the part a single line throws away.
//
// RELATIVE TO ITS OWN START, ALWAYS
//
// Every curve begins at zero. Two reasons, one of them a correctness one: a
// stored run's points are the player's FEET and your live line is your CAMERA,
// so the two sit 64 units apart in absolute height and comparing them directly
// would be off by that much forever. Subtracting each series' own first point
// cancels the offset exactly. And it is the more useful question anyway -- "how
// much did this run lose from where it started" compares runs of different
// heights, on different stages, without anything having to line up.

#ifndef WR_PROFILE_H
#define WR_PROFILE_H

#include "wr_common.h"

struct WrRun;

// 480: enough that a bucket is under a pixel wide on any panel you would open,
// so the plot is limited by the screen rather than by this. 480 buckets is
// 9.4 KB per run; 256 runs would be 2.4 MB, and only enabled runs are ever
// built.
#define WR_PROFILE_BUCKETS 480

struct WrProfileBucket
{
    float d;            // cumulative distance at the bucket's last point
    float t;            // elapsed seconds there, run's own clock
    float e;            // energy there, relative to the series' first point
    float eMin, eMax;   // over every point in the bucket -- see the header
};

struct WrProfile
{
    WrProfileBucket *b;
    int n;

    float eMin, eMax;   // over the whole series
    float dTotal;       // path length, teleports excluded
    float tTotal;
    float gravity;      // what it was built with; a change rebuilds it
    bool timeUsable;    // false when the run's recovered clock failed its test
    int builtFrom;      // point count at build time; the live series grows
    int builtStart;     // startIndex at build time; the store can reload
    int builtBuckets;   // g_wrProfileBuckets at build time
};

// How many buckets a curve is actually drawn with, up to WR_PROFILE_BUCKETS.
//
// A bucket is a slice of the run by index, so this is the graph's averaging
// window and it scales with the run: 480 buckets on a 60-second run is an eighth
// of a second each. Fewer buckets is a smoother, blunter curve -- which is the
// right trade when the question is "where did this run lose energy" rather than
// "what happened on this ramp".
//
// The array is still allocated at the compile-time maximum; only the used count
// moves, so changing it costs a rebuild and no reallocation.
extern int g_wrProfileBuckets;

// The profile for a loaded run, building it if needed. NULL while it has not
// been built yet -- the caller is expected to ask again next frame rather than
// to stall, since a full-resolution walk of every enabled run in one frame is
// exactly the hitch this avoids. Cheap once built.
const WrProfile *WrProfileFor(WrRun *run);

// How many builds are still owed across the enabled runs, AND the per-frame
// build budget's reset. Call it once at the top of the frame that draws the
// plot, before any WrProfileFor: without it the budget stays at zero and
// nothing new is ever built. The two are one call because they are one
// question -- "how much of this is not ready, and may I do some of it now".
int WrProfilePending(void);

// Your own line, from WrLivePoints. Rebuilt when it has grown, which is every
// frame while recording -- 32 768 points is a fraction of a millisecond, and it
// is only ever asked for while the tab is open.
const WrProfile *WrProfileLive(void);

void WrProfileFree(WrRun *run);
void WrProfileShutdown(void);

// Value at an x, interpolated between buckets. `byTime` picks which axis x is
// on. False when x is outside the series -- which is the point: a run that
// ended at 40 seconds must draw nothing at 60, not a flat line.
bool WrProfileAt(const WrProfile *p, float x, bool byTime, float *e);

#endif // WR_PROFILE_H
