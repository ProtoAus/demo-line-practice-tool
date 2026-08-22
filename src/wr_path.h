// wr_path.h  --  .wrpath loading and the in-memory run store.
//
// .wrpath files are derived from the game's own downloaded .mtv demos: a
// simple, versioned, CRC-checked array of points, and nothing that has to be
// decoded at draw time. Until v0.7.0 they were produced by a Python script that
// shipped beside the DLL; now they are written by src\wr_dp.cpp and the writer
// at the bottom of this file's .cpp, on the pool in src\wr_jobs.cpp.
//
// The split survives the script that forced it, and it is worth saying why the
// file format did not just disappear along with the second process. Extraction
// is seconds of work per demo and drawing is a frame; a run watched today was
// extracted once, weeks ago, possibly on a build that no longer exists. So the
// reader below is deliberately not coupled to the extractor above it -- it
// checks a version and a CRC and otherwise assumes nothing about who wrote the
// bytes, because in general it does not know.
//
// Also holds the live self-recording buffer, which needs no files and no demo
// at all -- it is the quickest way to prove the whole render path works.

#ifndef WR_PATH_H
#define WR_PATH_H

#include "wr_common.h"
#include "wr_phase.h"

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

// The same idea for the LIVE trail, where the samples are frames of a real
// camera rather than the extractor's linked points, so the safe number is
// different -- and it is 400 because that is what wr_energy.cpp calls a
// teleport. It was 512 and nothing agreed with it: a jump of 450 units was a
// teleport to the energy sampler and to the graph, and ordinary movement to the
// recorder, which then drew the straight bar across the map that the recorder's
// own comment says it exists to prevent.
#define WR_LIVE_TELEPORT_UNITS 400.0f

// A dip only counts once the path has actually descended this far and then
// climbed this far again. Without it, every wobble on a flat section becomes a
// label and the line disappears under text.
#define WR_DIP_MIN_DROP 32.0f

// And how far apart two of them have to be, IN SECONDS.
//
// It was 12 points, which is 0.18 s at a 0.015 tick and 0.12 s at 0.01 -- so a
// map recorded at 100 tick got half again as many labels on the same shape of
// line, for no reason anybody chose. That is the exact defect wr_smooth.h was
// written to remove from the live readout ("a velocity baseline of 4 frames...
// behaved like a different instrument depending on the frame rate"), surviving
// on the demo side because a tick felt more like a unit than a frame does. It
// is not: 482 of the 503 demos here are 0.015 and all 21 bhop_futile runs are
// 0.01.
//
// 0.18 is what 12 points meant on the tick the number was chosen at, so nothing
// moves for the runs it was tuned against.
#define WR_DIP_MIN_GAP_SECONDS 0.18f

// Bits 1 and 2 of the format are HAS_ANGLES and IS_EYE_PATH. Nothing has ever
// written either -- the extractor recovers positions, not view angles, and the
// positions it recovers are the entity origin rather than the eye -- so they
// are recorded here and not defined, because a name for a bit nobody sets reads
// as a feature that exists.
#define WRPATH_FLAG_HAS_VELOCITY (1u << 0)
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

// A bounding sphere over 64 consecutive points, so a stretch of path can be
// rejected without being read.
//
// 16 bytes covering 1792 bytes of WrPoint. That ratio is the entire reason this
// array exists: aiming at a line has to consider every drawn run every frame,
// and the arithmetic was never what cost -- the cache lines were. EmitPath
// measures at about 21 ns per chord, which is one miss. Four chunks fit in a
// cache line, so the reject pass streams 112 times less memory than a strided
// walk of the points would.
#define WR_PICK_CHUNK 64

struct WrChunk
{
    Vec3 c;
    float r;
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

// One board: where on the path it happened, and the statistics from wr_phase.h.
//
// The normal is kept as well as the angle because the renderer wants to draw the
// ramp's facing, and re-deriving it at draw time would mean re-finding the two
// velocities it came from.
struct WrBoard
{
    int pointIndex;
    Vec3 normal;
    WrBoardStats s;
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

    Vec3 startPos;              // points[startIndex], i.e. where the RUN begins
    float nearestDist;          // to the camera, refreshed each frame
    int nearestIndex;           // which point that was, for the energy readout

    // Where the run starts, as opposed to where the recording does.
    //
    // A .mtv carries pre-roll. Measured over 500 demo headers on this machine,
    // ticks * tick_interval exceeds run_time by a median of 2.06 seconds -- the
    // player walking into the start zone before the timer starts -- and the
    // extractor writes t = index * tick_interval from index 0. So points[0] is
    // somewhere in the approach, and everything that read it as t = 0 was off by
    // about three quarters of a second: the graph's origin and energy zero, the
    // live energy anchor, and the comparison run's time origin.
    //
    // An OFFSET rather than a re-sliced array, on purpose. breaks, dips, peaks,
    // eff and markers[].pointIndex all index points[], and every one of them
    // stays correct untouched this way.
    //
    // 0 with startTrusted false means "not recovered", which is precisely what
    // every run did before this field existed. Unknown is today's behaviour
    // wearing a label, not a regression.
    int startIndex;
    bool startTrusted;

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
    int effWindow;              // what it was built with; see WrPathRefreshDerived

    // And the PHYSICS it was built with, for the same reason.
    //
    // eff, phase and boards all read g_energy at load: the efficiency ceiling
    // needs gravity, air-accelerate and max-speed, and the air/contact split
    // needs gravity. All four are sliders. So a run loaded at gravity 800 and
    // then looked at with the slider on 600 was coloured against a ceiling that
    // is no longer the one the key on screen describes -- quietly, with nothing
    // to see, which is the failure mode this project keeps finding and keeps
    // giving the same answer to: record what it was built with and rebuild when
    // that changes. wr_profile.h::gravity is the same field for the same reason.
    float builtGravity;
    float builtAirAccel;
    float builtMaxSpeed;

    // Per-point movement phase: WR_PHASE_AIR / RAMP / GROUND / UNKNOWN.
    //
    // In free flight the vertical acceleration is exactly -sv_gravity, so
    // anything else means a surface is pushing back -- and the stored velocities
    // read gravity back to 0.1%, which makes this a clean split rather than a
    // statistical one. See wr_phase.h for the derivation and the measurements.
    //
    // Derived at load like breaks, dips, peaks and eff. Deliberately NOT stored
    // in the .wrpath: the format is frozen and byte-compared, and every one of
    // the files already on disk gains this on the next load with no
    // re-extraction. There is nothing here a position and a velocity do not
    // already contain.
    unsigned char *phase;

    // Where this run boarded a ramp, and what it cost.
    //
    // A board is the transition from air to sustained ramp contact, which is a
    // strictly better signal than the into-plane threshold the surf community's
    // server-side tools use -- that one catches ordinary surfing too, and
    // reproducing it against this library graded 84% of its own events perfect.
    WrBoard *boards;
    int boardCount;

    // Bounds, for aiming at a line. Built once at load; see WrChunk above.
    // boundRadius is 0 when there is nothing to bound, which reads as "cannot
    // be aimed at" rather than as a point at the origin.
    Vec3 boundCentre;
    float boundRadius;
    WrChunk *chunks;
    int chunkCount;

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

    // The same placing, among the runs currently ENABLED on this leg.
    //
    // A second pair rather than rescaling `rank`, because the two answer
    // different questions and both are wanted. `rank` is a fact about the
    // leaderboard -- the Runs tab reports it as one, and a number that changed
    // when you unticked something else would be a lie. `shownRank` is a fact
    // about the picture: with four lines on screen, colour-by-rank should spend
    // its whole ramp on those four rather than on the nine thousand runs they
    // were drawn from.
    //
    // Written by WrRenderRefreshScales, read only by WrRunColour, and 0 when
    // auto-scaling is off -- which reads as "no opinion" and falls back to rank.
    int shownRank;
    int shownOutOf;
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

// Bumped whenever the store changes identity: a new map, or a load finishing.
//
// So that anything derived from the WHOLE store -- the start zones, for one --
// can rebuild lazily off a single integer comparison, without wr_path.cpp having
// to know what those things are or call into them.
unsigned int WrRunStoreGeneration(void);
const char *WrPathLoadedMap(void);
int WrRunEnabledCount(void);

// Was this run extracted from the demo whose basename is `stem`?
//
// NOT strcmp, AND THE REASON IS IN THE FORMAT. srcSha1 is written into a
// forty-BYTE field at 0x9C by WrPathFixedField, which always reserves one byte
// for a terminator -- so a forty-CHARACTER replay hash is stored as thirty-nine
// characters and reads back one short, for every downloaded run in every file
// ever written. The reference does the same thing and tests\parity.ps1 holds the
// port to it byte for byte, so this is the format rather than a bug in the
// writer, and it cannot be widened: player[] begins at 0xC4, immediately after.
//
// It cost a real defect to find. The quick menu matched a leaderboard row's hash
// against this field with _stricmp, which can never be true for a downloaded
// run, so every successful extraction was declared a failure -- the .wrpath was
// on disk and the row said "that demo could not be read". The Runs tab's send,
// local and watch buttons had the same fault for longer and quieter: they build
// "<srcSha1>.mtv" as a filename, and a name one character short simply is not
// there.
//
// So the comparison is a prefix, with a floor. Thirty-nine hex characters is 156
// bits of a SHA-1; two demos colliding on that and differing in the last
// character is not a thing that has to be reasoned about. The floor exists
// because a run recorded by the game itself has a stem like
// "104455274-surf_fiellu-1781797367-main-nrm-60.990", which is far longer than
// the field and truncates in the middle -- and a prefix match with no floor
// would happily call two of those the same run.
bool WrRunIsFrom(const WrRun *run, const char *stem);

// "main", "stage 3", "bonus 1".
//
// Both return a pointer into ONE shared static buffer, so two calls in a single
// expression clobber each other -- copy the result before calling again.
const char *WrTrackName(const WrRun *run);
const char *WrTrackNameOf(int trackType, int trackNum);

// Where a run places among the loaded runs of its OWN leg, 1 being fastest, and
// how many runs that leg has. Ranking across legs would award first place to a
// bonus for beating a main track it was never racing.
//
// Rank 0 means "not placed yet" -- a store still loading, or a run with too few
// points to be one. Callers treat that as unranked rather than as last.
int WrRunRankInTrack(const WrRun *run, int *outOf);

// Elapsed time at one point of a run, on the run's own clock.
//
// ONE function because there were three copies of this arithmetic and two of
// them were wrong. A stored point's `t` is measured from the first RECORDED
// sample, and everything a user sees is measured from the first sample of the
// RUN -- so the pre-roll has to come off before timeScale is applied, or a
// finish label reads runTime * (1 + preroll/span) instead of runTime.
//
// Returns seconds from the run's own start. Meaningless unless timingTrusted,
// which every caller already checks.
float WrRunTimeAt(const WrRun *run, int index);

// Refresh every run's distance-to-camera. Cheap: samples the path rather than
// walking every point, which is plenty to answer "is this run near me".
void WrUpdateNearest(const Vec3 &cam);

// Enable the fastest `count` runs *on the leg you are standing in*, which is
// almost always what "show me the route" means on a staged map.
void WrEnableBestNearby(int count, float radius);

// Drop the pending "turn on the best run near you" that a load arms.
//
// That behaviour is right when nobody has said what they want and wrong the
// moment somebody has: WrEnableBestNearby CLEARS every other run, so a store
// reload after an extraction would wipe a set of ticks and replace it with one
// run of its own choosing. The quick menu re-applies its ticks when the store
// changes and calls this in the same breath, because the auto-enable happens
// LATER in the frame -- it runs from WrUpdateNearest, inside the renderer,
// which is after WrIdleTick -- and would otherwise win.
//
// Idempotent, and does nothing if no load is pending.
void WrPathCancelAutoEnable(void);

// How many points either side the efficiency figure is differenced over, and a
// rebuild of every run's eff[] when it changes.
//
// A setting rather than a constant because it is the averaging window behind the
// efficiency colours, and how wide it wants to be depends on what is being
// looked at. The rebuild is a full pass over every loaded run, so it is called
// when the slider is RELEASED, not while it is being dragged.
extern int g_wrEffWindow;

// Rebuild every loaded run's derived arrays whose inputs have moved -- eff,
// phase and boards. Runs already built with the wanted window and the current
// physics are skipped, so this costs one comparison each when nothing has
// changed and a full pass over every point when something has.
//
// It was WrPathRefreshEfficiency and it watched one input. There are four, and
// three of them are on the HUD settings tab rather than Line settings, which is
// exactly why the staleness was invisible: the slider you moved and the lines
// that went stale were not on the same page.
void WrPathRefreshDerived(void);

// Live self-recording. Feed it the player's FEET each frame, along with the
// exact player velocity when available (camera fallback otherwise) and the run
// clock, so a live point carries the same three things a demo point does.
void WrLiveRecord(const Vec3 &pos, const Vec3 &vel, float elapsed);
void WrLiveClear(void);

// Pause recording without discarding what is already there, so a failed attempt
// is still on the graph when you go and look at it. Set by wr_timer.cpp on a
// restart and let go of when you leave the start zone; WrLiveClear also drops
// it, which is what makes the Clear button and a map change correct for free.
void WrLiveHold(bool on);
bool WrLiveHeld(void);
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

// ---------------------------------------------------------------------------
// Writing one
// ---------------------------------------------------------------------------
//
// The writer lives beside the reader, in wr_path.cpp, and that placement is the
// point: the offsets at the top of that file are the only statement of this
// format that exists now that the reference has stopped shipping, and a format
// with its two halves in different files drifts. tests\test_wrpath.exe writes a
// file and loads it back through the REAL loader, which is a thing nothing did
// before the port -- LoadOne had no test at all.
//
// THE HEADER IS 0x100 BYTES AND MOSTLY ZEROS. Only the fields the reader reads
// are written; the gaps (0x20..0x23 between the run time and the SteamID, and
// 0xFB) stay zero because they are zero in every file ever written and the CRC
// covers them.

struct WrDpPoint;               // wr_dp.h: x/y/z/vx/vy/vz, all double

// The JSON's split points, already matched onto the path. `pointIndex` is an
// index into the points array, which is why the two travel together.
struct WrPathWriteMarker
{
    unsigned int pointIndex;
    unsigned short segment;
    unsigned short minorNum;
    double timeReached;
    float vx, vy, vz;
    float maxSpeed;
};

struct WrPathWriteArgs
{
    const char *outPath;        // its directory is created if it is missing

    float tickInterval;
    double runTime;
    unsigned long long steamid64;
    long long dateMs;

    // Written into fixed-width NUL-padded fields, one byte of which is always
    // the terminator. See WrPathFixedField in the .cpp for the one subtlety --
    // these arrive as raw bytes out of somebody's demo and the reference put
    // them through a UTF-8 decode first.
    const char *map;            // 64 bytes at 0x34
    const char *mapHash;        // 40 at 0x74
    const char *srcSha1;        // 40 at 0x9C -- the demo's basename, despite the name
    const char *player;         // 32 at 0xC4

    unsigned int flags;
    unsigned char gamemode, trackType, trackNum;

    unsigned int startIndex;
    bool startOk;

    const WrDpPoint *points;
    int pointCount;
    const WrPathWriteMarker *markers;
    int markerCount;
};

// Bytes written, or -1 having filled in `err`. Temp file plus MoveFileEx, so a
// reader never sees a partial file and a kill never leaves one.
long long WrPathWrite(const WrPathWriteArgs *a, char *err, int errCap);

// A fixed-width NUL-padded field, filled the way the reference's _fixed() does.
// Exposed for tests\test_wrpath.exe; see the definition for why this is not
// strncpy.
void WrPathFixedField(unsigned char *dst, int size, const char *src);

void WrPathShutdown(void);

#endif // WR_PATH_H
