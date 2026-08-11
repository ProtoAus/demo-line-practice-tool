// wr_demo.cpp  --  see wr_demo.h.

#include "wr_demo.h"
#include "wr_json.h"
#include "wr_log.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The run's own JSON
// ---------------------------------------------------------------------------

// [x, y, z] and nothing else. The reference tests isinstance(v, list) and
// len(v) == 3, so a two- or four-element array is not a velocity and the
// subsegment carrying it is dropped.
static bool ReadVec3(WrJson *j, double v[3])
{
    if (!WrJsonEnterArray(j))
    {
        WrJsonSkip(j);
        return false;
    }
    int k = 0;
    bool ok = true;
    while (WrJsonNextElement(j))
    {
        bool got = false;
        double d = WrJsonReal(j, 0.0, &got);
        if (k < 3)
        {
            v[k] = d;
            if (!got)
                ok = false;
        }
        k++;
    }
    return ok && k == 3;
}

static bool WantedPush(WrDemoJson *out, const WrDemoWanted *w, int *cap)
{
    if (out->wantedCount == *cap)
    {
        int grown = *cap ? *cap * 2 : 16;
        WrDemoWanted *bigger = (WrDemoWanted *)realloc(
            out->wanted, sizeof(WrDemoWanted) * (size_t)grown);
        if (!bigger)
            return false;
        out->wanted = bigger;
        *cap = grown;
    }
    out->wanted[out->wantedCount++] = *w;
    return true;
}

static void ReadSubsegments(WrJson *j, WrDemoJson *out, int si, int *cap)
{
    if (!WrJsonEnterArray(j))
    {
        WrJsonSkip(j);
        return;
    }
    while (WrJsonNextElement(j))
    {
        WrDemoWanted w;
        memset(&w, 0, sizeof(w));
        w.segment = si;
        bool haveVel = false, haveTime = false;

        if (!WrJsonEnterObject(j))
        {
            WrJsonSkip(j);
            continue;
        }
        char key[64];
        while (WrJsonNextMember(j, key, sizeof(key)))
        {
            if (strcmp(key, "velocityWhenReached") == 0)
            {
                haveVel = ReadVec3(j, w.vel);
            }
            else if (strcmp(key, "timeReached") == 0)
            {
                bool ok = false;
                w.timeReached = WrJsonReal(j, 0.0, &ok);
                haveTime = ok;          // isinstance(t, (int, float))
            }
            else if (strcmp(key, "minorNum") == 0)
            {
                // int(sub.get("minorNum") or 0): absent, null and 0 all give 0.
                bool ok = false;
                double d = WrJsonReal(j, 0.0, &ok);
                w.minorNum = ok ? (int)d : 0;
            }
            else if (strcmp(key, "stats") == 0)
            {
                if (WrJsonEnterObject(j))
                {
                    char k2[64];
                    while (WrJsonNextMember(j, k2, sizeof(k2)))
                    {
                        if (strcmp(k2, "maxOverallSpeed") == 0)
                        {
                            bool ok = false;
                            double d = WrJsonReal(j, 0.0, &ok);
                            w.maxOverallSpeed = ok ? d : 0.0;
                        }
                        else
                        {
                            WrJsonSkip(j);
                        }
                    }
                }
                else
                {
                    WrJsonSkip(j);      // (sub.get("stats") or {})
                }
            }
            else
            {
                WrJsonSkip(j);
            }
        }

        if (!haveVel || !haveTime)
            continue;
        if (w.vel[0] == 0.0 && w.vel[1] == 0.0 && w.vel[2] == 0.0)
            continue;                   // the start subsegment carries no velocity
        if (!WantedPush(out, &w, cap))
            return;
    }
}

bool WrDemoParseJson(const char *text, size_t len, WrDemoJson *out)
{
    memset(out, 0, sizeof(*out));

    // The reference strips the NUL padding before json.loads. The blob's length
    // prefix counts the padding, so without this every document ends in
    // "Extra data".
    while (len > 0 && text[len - 1] == '\0')
        len--;

    WrJson j;
    WrJsonInit(&j, text, len);
    int cap = 0;

    if (WrJsonEnterObject(&j))
    {
        char key[64];
        while (WrJsonNextMember(&j, key, sizeof(key)))
        {
            if (strcmp(key, "trackStats") == 0)
            {
                if (WrJsonEnterObject(&j))
                {
                    char k2[64];
                    while (WrJsonNextMember(&j, k2, sizeof(k2)))
                    {
                        if (strcmp(k2, "maxHorizontalSpeed") == 0)
                        {
                            bool ok = false;
                            double d = WrJsonReal(&j, 0.0, &ok);
                            if (ok)
                            {
                                out->haveRef = true;
                                out->refMaxHoriz = d;
                            }
                        }
                        else
                        {
                            WrJsonSkip(&j);
                        }
                    }
                }
                else
                {
                    WrJsonSkip(&j);
                }
            }
            else if (strcmp(key, "segments") == 0)
            {
                if (WrJsonEnterArray(&j))
                {
                    int si = 0;
                    while (WrJsonNextElement(&j))
                    {
                        if (WrJsonEnterObject(&j))
                        {
                            char k2[64];
                            while (WrJsonNextMember(&j, k2, sizeof(k2)))
                            {
                                if (si == 0 &&
                                    strcmp(k2, "effectiveStartVelocity") == 0)
                                {
                                    out->haveStartVel = ReadVec3(&j,
                                                                 out->startVel);
                                }
                                else if (strcmp(k2, "subsegments") == 0)
                                {
                                    ReadSubsegments(&j, out, si, &cap);
                                }
                                else
                                {
                                    WrJsonSkip(&j);
                                }
                            }
                        }
                        else
                        {
                            WrJsonSkip(&j);
                        }
                        si++;
                        out->segmentCount = si;
                    }
                }
                else
                {
                    WrJsonSkip(&j);     // `js.get("segments") or []`
                }
            }
            else
            {
                WrJsonSkip(&j);
            }
        }
    }

    if (WrJsonFailed(&j))
    {
        // json.loads is all or nothing. Half a document is not a document, and
        // keeping the fields read before the syntax error would give this run a
        // reference speed the reference implementation never saw.
        WrDemoFreeJson(out);
        return false;
    }
    return true;
}

void WrDemoFreeJson(WrDemoJson *j)
{
    if (!j)
        return;
    free(j->wanted);
    memset(j, 0, sizeof(*j));
}

// ---------------------------------------------------------------------------
// Placing the splits
// ---------------------------------------------------------------------------

static double ErrAt(const WrDpPoint *pts, int i, const double tv[3])
{
    const double v[3] = {pts[i].vx, pts[i].vy, pts[i].vz};
    return WrDpDist3(v, tv);
}

// The first index in [lo, hi) attaining the smallest error. First, not any:
// min(range(...), key=...) returns the first, and the tie-break is a
// determinism requirement rather than a detail.
static int ArgMinErr(const WrDpPoint *pts, int lo, int hi, const double tv[3],
                     double *errOut)
{
    int best = lo;
    double bestErr = ErrAt(pts, lo, tv);
    for (int i = lo + 1; i < hi; i++)
    {
        double e = ErrAt(pts, i, tv);
        if (e < bestErr)
        {
            bestErr = e;
            best = i;
        }
    }
    if (errOut)
        *errOut = bestErr;
    return best;
}

static double SpeedOf(const double tv[3])
{
    const double zero[3] = {0.0, 0.0, 0.0};
    double s = WrDpDist3(zero, tv);
    return s != 0.0 ? s : 1.0;          // `... or 1.0`
}

static void FillMarker(WrPathWriteMarker *m, int idx, const WrDemoWanted *w)
{
    m->pointIndex = (unsigned int)idx;
    m->segment = (unsigned short)w->segment;
    m->minorNum = (unsigned short)w->minorNum;
    m->timeReached = w->timeReached;
    m->vx = (float)w->vel[0];
    m->vy = (float)w->vel[1];
    m->vz = (float)w->vel[2];
    m->maxSpeed = (float)w->maxOverallSpeed;
}

bool WrDemoAnchorMarkers(const WrDpPoint *pts, int n, double runTime,
                         const WrDemoWanted *wanted, int wantedCount,
                         int pathSegments,
                         WrPathWriteMarker *out, int *outCount)
{
    *outCount = 0;
    if (n < 8)
        return false;
    // The reference also refuses when the JSON carries no segments at all. It
    // cannot get here in that state: `wanted` is built out of those segments,
    // so wantedCount > 0 already says there was at least one.
    if (wantedCount <= 0 || runTime <= 0.0)
        return false;

    // A stitched path is a concatenation of legs with time gaps between them,
    // so point index is no longer proportional to elapsed time and the
    // time-seeded window below would look in the wrong place. The velocity
    // fingerprint does not depend on that assumption, so search on it alone.
    if (pathSegments > 1)
    {
        int last = -1;
        bool ok = true;
        for (int k = 0; k < wantedCount; k++)
        {
            const WrDemoWanted *w = &wanted[k];
            const double speed = SpeedOf(w->vel);
            const int lo = last + 1;
            if (lo >= n)
            {
                ok = false;
                break;
            }
            // Searching forward from the previous match enforces the ordering
            // requirement by construction rather than checking it afterwards.
            double e = 0.0;
            const int idx = ArgMinErr(pts, lo, n, w->vel, &e);
            if (e / speed > WR_DEMO_MARKER_TOL)
                ok = false;
            last = idx;
            FillMarker(&out[(*outCount)++], idx, w);
        }
        return ok;
    }

    const int window = n / 20 > 4 ? n / 20 : 4;
    const int span = n / 4 > 8 ? n / 4 : 8;
    const int stride = span / 128 > 1 ? span / 128 : 1;

    int bestOff = 0;
    bool haveCost = false;
    double bestCost = 0.0;
    for (int off = -span; off <= span; off += stride)
    {
        double cost = 0.0;
        for (int k = 0; k < wantedCount; k++)
        {
            const WrDemoWanted *w = &wanted[k];
            int g = (int)((double)n * (w->timeReached / runTime)) + off;
            if (g < 0) g = 0;
            if (g > n - 1) g = n - 1;
            const int lo = g - window > 0 ? g - window : 0;
            const int hi = g + window + 1 < n ? g + window + 1 : n;
            double e = 0.0;
            ArgMinErr(pts, lo, hi, w->vel, &e);
            cost += e;
        }
        if (!haveCost || cost < bestCost)
        {
            haveCost = true;
            bestCost = cost;
            bestOff = off;
        }
    }

    int last = -1;
    bool ok = true;
    for (int k = 0; k < wantedCount; k++)
    {
        const WrDemoWanted *w = &wanted[k];
        int g = (int)((double)n * (w->timeReached / runTime)) + bestOff;
        if (g < 0) g = 0;
        if (g > n - 1) g = n - 1;
        const int lo = g - window > 0 ? g - window : 0;
        const int hi = g + window + 1 < n ? g + window + 1 : n;
        double e = 0.0;
        const int idx = ArgMinErr(pts, lo, hi, w->vel, &e);
        const double speed = SpeedOf(w->vel);
        if (e / speed > WR_DEMO_MARKER_TOL || idx <= last)
            ok = false;
        last = idx;
        FillMarker(&out[(*outCount)++], idx, w);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Where the run starts
// ---------------------------------------------------------------------------

bool WrDemoFindStart(const WrDpPoint *pts, int n, double tickInterval,
                     const double startVel[3], int *indexOut)
{
    *indexOut = 0;
    if (n < 8 || tickInterval <= 0.0 || !startVel)
        return false;

    const double zero[3] = {0.0, 0.0, 0.0};
    const double speed = WrDpDist3(zero, startVel);
    if (speed < WR_DEMO_START_MIN_SPEED)
    {
        // A standing start -- a bhop map, or a hold in the zone. Every pre-roll
        // sample looks like this one, so the match would be a coin toss and
        // saying nothing is the honest answer.
        return false;
    }

    // Only the front of the path. Searching the whole of it would let a
    // coincidental velocity match halfway round the map win, and the run start
    // is by definition near the beginning.
    int hi = (int)(WR_DEMO_START_SEARCH_SECONDS / tickInterval) + 1;
    if (hi > n)
        hi = n;

    double bestErr = 0.0;
    const int best = ArgMinErr(pts, 0, hi, startVel, &bestErr);
    if (bestErr / speed > WR_DEMO_MARKER_TOL)
        return false;
    *indexOut = best;
    return true;
}

// ---------------------------------------------------------------------------
// The last few decisions about a run, and the flags they produce
// ---------------------------------------------------------------------------

void WrDemoFinishInfo(WrDemoResult *r)
{
    unsigned int flags = WRPATH_FLAG_HAS_VELOCITY | WRPATH_FLAG_FROM_EXTRACTOR;
    if (r->markersOk)
        flags |= WRPATH_FLAG_MARKERS_OK;
    if (!r->dp.info.confident)
    {
        flags |= WRPATH_FLAG_LOW_CONFIDENCE;
        strcpy_s(r->whyFlagged, sizeof(r->whyFlagged), "speed check");
    }
    // Passing the speed oracle only proves the leg we identified is real. It
    // says nothing about whether we found the whole run, and that is exactly
    // how two main-track runs got written at 8.8% and 3.9% coverage with no
    // warning at all.
    if (r->dp.info.coverage < WR_DP_MIN_COVERAGE_CONFIDENT)
    {
        flags |= WRPATH_FLAG_LOW_CONFIDENCE;
        if (!r->whyFlagged[0])          // setdefault: the first reason wins
            strcpy_s(r->whyFlagged, sizeof(r->whyFlagged), "coverage");
    }
    r->flags = flags;
    r->flagged = (flags & WRPATH_FLAG_LOW_CONFIDENCE) != 0;

    r->startOk = WrDemoFindStart(r->dp.points, r->dp.pointCount,
                                 (double)r->h.tickInterval,
                                 r->startVelValid ? r->startVel : NULL,
                                 &r->startIndex);
    r->preroll = r->startOk
               ? (double)r->startIndex * (double)r->h.tickInterval
               : -1.0;
}

// ---------------------------------------------------------------------------
// process_one
// ---------------------------------------------------------------------------

// The stem is WrFileStem, in wr_common.h. It was a local strrchr copy here
// until the rule it implements -- os.path.splitext's, which is not "cut at the
// last dot" -- turned out both to matter and to be needed identically in
// wr_extract.cpp, where it keys the failure record.

// A monotonic wall clock in seconds, for info.scan_seconds. wr_dp.cpp does not
// read a clock -- it has no Windows headers and nothing else needs one -- so
// the measurement is taken from out here, around exactly the span the reference
// measures: extract_path and nothing else.
static double Seconds(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

WrDemoOutcome WrDemoProcess(const char *path, const WrDemoArgs *a,
                            WrDemoResult *out)
{
    memset(out, 0, sizeof(*out));

    size_t len = 0;
    unsigned char *data = WrMtvReadFile(path, &len, out->message,
                                        (int)sizeof(out->message));
    if (!data)
        return WR_DEMO_ERROR;
    out->fileBytes = len;

    if (!WrMtvParseHeader(data, len, &out->h, out->message,
                          (int)sizeof(out->message)))
    {
        free(data);
        return WR_DEMO_ERROR;
    }

    // A SKIP, never an error. cmd_extract does not write skips to _failed.txt,
    // and recording these would put ~142 permanent entries in every user's
    // failure record that --retry-failed would re-fail forever.
    if (out->h.codec == WR_MTV_CODEC_ZSTD)
    {
        strcpy_s(out->message, sizeof(out->message),
                 "zstd body (pip install zstandard)");
        free(data);
        return WR_DEMO_SKIP;
    }

    // Read before the body is decompressed, because it lives in the same buffer
    // and this is the last thing that needs it.
    WrDemoJson js;
    WrDemoParseJson((const char *)data + out->h.jsonStart, out->h.jsonLen, &js);

    size_t bodyLen = 0;
    unsigned char *body = WrMtvBody(data, len, &out->h, &bodyLen, out->message,
                                    (int)sizeof(out->message));
    free(data);
    if (!body)
    {
        WrDemoFreeJson(&js);
        return WR_DEMO_ERROR;
    }
    out->bodyBytes = bodyLen;

    WrDpArgs dpa;
    memset(&dpa, 0, sizeof(dpa));
    dpa.body = body;
    dpa.bodyLen = bodyLen;
    dpa.tickInterval = (double)out->h.tickInterval;
    dpa.ticks = out->h.ticks;
    dpa.haveRef = js.haveRef;
    dpa.refMaxHoriz = js.refMaxHoriz;
    dpa.abort = a->abort;
    dpa.abortUser = a->abortUser;
    dpa.timeoutSeconds = a->timeoutSeconds;
    dpa.keepDetail = a->keepDetail;

    bool cancelled = false;
    const double t0 = Seconds();
    const bool got = WrDpExtract(&dpa, &out->dp, &cancelled, out->message,
                                 (int)sizeof(out->message));
    out->dp.info.scanSeconds = Seconds() - t0;
    free(body);
    if (!got)
    {
        WrDemoFreeJson(&js);
        return cancelled ? WR_DEMO_CANCELLED : WR_DEMO_ERROR;
    }

    if (js.wantedCount > 0)
    {
        out->markers = (WrPathWriteMarker *)malloc(
            sizeof(WrPathWriteMarker) * (size_t)js.wantedCount);
        if (out->markers)
            // dp.info.segments, NOT js.segmentCount. See the header: the two
            // are different numbers with the same name, and this is the one
            // that says whether index is still proportional to time.
            out->markersOk = WrDemoAnchorMarkers(
                out->dp.points, out->dp.pointCount, out->h.runTime,
                js.wanted, js.wantedCount, out->dp.info.segments,
                out->markers, &out->markerCount);
    }

    out->startVelValid = js.haveStartVel && js.segmentCount > 0;
    out->startVel[0] = js.startVel[0];
    out->startVel[1] = js.startVel[1];
    out->startVel[2] = js.startVel[2];
    WrDemoFinishInfo(out);
    WrDemoFreeJson(&js);

    if (!a->verify)
    {
        char stem[MAX_PATH];
        WrFileStem(path, stem, sizeof(stem));

        char outPath[MAX_PATH];
        _snprintf_s(outPath, sizeof(outPath), _TRUNCATE, "%s\\%s\\%s.wrpath",
                    a->outDir, out->h.map, stem);

        WrPathWriteArgs wa;
        memset(&wa, 0, sizeof(wa));
        wa.outPath = outPath;
        wa.tickInterval = out->h.tickInterval;
        wa.runTime = out->h.runTime;
        wa.steamid64 = out->h.steamid64;
        wa.dateMs = out->h.dateMs;
        wa.map = out->h.map;
        wa.mapHash = out->h.mapHash;
        wa.srcSha1 = stem;
        wa.player = out->h.player;
        wa.flags = out->flags;
        wa.gamemode = (unsigned char)out->h.gamemode;
        wa.trackType = (unsigned char)out->h.trackType;
        wa.trackNum = (unsigned char)out->h.trackNum;
        wa.startIndex = (unsigned int)out->startIndex;
        wa.startOk = out->startOk;
        wa.points = out->dp.points;
        wa.pointCount = out->dp.pointCount;
        // Wrong markers are worse than no markers: an unanchored set is dropped
        // rather than written with a warning bit nobody would look at.
        wa.markers = out->markersOk ? out->markers : NULL;
        wa.markerCount = out->markersOk ? out->markerCount : 0;

        out->bytes = WrPathWrite(&wa, out->message, (int)sizeof(out->message));
        if (out->bytes < 0)
            return WR_DEMO_ERROR;
    }

    out->message[0] = '\0';
    return WR_DEMO_OK;
}

void WrDemoFree(WrDemoResult *r)
{
    if (!r)
        return;
    WrDpFree(&r->dp);
    free(r->markers);
    r->markers = NULL;
    r->markerCount = 0;
}
