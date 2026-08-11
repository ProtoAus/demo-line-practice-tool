// wr_demo.h  --  one .mtv on disk -> one .wrpath on disk.
//
// wr_mtv.cpp knows what a demo file IS. wr_dp.cpp knows how to find a path in
// its decompressed body. This is the thing in between that reads the run's own
// JSON and uses it to answer the three questions the body cannot:
//
//     WHERE THE SPLITS ARE       the checkpoint times, placed onto the path
//     WHERE THE RUN STARTS       as opposed to where the recording does
//     WHETHER TO TRUST ANY OF IT the flags that go into the file
//
// It is `process_one` from the reference, and the split is the reference's own:
// finish_info exists there for exactly this reason, so that --dump-info reports
// what a real extraction would record rather than an approximation of it. Two
// implementations of "is this run flagged" would be one implementation and one
// thing that agrees with itself.
//
// ---------------------------------------------------------------------------
// THE MARKER FINGERPRINT
// ---------------------------------------------------------------------------
//
// The JSON records, for each subsegment, the time it was reached and the
// player's exact velocity at that moment -- but no position. So the splits
// cannot simply be looked up; they have to be FOUND on the extracted path.
//
// Velocity is a 3-vector of full-precision floats, which is a distinctive
// fingerprint: two ticks of a run rarely share one to four decimal places in
// all three components. So the match is on velocity, seeded by time, and the
// matched indices are required to come out in order. One shared index offset
// absorbs the pre-roll, because the tick count is always larger than
// run_time / tick_interval.
//
// Wrong markers are worse than no markers -- a checkpoint drawn at the wrong
// place on the line is a lie about where the split was -- so a failed match
// reports "not anchored" and the caller writes NO markers rather than placing
// them anyway. That is the `mok` flag, and it is why the writer is handed an
// empty marker list rather than a full one with a warning bit.
//
// TWO SEARCHES, NOT ONE, AND THE SECOND IS NOT A REFINEMENT OF THE FIRST
//
// A stitched path -- one recovered from a run that teleports -- is a
// concatenation of legs with unknown time gaps between them, so point index
// stops being proportional to elapsed time. The time-seeded window would then
// look in the wrong place and every marker would miss.
//
// So there are two paths through this, chosen on the segment count. With one
// segment, index is proportional to time and a global offset is fitted by
// sweeping it and taking the cheapest total error -- that is a few hundred
// offsets times a window, and it is the expensive part of this file. With more
// than one, the time seed is abandoned entirely and each marker is searched for
// forward from the previous match, which enforces the ordering requirement by
// construction rather than checking it afterwards.
//
// Both fail safe. What the second one buys is that they fail safe LESS often:
// dropping the markers is a silent loss of the split times, so it is worth
// searching properly.
//
// ---------------------------------------------------------------------------
// PRE-ROLL
// ---------------------------------------------------------------------------
//
// A demo starts recording before the run does. Measured over 500 demo headers,
// ticks * tick_interval exceeds run_time by a median of 2.06 seconds and by as
// much as 4.11 -- the player walking into the start zone while the recorder is
// already going. Nothing in the written file used to say where that ended, so
// every consumer treated point 0 as t = 0 and was about three quarters of a
// second early.
//
// The JSON says so exactly. Each segment carries effectiveStartVelocity: the
// player's velocity at the instant the timer started, as three full-precision
// floats. That is the same kind of fingerprint the split points are matched on.
//
// wr_path.cpp can also back-solve the start from the run's duration, and on a
// complete point stream the two agree to within a twentieth of a second. This
// exists for the streams that are NOT complete -- 39% of the files on this
// machine -- where a tick count proves nothing and a velocity fingerprint still
// works. A standing start (a bhop map, or a hold in the zone) is refused rather
// than guessed at: every pre-roll sample looks like that one.

#ifndef WR_DEMO_H
#define WR_DEMO_H

#include "wr_common.h"
#include "wr_dp.h"
#include "wr_mtv.h"
#include "wr_path.h"

// The same relative tolerance for both fingerprints, and for the same reason:
// a stored velocity is a central difference of positions while the JSON's is
// exact, so they agree closely rather than exactly.
#define WR_DEMO_MARKER_TOL 0.08

#define WR_DEMO_START_MIN_SPEED 40.0    // below this the fingerprint is not distinctive
#define WR_DEMO_START_SEARCH_SECONDS 6.0    // the worst pre-roll measured is 4.11 s

// ---------------------------------------------------------------------------
// The run's own JSON
// ---------------------------------------------------------------------------

// One subsegment worth keeping: it has a velocity to match on and a time to
// look near. The ones that do not -- the start subsegment carries a zero
// velocity -- are dropped by the parser rather than carried and skipped later.
struct WrDemoWanted
{
    int segment;
    int minorNum;
    double timeReached;
    double vel[3];
    double maxOverallSpeed;
};

struct WrDemoJson
{
    bool haveRef;               // trackStats.maxHorizontalSpeed was a number
    double refMaxHoriz;

    int segmentCount;           // 0 is the reference's falsy `segments`
    bool haveStartVel;          // segments[0].effectiveStartVelocity was a 3-list
    double startVel[3];

    WrDemoWanted *wanted;       // malloc'd; WrDemoFreeJson releases it
    int wantedCount;
};

// All-or-nothing, like json.loads: a malformed document leaves every field
// zeroed, which is what the reference's `h["json"] = None` then produces
// downstream -- no reference speed, no markers, no start.
bool WrDemoParseJson(const char *text, size_t len, WrDemoJson *out);
void WrDemoFreeJson(WrDemoJson *j);

// ---------------------------------------------------------------------------
// Placing the splits and the start
// ---------------------------------------------------------------------------

// Writes at most `wantedCount` markers. Returns whether they are trustworthy;
// the caller writes none at all when it says no.
//
// `pathSegments` IS THE DYNAMIC PROGRAM'S SEGMENT COUNT, NOT THE JSON'S. Two
// completely different numbers happen to be called "segments" in this file, and
// passing the wrong one costs a whole search strategy:
//
//   the JSON's    how many legs the RUN has, as Momentum recorded it. It is 1
//                 on a main track and it is what `wanted` was built from.
//   the DP's      how many pieces the extracted PATH came back in, after the
//                 stitcher. 1 on a clean demo, 24 on a fragmented one.
//
// It is the second that decides the search, because the question it answers is
// "can point index still be treated as proportional to elapsed time" -- and a
// path stitched out of 24 legs with gaps between them cannot, whatever the run
// looked like. Handing it the JSON's number silently takes the time-windowed
// branch on exactly the fragmented demos the other branch exists for, which is
// where its window is pointing at the wrong part of the path. It cost a parity
// run to find, on one demo in six thousand, and it looked like a float bug.
bool WrDemoAnchorMarkers(const WrDpPoint *pts, int n, double runTime,
                         const WrDemoWanted *wanted, int wantedCount,
                         int pathSegments,
                         WrPathWriteMarker *out, int *outCount);

// The index of the first point of the RUN. False means "not recovered", which
// is what every file said before this field existed.
bool WrDemoFindStart(const WrDpPoint *pts, int n, double tickInterval,
                     const double startVel[3], int *indexOut);

// ---------------------------------------------------------------------------
// One demo, end to end
// ---------------------------------------------------------------------------

enum WrDemoOutcome
{
    WR_DEMO_OK = 0,
    WR_DEMO_SKIP,               // never a failure; see the note in wr_mtv.h
    WR_DEMO_ERROR,
    WR_DEMO_CANCELLED           // the user pressed Stop; nothing was learned
};

struct WrDemoArgs
{
    const char *outDir;         // <data>\paths; <map>\<stem>.wrpath is appended
    bool verify;                // compute everything, write nothing
    double timeoutSeconds;      // for the message only; the predicate keeps time
    WrDpAbort abort;
    void *abortUser;
    bool keepDetail;            // --dump-chain wants the bit positions kept
};

struct WrDemoResult
{
    WrMtvHeader h;
    WrDpResult dp;

    WrPathWriteMarker *markers;
    int markerCount;
    bool markersOk;

    // The run's own effectiveStartVelocity, copied out of the JSON so that
    // WrDemoFinishInfo can be called on its own by --dump-info without the
    // document still being in memory.
    bool startVelValid;
    double startVel[3];

    unsigned int flags;
    int startIndex;
    bool startOk;
    double preroll;             // -1.0 when the start was not recovered
    bool flagged;
    char whyFlagged[16];        // "" , "speed check" or "coverage"

    long long bytes;            // what the writer wrote, 0 under --verify
    size_t fileBytes, bodyBytes;

    // The reason on ERROR and SKIP. These strings go into
    // paths\<map>\_failed.txt, so they are the reference's words.
    char message[256];
};

WrDemoOutcome WrDemoProcess(const char *path, const WrDemoArgs *a,
                            WrDemoResult *out);
void WrDemoFree(WrDemoResult *r);

// Everything after extraction: the flags, the marker count, the start index and
// the pre-roll. Split out so --dump-info reports what a real run would record.
// Fills in the same fields WrDemoProcess would.
void WrDemoFinishInfo(WrDemoResult *r);

#endif // WR_DEMO_H
