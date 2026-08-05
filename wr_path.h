// wr_path.h  --  .wrpath loading and the in-memory run store.
//
// .wrpath files are produced offline by wrpath_extract.py from the game's own
// downloaded .mtv demos. This side never parses .mtv: the DLL only ever reads a
// simple, versioned, CRC-checked array of points.
//
// Also holds the live self-recording buffer, which needs no files and no demo
// at all -- it is the quickest way to prove the whole render path works.

#ifndef WR_PATH_H
#define WR_PATH_H

#include "wr_common.h"

// 1000, and it used to be 256.
//
// The old figure was measured, and the measurement was of the wrong thing. Across
// a 4095-demo library covering 475 maps the busiest map had 221 demos, so 256
// cleared the largest real map with room. But that library was whatever the game
// happened to have downloaded while playing, and the Board tab has since made the
// leaderboard itself the source: you can sort a board, tick the whole filtered
// set and fetch it. A busy map's board runs to the thousands, so the number that
// bounds this is no longer "how many demos does the game keep lying around".
//
// sizeof(WrRun) is 2672 bytes -- most of it the 64 embedded markers -- so the
// static array goes from 668 KB to 2.55 MB, which is nothing in an x64 process.
// The real cost is the per-run heap: points, breaks, dips, peaks and eff, tens of
// KB each, so a genuinely full store is tens of megabytes. That is the price of
// having asked for a thousand runs, and it is paid only if you do.
//
// Still a cap rather than a growable array, because the store is a static that
// several systems index into every frame, and a realloc that moved it would
// invalidate every WrRun* held across a frame boundary -- tag anchors, the
// compare target, the nearest-run cache. A bigger number is a one-line change;
// making it dynamic is not.
#define WR_MAX_RUNS 1000
#define WR_MAX_MARKERS 64
#define WR_LIVE_POINTS 32768

// A step longer than this between consecutive samples is a teleport, not
// movement. Safe because the extractor's DP refuses to link samples more than
// MAX_STEP = 200 units apart, so nothing inside a single leg can reach this.
#define WR_TELEPORT_UNITS 256.0f

// A dip only counts once the path has actually descended this far and then
// climbed this far again. Without it, every wobble on a flat section becomes a
// label and the line disappears under text.
#define WR_DIP_MIN_DROP 32.0f
#define WR_DIP_MIN_GAP 12          // points between successive dips

#define WRPATH_FLAG_MARKERS_OK (1u << 3)
#define WRPATH_FLAG_SELF_RECORDED (1u << 4)
#define WRPATH_FLAG_FROM_EXTRACTOR (1u << 5)
#define WRPATH_FLAG_LOW_CONFIDENCE (1u << 6)

struct WrPoint
{
    Vec3 pos;
    Vec3 vel;
    float t;
};

struct WrMarker
{
    unsigned int pointIndex;
    unsigned short segment;
    unsigned short minorNum;
    double timeReached;
    Vec3 vel;
    float maxSpeed;
};

struct WrRun
{
    char player[40];
    char srcSha1[48];
    char map[72];
    char file[MAX_PATH];

    double runTime;
    float tickInterval;
    unsigned long long steamId;
    long long dateMs;
    unsigned int flags;

    // Which part of the map this run is of. Momentum records a separate demo per
    // stage and per bonus, not just one per map, and on a staged map most of
    // what you have downloaded is stage runs. Without this the run list is
    // actively misleading: the fastest three runs are usually three *different*
    // stages, none of them where you are standing.
    unsigned char gamemode;
    unsigned char trackType;    // 0 main, 1 stage, 2 bonus
    unsigned char trackNum;

    Vec3 startPos;              // first recorded point, i.e. where this leg begins
    float nearestDist;          // to the camera, refreshed each frame
    int nearestIndex;           // which point that was, for the energy readout

    // Where this run's name tag sits, carried between frames. The anchor is the
    // point at the fade distance, and picking it fresh every frame from a
    // strided sample made the tag hop along the line in ~200 unit steps; keeping
    // last frame's answer lets the search stay local, interpolate between
    // points, and smooth.
    int tagIndex;
    Vec3 tagPos;

    WrPoint *points;
    int pointCount;

    // Indices where the path teleports: breaks[j] == k means the step from
    // point k to point k+1 is a jump, not movement, and the two must never be
    // joined by a line.
    //
    // Found once at load, at full resolution. The renderer decimates by index
    // for performance, and a distance test applied to those strided chords has
    // to be loosened in proportion -- at which point it stops being able to see
    // a teleport at all. Recording the breaks up front makes the test exact and
    // independent of however coarsely the path is later drawn.
    int *breaks;
    int breakCount;

    // Bottoms of ramps: indices where the path stops descending and starts
    // climbing. These are the points worth labelling with a speed, because they
    // are where a surf line either kept its momentum or did not.
    int *dips;
    int dipCount;

    // Tops: where the path stops climbing and starts descending. The mirror of
    // dips and found the same way, which is why they are exact here and merely
    // approximate live -- a stored run's vz is a clean central difference of
    // dense samples, and yours is a camera differenced over 40 ms.
    //
    // A bottom says what a line carried THROUGH a ramp; a top says what it
    // bought with it. Reading only one of the two tells you a surfer lost speed
    // without telling you whether they got height for it.
    int *peaks;
    int peakCount;

    // Per-point air-strafing efficiency, as a signed byte over [-1, +1]:
    // dE/dt divided by the most air acceleration could physically add. See
    // wr_stress.h -- in particular for why this is NOT a turn-rate metric.
    // Computed once at load at full resolution, like breaks and dips, so the
    // renderer only ever does a lookup.
    signed char *eff;

    // Whether this run's stored per-point times can be used as a clock.
    //
    // The extractor writes t = index * tick_interval. The RATE of that is right
    // -- checked against demo markers over 115 runs, median slope 1.0000 -- but
    // it only stays right while no ticks are missed, and a teleport join skips
    // an unknown duration. Comparing the last point's time against the run's own
    // recorded duration catches the bad ones: 0.96-1.00x on surf_demise,
    // 0.36x-10.32x on surf_colin_blaster_69000.
    //
    // Deliberately a TEST, not a correction. Rescaling to force the endpoints to
    // match would stretch a clock whose rate is already correct.
    float timeScale;
    bool timingTrusted;

    WrMarker markers[WR_MAX_MARKERS];
    int markerCount;

    float speedMin, speedMax;   // for colour-by-speed
    float pathLength;

    // Energy against distance and time, for the Graphs tab. Built on demand
    // rather than at load -- see wr_profile.h for why this one array is cached
    // when the per-point energy deliberately is not.
    struct WrProfile *profile;

    bool enabled;
    unsigned int colour;        // ImGui packed ABGR

    // Where this run placed on its own leg, and how big that leg is. Computed
    // once when the store settles, not per query.
    //
    // It was a scan of the whole store per call, which was fine at a cap of 256
    // and is not at 1000: the renderer asks for a run's colour once for the
    // line, again for its name tag, again for its ramp numbers, its checkpoints
    // and its comparison ring, and every one of those was another full pass. At
    // 256 drawn out of 1000 loaded that is a quarter of a million comparisons
    // per call site per frame, inside a Present hook.
    //
    // Zero until the store settles, which reads as "unranked" and falls back to
    // the palette colour -- correct during a progressive load, when the store is
    // not yet sorted and any rank would be provisional anyway.
    int rank;                   // 1 is fastest; 0 = not computed yet
    int rankOutOf;
};

// Load every .wrpath under wrlines_data\paths\<map>\. Replaces whatever was
// loaded before. Safe to call with an empty/unknown map name (clears the store).
void WrPathLoadMap(const char *map);

// Loads a few of them per frame; call once per frame. Doing a full store at once
// inside Present would stall the render thread for seconds, not milliseconds.
void WrPathLoadTick(void);
bool WrPathLoading(int *done, int *total);

int WrRunCount(void);
WrRun *WrRunAt(int i);
const char *WrPathLoadedMap(void);
int WrRunEnabledCount(void);

// "main", "stage 3", "bonus 1".
const char *WrTrackName(const WrRun *run);

// Where a run places among the loaded runs of its OWN leg, 1 being fastest, and
// how many runs that leg has. Ranking across legs would award first place to a
// bonus for beating a main track it was never racing.
//
// Rank 0 means "not placed yet" -- a store still loading, or a run with too few
// points to be one. Callers treat that as unranked rather than as last.
int WrRunRankInTrack(const WrRun *run, int *outOf);

// Refresh every run's distance-to-camera. Cheap: samples the path rather than
// walking every point, which is plenty to answer "is this run near me".
void WrUpdateNearest(const Vec3 &cam);

// Enable the fastest `count` runs *on the leg you are standing in*, which is
// almost always what "show me the route" means on a staged map.
void WrEnableBestNearby(int count, float radius);

// Live self-recording. Feed it the player's FEET each frame, along with the
// smoothed velocity and the run clock, so a live point carries the same three
// things a demo point does and can be compared against one directly.
void WrLiveRecord(const Vec3 &pos, const Vec3 &vel, float elapsed);
void WrLiveClear(void);
bool WrLiveEnabled(void);
void WrLiveSetEnabled(bool on);
const WrPoint *WrLivePoints(int *count);

// The live point nearest a world position, within `radius`. NULL if you have not
// been there -- which is exactly what a "how do I compare here" label needs, so
// it can say nothing rather than guess.
const WrPoint *WrLiveNearest(const Vec3 &pos, float radius);

// Which maps have extracted paths on disk, for the manual picker.
void WrScanAvailableMaps(void);
int WrAvailableMapCount(void);
const char *WrAvailableMapAt(int i);
int WrAvailableMapRuns(int i);

void WrPathShutdown(void);

#endif // WR_PATH_H
