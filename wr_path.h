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

// 256, not 64, and not 2000.
//
// Measured across a 4095-demo library covering 475 maps: the busiest map has
// 221 demos, the next 141, and only two maps exceed 128. Sixty-four truncated
// several maps outright. Two thousand would be a five-megabyte static array and,
// if ever filled, about 150 MB of point data for maps that do not exist.
//
// 256 clears the largest real map with room and costs 665 KB of static array.
#define WR_MAX_RUNS 256
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

    WrMarker markers[WR_MAX_MARKERS];
    int markerCount;

    float speedMin, speedMax;   // for colour-by-speed
    float pathLength;

    bool enabled;
    unsigned int colour;        // ImGui packed ABGR
};

// Load every .wrpath under wrlines_data\paths\<map>\. Replaces whatever was
// loaded before. Safe to call with an empty/unknown map name (clears the store).
void WrPathLoadMap(const char *map);

// Loads a few of them per frame; call once per frame. Doing all 256 at once
// inside Present would stall the render thread for most of a second.
void WrPathLoadTick(void);
bool WrPathLoading(int *done, int *total);

int WrRunCount(void);
WrRun *WrRunAt(int i);
const char *WrPathLoadedMap(void);
int WrRunEnabledCount(void);

// "main", "stage 3", "bonus 1".
const char *WrTrackName(const WrRun *run);

// Refresh every run's distance-to-camera. Cheap: samples the path rather than
// walking every point, which is plenty to answer "is this run near me".
void WrUpdateNearest(const Vec3 &cam);

// Enable the fastest `count` runs *on the leg you are standing in*, which is
// almost always what "show me the route" means on a staged map.
void WrEnableBestNearby(int count, float radius);

// Live self-recording. Feed it the camera position each frame; it stores a point
// whenever the camera has moved far enough or turned sharply enough.
void WrLiveRecord(const Vec3 &pos);
void WrLiveClear(void);
bool WrLiveEnabled(void);
void WrLiveSetEnabled(bool on);
const WrPoint *WrLivePoints(int *count);

// Which maps have extracted paths on disk, for the manual picker.
void WrScanAvailableMaps(void);
int WrAvailableMapCount(void);
const char *WrAvailableMapAt(int i);
int WrAvailableMapRuns(int i);

void WrPathShutdown(void);

#endif // WR_PATH_H
