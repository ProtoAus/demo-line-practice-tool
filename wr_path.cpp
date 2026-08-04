// wr_path.cpp  --  see wr_path.h.

#include "wr_path.h"
#include "wr_stress.h"
#include "wr_energy.h"
#include "wr_engine.h"
#include "wr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Must match write_wrpath() in wrpath_extract.py exactly.
#define WRPATH_HEADER_BYTES 0x100
#define WRPATH_POINT_BYTES 28
#define WRPATH_MARKER_BYTES 36

static WrRun g_runs[WR_MAX_RUNS];
static int g_runCount = 0;
static char g_loadedMap[72] = {0};

static WrPoint g_live[WR_LIVE_POINTS];
static int g_liveCount = 0;
static bool g_liveOn = true;

// Set when a load finishes, cleared by the first WrUpdateNearest with a live
// camera, which is where the default run selection is actually made.
static bool g_autoEnablePending = false;

// What counts as "on the leg I am standing in" for that default. Matches the
// panel's own default radius: enough to cover one stage of a surf map without
// reaching into the next one.
#define AUTO_ENABLE_RADIUS 4096.0f

// A distinct, readable palette. Gold first so the best run reads as the best
// run without needing a legend.
static const unsigned int kPalette[] = {
    0xFF33CCFF,   // gold      (ABGR)
    0xFFFFCC44,   // cyan
    0xFF66FF66,   // green
    0xFF6666FF,   // red
    0xFFFF66FF,   // magenta
    0xFF44DDFF,   // orange
    0xFFDDDD66,   // teal
    0xFFCCCCCC,   // grey
};

static unsigned int PaletteColour(int i)
{
    return kPalette[i % (int)(sizeof(kPalette) / sizeof(kPalette[0]))];
}

// ---------------------------------------------------------------------------
// CRC32 (zlib polynomial), to match what the Python writer stamps on.
// ---------------------------------------------------------------------------

static unsigned int Crc32(const unsigned char *data, size_t len)
{
    static unsigned int table[256];
    static bool built = false;
    if (!built)
    {
        for (unsigned int i = 0; i < 256; i++)
        {
            unsigned int c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    unsigned int c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

static void FreeRuns(void)
{
    for (int i = 0; i < g_runCount; i++)
    {
        if (g_runs[i].points)
            free(g_runs[i].points);
        if (g_runs[i].breaks)
            free(g_runs[i].breaks);
        if (g_runs[i].dips)
            free(g_runs[i].dips);
        if (g_runs[i].eff)
            free(g_runs[i].eff);
    }
    memset(g_runs, 0, sizeof(g_runs));
    g_runCount = 0;
}

static void ReadFixed(char *dst, int dstLen, const unsigned char *src, int srcLen)
{
    int n = srcLen < dstLen - 1 ? srcLen : dstLen - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    // Fixed-width fields are NUL-padded; make sure we stop at the first one.
    for (int i = 0; i < n; i++)
        if (dst[i] == '\0') { dst[i] = '\0'; break; }
}

// Bottoms of ramps: where the path stops falling and starts climbing.
//
// Detected on the stored velocity's z sign change rather than on position, so a
// single noisy point cannot invent one. The hysteresis is what keeps this
// useful: a surf line spends a lot of time near level, and without requiring a
// real descent followed by a real climb every one of those wobbles would get a
// label until the line vanished under text.
static void FindDips(WrRun *run)
{
    run->dips = NULL;
    run->dipCount = 0;
    if (run->pointCount < 8)
        return;

    // Two passes: count, then fill. Dips are rare (tens per run) so allocating
    // for the worst case would waste far more than the second walk costs.
    for (int pass = 0; pass < 2; pass++)
    {
        int n = 0;
        int last = -WR_DIP_MIN_GAP;
        float peakZ = run->points[0].pos.z;      // highest point since climbing
        float lowZ = run->points[0].pos.z;       // lowest since we started down
        bool falling = false;

        for (int i = 1; i < run->pointCount; i++)
        {
            float z = run->points[i].pos.z;
            float vz = run->points[i].vel.z;

            if (vz < 0.0f)
            {
                if (!falling)
                {
                    falling = true;
                    lowZ = z;
                }
                if (z < lowZ)
                    lowZ = z;
                if (z > peakZ)
                    peakZ = z;
            }
            else if (falling)
            {
                // Turned upward. Was the descent deep enough, and are we far
                // enough from the previous dip to be a separate feature?
                if ((peakZ - lowZ) >= WR_DIP_MIN_DROP && (i - last) >= WR_DIP_MIN_GAP)
                {
                    if (pass == 1 && run->dips)
                        run->dips[n] = i;
                    n++;
                    last = i;
                }
                falling = false;
                peakZ = z;
            }
            else if (z > peakZ)
            {
                peakZ = z;
            }
        }

        if (pass == 0)
        {
            if (n == 0)
                return;
            run->dips = (int *)malloc(sizeof(int) * n);
            if (!run->dips)
                return;
        }
        else
        {
            run->dipCount = n;
        }
    }
}

// Defined below, next to the rest of the per-run analysis.
static void CheckTimes(WrRun *run);
static void FindEfficiency(WrRun *run);

static bool LoadOne(const char *path, WrRun *run)
{
    FILE *f = NULL;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < WRPATH_HEADER_BYTES + 4)
    {
        fclose(f);
        return false;
    }

    unsigned char *buf = (unsigned char *)malloc((size_t)size);
    if (!buf)
    {
        fclose(f);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size)
    {
        free(buf);
        return false;
    }

    if (memcmp(buf, "WRPATH\0\0", 8) != 0)
    {
        WrLogf("[!] %s: bad magic", path);
        free(buf);
        return false;
    }

    unsigned int stored;
    memcpy(&stored, buf + size - 4, 4);
    if (Crc32(buf, (size_t)size - 4) != stored)
    {
        WrLogf("[!] %s: CRC mismatch -- truncated or corrupt, skipping", path);
        free(buf);
        return false;
    }

    unsigned int version, flags, nPts, nMks;
    memcpy(&version, buf + 0x08, 4);
    memcpy(&flags, buf + 0x0C, 4);
    memcpy(&nPts, buf + 0x10, 4);
    memcpy(&nMks, buf + 0x14, 4);
    if (version != 1)
    {
        WrLogf("[!] %s: unsupported version %u", path, version);
        free(buf);
        return false;
    }

    size_t need = (size_t)WRPATH_HEADER_BYTES + (size_t)nPts * WRPATH_POINT_BYTES
                + (size_t)nMks * WRPATH_MARKER_BYTES + 4;
    if (nPts < 2 || need > (size_t)size)
    {
        WrLogf("[!] %s: point/marker counts do not fit the file", path);
        free(buf);
        return false;
    }

    memset(run, 0, sizeof(*run));
    run->flags = flags;
    memcpy(&run->tickInterval, buf + 0x18, 4);
    memcpy(&run->runTime, buf + 0x1C, 8);
    memcpy(&run->steamId, buf + 0x24, 8);
    memcpy(&run->dateMs, buf + 0x2C, 8);
    ReadFixed(run->map, sizeof(run->map), buf + 0x34, 64);
    ReadFixed(run->srcSha1, sizeof(run->srcSha1), buf + 0x9C, 40);
    ReadFixed(run->player, sizeof(run->player), buf + 0xC4, 32);
    run->gamemode = buf[0xF8];
    run->trackType = buf[0xF9];
    run->trackNum = buf[0xFA];
    strcpy_s(run->file, sizeof(run->file), path);

    run->points = (WrPoint *)malloc(sizeof(WrPoint) * nPts);
    if (!run->points)
    {
        free(buf);
        return false;
    }

    const unsigned char *p = buf + WRPATH_HEADER_BYTES;
    int kept = 0;
    for (unsigned int i = 0; i < nPts; i++, p += WRPATH_POINT_BYTES)
    {
        float v[7];
        memcpy(v, p, sizeof(v));
        Vec3 pos = WrVec(v[0], v[1], v[2]);
        if (!WrSaneVec(pos))
            continue;       // never let a bad float reach the renderer
        run->points[kept].pos = pos;
        run->points[kept].vel = WrVec(v[3], v[4], v[5]);
        run->points[kept].t = v[6];
        kept++;
    }
    run->pointCount = kept;

    const unsigned char *m = buf + WRPATH_HEADER_BYTES
                           + (size_t)nPts * WRPATH_POINT_BYTES;
    run->markerCount = 0;
    for (unsigned int i = 0; i < nMks && run->markerCount < WR_MAX_MARKERS;
         i++, m += WRPATH_MARKER_BYTES)
    {
        WrMarker mk;
        memset(&mk, 0, sizeof(mk));
        memcpy(&mk.pointIndex, m + 0, 4);
        memcpy(&mk.segment, m + 4, 2);
        memcpy(&mk.minorNum, m + 6, 2);
        memcpy(&mk.timeReached, m + 8, 8);
        float mv[4];
        memcpy(mv, m + 16, sizeof(mv));
        mk.vel = WrVec(mv[0], mv[1], mv[2]);
        mk.maxSpeed = mv[3];
        if (mk.pointIndex >= (unsigned int)run->pointCount)
            continue;
        run->markers[run->markerCount++] = mk;
    }

    // Precompute what the renderer needs so the hot path stays arithmetic-free.
    run->speedMin = 1e9f;
    run->speedMax = 0.0f;
    run->pathLength = 0.0f;
    for (int i = 0; i < run->pointCount; i++)
    {
        float s = WrLength(run->points[i].vel);
        if (s < run->speedMin) run->speedMin = s;
        if (s > run->speedMax) run->speedMax = s;
        if (i > 0)
            run->pathLength += WrDist(run->points[i - 1].pos, run->points[i].pos);
    }
    if (run->speedMin > run->speedMax)
    {
        run->speedMin = 0.0f;
        run->speedMax = 1.0f;
    }
    if (run->pointCount > 0)
        run->startPos = run->points[0].pos;
    run->nearestDist = -1.0f;
    run->nearestIndex = -1;
    run->tagIndex = -1;

    // Find the teleports. wrpath_extract.py's DP caps the distance between
    // consecutive samples within a leg at MAX_STEP = 200 units, so anything
    // larger than that in a written file is a join between two legs that
    // harvest_segments() stitched together -- i.e. exactly a teleport.
    run->breaks = NULL;
    run->breakCount = 0;
    int nBreaks = 0;
    for (int i = 0; i + 1 < run->pointCount; i++)
        if (WrDistSqr(run->points[i].pos, run->points[i + 1].pos) >
            WR_TELEPORT_UNITS * WR_TELEPORT_UNITS)
            nBreaks++;
    if (nBreaks > 0)
    {
        run->breaks = (int *)malloc(sizeof(int) * nBreaks);
        if (run->breaks)
        {
            for (int i = 0; i + 1 < run->pointCount; i++)
                if (WrDistSqr(run->points[i].pos, run->points[i + 1].pos) >
                    WR_TELEPORT_UNITS * WR_TELEPORT_UNITS)
                    run->breaks[run->breakCount++] = i;
        }
    }

    FindDips(run);
    CheckTimes(run);        // after breaks: a break makes the clock untrustworthy
    FindEfficiency(run);    // after breaks: never differences across a teleport

    // No energy array. It would only ever be read one element at a time -- the
    // point nearest the camera -- and caching it here would go silently stale
    // the moment the gravity setting moved. WrEnergyOf() on the stored velocity
    // costs nothing and is always current.

    free(buf);
    return run->pointCount >= 2;
}

const char *WrTrackName(const WrRun *run)
{
    static char buf[32];
    if (!run)
        return "?";
    switch (run->trackType)
    {
    case 0:  return "main";
    case 1:  _snprintf_s(buf, sizeof(buf), _TRUNCATE, "stage %d", run->trackNum);
             return buf;
    case 2:  _snprintf_s(buf, sizeof(buf), _TRUNCATE, "bonus %d", run->trackNum);
             return buf;
    default: _snprintf_s(buf, sizeof(buf), _TRUNCATE, "t%d/%d",
                         run->trackType, run->trackNum);
             return buf;
    }
}

// The stored per-point time is not a clock, and it has to be made into one.
//
// wrpath_extract.py writes t = index * tick_interval. That is only elapsed time
// if every tick was recovered, and extraction never recovers every tick -- so
// the stored clock runs at the wrong rate, by a different amount in every run.
// Measured against each run's own recorded duration across a real library:
// surf_demise 0.96-1.00x, surf_tensor2 up to 1.87x, surf_colin_blaster_69000
// from 0.36x to 10.32x. A time comparison built on the raw value would have
// been silently wrong by a factor of ten on the map being practised.
//
// Rescaling so the last point lands on the recorded duration makes both ends
// exact. The middle is only as good as the assumption that the missing ticks
// are spread evenly, which is why anything far from 1.0 is marked untrusted
// rather than quietly used.
#define TIME_SCALE_TRUST 0.10f      // how far from 1.0 is still believable

// Decide whether this run's stored times can be used as a clock. A TEST, not a
// correction -- see the comment on timeScale in wr_path.h. Rescaling would
// stretch a clock whose rate is already right.
static void CheckTimes(WrRun *run)
{
    run->timeScale = 1.0f;
    run->timingTrusted = false;
    if (run->pointCount < 2 || run->runTime <= 0.0f)
        return;

    float last = run->points[run->pointCount - 1].t;
    if (!(last > 1e-3f))
        return;

    run->timeScale = (float)run->runTime / last;
    float off = run->timeScale - 1.0f;
    if (off < 0.0f) off = -off;

    // A teleport join skips an unknown duration, so any run with a break has a
    // clock with an unknown gap in it whatever the endpoints say.
    run->timingTrusted = (off <= TIME_SCALE_TRUST) &&
                         run->breakCount == 0 &&
                         !(run->flags & WRPATH_FLAG_LOW_CONFIDENCE);
}

// Per-point air-strafing efficiency: how much of the energy air acceleration
// could physically have added was actually added. See wr_stress.h, especially
// for why this is not a turn-rate metric.
#define EFF_WINDOW 4                // points either side; ~120 ms at 66 tick

static void FindEfficiency(WrRun *run)
{
    run->eff = NULL;
    if (run->pointCount < EFF_WINDOW * 2 + 1)
        return;
    run->eff = (signed char *)malloc((size_t)run->pointCount);
    if (!run->eff)
        return;

    // NO DATA, not neutral. This was calloc'd, so the EFF_WINDOW points at each
    // end and every window spanning a teleport read as "eta 0" -- the same value
    // as free flight. A gap in the measurement and a player coasting are not the
    // same thing and must not draw the same.
    memset(run->eff, WR_ETA_NO_DATA, (size_t)run->pointCount);

    float ceiling = WrAirPowerCeiling(g_energy.gravity, run->tickInterval);
    float dt = run->tickInterval * (float)(EFF_WINDOW * 2);
    if (!(dt > 1e-5f))
        return;

    // A centred difference, so the figure belongs to the point it is drawn at
    // rather than trailing it by half a window.
    for (int i = EFF_WINDOW; i + EFF_WINDOW < run->pointCount; i++)
    {
        int a = i - EFF_WINDOW, b = i + EFF_WINDOW;

        // Never across a teleport: the join skips an unknown duration, and the
        // height either side of it is unrelated.
        bool spans = false;
        for (int k = 0; k < run->breakCount; k++)
            if (run->breaks[k] >= a && run->breaks[k] < b)
            {
                spans = true;
                break;
            }
        if (spans)
            continue;

        float ea = WrEnergyOf(run->points[a].pos, run->points[a].vel);
        float eb = WrEnergyOf(run->points[b].pos, run->points[b].vel);
        float power = (eb - ea) / dt;

        // A booster stays no-data. Only the GAIN side is rejected: losing faster
        // than the ceiling is a ramp entry or a wall, which is 18.8% of all
        // samples and 94.6% of all energy lost, and is the thing worth seeing.
        if (WrEtaIsNoData(power, ceiling))
            continue;
        run->eff[i] = WrEtaToByte(WrEfficiency(power, ceiling));
    }
}

// Distance from the camera to the nearest point of each run.
//
// Sampled, not exhaustive: 64 evenly spaced points is more than enough to answer
// "is this run anywhere near me", and it keeps this at a few thousand distance
// tests per frame regardless of how many runs are loaded.
// How many points the refine pass may examine, whatever the run's length.
//
// The coarse pass samples 64 points and then refined the whole bracket around
// the winner -- which is pointCount/64 wide, so on a 38 751-point run that was
// 1211 extra distance tests. Sixty-four of them find the minimum of a smooth
// path to well inside a unit; the rest were spent proving it.
#define REFINE_BUDGET 64

// How many disabled runs to re-measure per frame.
//
// This used to run over every loaded run, every frame, enabled or not, and the
// comment claimed 64 samples per run. With the refine bracket it was closer to
// 1275 for a long run, so 256 loaded runs cost roughly 326 000 distance tests
// per frame to keep a column up to date that nobody is reading mid-surf.
//
// Enabled runs still update every frame -- the energy and time comparisons read
// nearestIndex and must be exact. The rest take turns.
#define NEAREST_PER_FRAME 4

static void MeasureNearest(WrRun *r, const Vec3 &cam)
{
    if (r->pointCount < 2)
    {
        r->nearestDist = -1.0f;
        r->nearestIndex = -1;
        return;
    }
    int step = r->pointCount / 64;
    if (step < 1)
        step = 1;
    float best = 1e18f;
    int bestIdx = 0;
    for (int p = 0; p < r->pointCount; p += step)
    {
        float d = WrDistSqr(r->points[p].pos, cam);
        if (d < best)
        {
            best = d;
            bestIdx = p;
        }
    }
    // Refine within the sampled bracket, so the energy read off this index is
    // the run's energy where you actually are rather than up to 64 points away.
    int rstep = (step * 2 + REFINE_BUDGET - 1) / REFINE_BUDGET;
    if (rstep < 1)
        rstep = 1;
    int lo = bestIdx - step, hi = bestIdx + step;
    if (lo < 0) lo = 0;
    if (hi > r->pointCount - 1) hi = r->pointCount - 1;
    for (int p = lo; p <= hi; p += rstep)
    {
        float d = WrDistSqr(r->points[p].pos, cam);
        if (d < best)
        {
            best = d;
            bestIdx = p;
        }
    }
    r->nearestDist = sqrtf(best);
    r->nearestIndex = bestIdx;
}

void WrUpdateNearest(const Vec3 &cam)
{
    static int cursor = 0;

    for (int i = 0; i < g_runCount; i++)
        if (g_runs[i].enabled)
            MeasureNearest(&g_runs[i], cam);

    for (int n = 0; n < NEAREST_PER_FRAME && g_runCount > 0; n++)
    {
        cursor = (cursor + 1) % g_runCount;
        WrRun *r = &g_runs[cursor];
        if (!r->enabled)
            MeasureNearest(r, cam);
    }

    // The first time distances are known after a load, turn on the fastest run
    // that actually comes near you rather than the fastest run in the file list.
    //
    // Those are usually not the same run. Momentum records a separate demo per
    // stage, so on a staged map the fastest recorded time is generally a single
    // short stage somewhere else entirely -- and enabling it puts a line in the
    // map that you cannot see from where you spawned, which reads exactly like
    // "I changed map and no lines showed up".
    //
    // Deferred to here rather than done at load time because it needs a live
    // camera, and during a level load the last known camera still belongs to the
    // previous map.
    if (g_autoEnablePending && g_runCount > 0)
    {
        g_autoEnablePending = false;
        // This one frame measures everything: picking the best run near you is
        // exactly the decision that needs every run's distance at once, and the
        // round-robin above has only touched a handful of them so far.
        for (int i = 0; i < g_runCount; i++)
            MeasureNearest(&g_runs[i], cam);
        WrEnableBestNearby(1, AUTO_ENABLE_RADIUS);
        int on = 0;
        for (int i = 0; i < g_runCount; i++)
            if (g_runs[i].enabled)
                on++;
        if (on == 0)
        {
            g_runs[0].enabled = true;   // nothing nearby: the overall best it is
            WrLogf("no run passes within %.0f units of you; enabled the fastest "
                   "one instead (\"%s\", %s)", AUTO_ENABLE_RADIUS,
                   g_runs[0].player, WrTrackName(&g_runs[0]));
        }
    }
}

void WrEnableBestNearby(int count, float radius)
{
    // Runs are already sorted fastest-first, so the first `count` within range
    // are the fastest ones covering where we are standing.
    int on = 0;
    for (int i = 0; i < g_runCount; i++)
    {
        WrRun *r = &g_runs[i];
        // Not `near`: windows.h still defines that as a macro.
        bool inRange = (r->nearestDist >= 0.0f && r->nearestDist <= radius);
        r->enabled = (inRange && on < count);
        if (r->enabled)
            on++;
    }
}

static int CompareByTime(const void *a, const void *b)
{
    const WrRun *ra = (const WrRun *)a;
    const WrRun *rb = (const WrRun *)b;
    if (ra->runTime < rb->runTime) return -1;
    if (ra->runTime > rb->runTime) return 1;
    return 0;
}

// Loading is spread over frames rather than done in one go.
//
// WrPathLoadMap runs inside Present, and a .wrpath costs a few milliseconds to
// read, CRC, and scan for teleports and dips. With 64 runs that was a barely
// noticeable hitch; the busiest map on disk has 125 demos, and at 256 the
// one-shot version would stall the render thread for the better part of a
// second every time you loaded a map.
//
// So the file list is collected up front -- cheap, one directory walk -- and the
// files themselves are loaded a few per frame. The run store is usable the whole
// time; the list simply fills in over the next second.
#define LOAD_PER_FRAME 4

static char (*g_pending)[MAX_PATH] = NULL;
static int g_pendingCount = 0;
static int g_pendingNext = 0;
static int g_pendingFailed = 0;

static void FinishLoad(void)
{
    qsort(g_runs, (size_t)g_runCount, sizeof(WrRun), CompareByTime);
    for (int i = 0; i < g_runCount; i++)
    {
        g_runs[i].colour = PaletteColour(i);
        g_runs[i].enabled = (i == 0);       // provisional; see WrUpdateNearest
    }
    g_autoEnablePending = true;

    WrLogf("loaded %d run%s for \"%s\"%s", g_runCount, g_runCount == 1 ? "" : "s",
           g_loadedMap, g_pendingFailed ? " (some files rejected)" : "");
    if (g_pendingFailed)
        WrLogf("[!] %d .wrpath file%s rejected -- see lines above", g_pendingFailed,
               g_pendingFailed == 1 ? "" : "s");

    free(g_pending);
    g_pending = NULL;
    g_pendingCount = g_pendingNext = g_pendingFailed = 0;
}

void WrPathLoadTick(void)
{
    if (!g_pending)
        return;
    for (int n = 0; n < LOAD_PER_FRAME && g_pendingNext < g_pendingCount; n++)
    {
        if (g_runCount >= WR_MAX_RUNS)
        {
            WrLogf("[!] %s has more than %d runs; the rest are ignored",
                   g_loadedMap, WR_MAX_RUNS);
            g_pendingNext = g_pendingCount;
            break;
        }
        if (LoadOne(g_pending[g_pendingNext], &g_runs[g_runCount]))
            g_runCount++;
        else
            g_pendingFailed++;
        g_pendingNext++;
    }
    if (g_pendingNext >= g_pendingCount)
        FinishLoad();
}

bool WrPathLoading(int *done, int *total)
{
    if (done) *done = g_pendingNext;
    if (total) *total = g_pendingCount;
    return g_pending != NULL;
}

void WrPathLoadMap(const char *map)
{
    FreeRuns();
    free(g_pending);
    g_pending = NULL;
    g_pendingCount = g_pendingNext = g_pendingFailed = 0;

    g_loadedMap[0] = '\0';
    if (!map || !*map)
        return;
    strcpy_s(g_loadedMap, sizeof(g_loadedMap), map);

    char pattern[MAX_PATH];
    char dir[MAX_PATH];
    _snprintf_s(dir, sizeof(dir), _TRUNCATE, "paths\\%s", map);
    const char *base = WrDataPath(dir);
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*.wrpath", base);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        WrLogf("no cached paths for \"%s\" (looked in %s)", map, base);
        return;
    }

    g_pending = (char (*)[MAX_PATH])malloc(sizeof(*g_pending) * WR_MAX_RUNS);
    if (!g_pending)
    {
        FindClose(h);
        return;
    }

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (g_pendingCount >= WR_MAX_RUNS)
            break;
        _snprintf_s(g_pending[g_pendingCount], MAX_PATH, _TRUNCATE, "%s\\%s",
                    base, fd.cFileName);
        g_pendingCount++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if (g_pendingCount == 0)
        FinishLoad();
}

int WrRunCount(void) { return g_runCount; }
WrRun *WrRunAt(int i) { return (i >= 0 && i < g_runCount) ? &g_runs[i] : NULL; }
const char *WrPathLoadedMap(void) { return g_loadedMap; }

int WrRunEnabledCount(void)
{
    int n = 0;
    for (int i = 0; i < g_runCount; i++)
        if (g_runs[i].enabled)
            n++;
    return n;
}

void WrPathShutdown(void) { FreeRuns(); }

// ---------------------------------------------------------------------------
// Available maps (whatever the extractor has produced)
// ---------------------------------------------------------------------------

#define WR_MAX_AVAIL_MAPS 256

static char g_availMap[WR_MAX_AVAIL_MAPS][72];
static int g_availRuns[WR_MAX_AVAIL_MAPS];
static int g_availCount = 0;

void WrScanAvailableMaps(void)
{
    g_availCount = 0;

    char pattern[MAX_PATH];
    const char *base = WrDataPath("paths");
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*", base);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (fd.cFileName[0] == '.')
            continue;
        if (g_availCount >= WR_MAX_AVAIL_MAPS)
            break;

        char sub[MAX_PATH];
        _snprintf_s(sub, sizeof(sub), _TRUNCATE, "%s\\%s\\*.wrpath", base,
                    fd.cFileName);
        int n = 0;
        WIN32_FIND_DATAA f2;
        HANDLE h2 = FindFirstFileA(sub, &f2);
        if (h2 != INVALID_HANDLE_VALUE)
        {
            do { n++; } while (FindNextFileA(h2, &f2));
            FindClose(h2);
        }
        if (n == 0)
            continue;

        strcpy_s(g_availMap[g_availCount], sizeof(g_availMap[0]), fd.cFileName);
        g_availRuns[g_availCount] = n;
        g_availCount++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    WrLogf("%d map%s have extracted paths", g_availCount,
           g_availCount == 1 ? "" : "s");
}

int WrAvailableMapCount(void) { return g_availCount; }

const char *WrAvailableMapAt(int i)
{
    return (i >= 0 && i < g_availCount) ? g_availMap[i] : "";
}

int WrAvailableMapRuns(int i)
{
    return (i >= 0 && i < g_availCount) ? g_availRuns[i] : 0;
}

// ---------------------------------------------------------------------------
// Live self-recording
// ---------------------------------------------------------------------------

bool WrLiveEnabled(void) { return g_liveOn; }
void WrLiveSetEnabled(bool on) { g_liveOn = on; }
void WrLiveClear(void) { g_liveCount = 0; }

const WrPoint *WrLivePoints(int *count)
{
    if (count)
        *count = g_liveCount;
    return g_live;
}

// Linear, but strided coarse-then-refine like MeasureNearest, because this is
// asked once per drawn label rather than once per frame and the buffer holds
// 32768 points.
const WrPoint *WrLiveNearest(const Vec3 &pos, float radius)
{
    if (g_liveCount < 1)
        return NULL;

    int step = g_liveCount / 256;
    if (step < 1) step = 1;

    int best = -1;
    float bestSqr = radius * radius;
    for (int i = 0; i < g_liveCount; i += step)
    {
        float d = WrDistSqr(g_live[i].pos, pos);
        if (d < bestSqr) { bestSqr = d; best = i; }
    }
    if (best < 0)
        return NULL;

    int lo = best - step, hi = best + step;
    if (lo < 0) lo = 0;
    if (hi >= g_liveCount) hi = g_liveCount - 1;
    for (int i = lo; i <= hi; i++)
    {
        float d = WrDistSqr(g_live[i].pos, pos);
        if (d < bestSqr) { bestSqr = d; best = i; }
    }
    return &g_live[best];
}

// The velocity and the clock are passed in rather than derived here.
//
// This used to store the raw position DELTA in `vel` and cumulative DISTANCE in
// `t`, neither of which is what the field names say, and that made your own line
// the only one that could not be asked "how fast was I here, and when?". The
// energy sampler has already computed a smoothed velocity from the same camera
// this is being fed, and the timer already has the elapsed run time, so both are
// free -- and it is what lets a label on somebody else's line say how you
// compare at that point.
void WrLiveRecord(const Vec3 &pos, const Vec3 &vel, float elapsed)
{
    if (!g_liveOn || !WrSaneVec(pos))
        return;

    Vec3 v = WrSaneVec(vel) ? vel : WrVec(0.0f, 0.0f, 0.0f);

    if (g_liveCount == 0)
    {
        g_live[0].pos = pos;
        g_live[0].vel = v;
        g_live[0].t = elapsed;
        g_liveCount = 1;
        return;
    }

    WrPoint *last = &g_live[g_liveCount - 1];
    float moved = WrDist(last->pos, pos);

    // A teleport (savestate load, stage restart) should break the line rather
    // than draw a straight bar across the map.
    if (moved > 512.0f)
    {
        g_liveCount = 0;
        g_live[0].pos = pos;
        g_live[0].vel = v;
        g_live[0].t = elapsed;
        g_liveCount = 1;
        return;
    }

    if (moved < 2.0f)
        return;

    if (g_liveCount >= WR_LIVE_POINTS)
    {
        // Drop the oldest half rather than stopping dead, so a long session
        // keeps showing the recent path.
        int keep = WR_LIVE_POINTS / 2;
        memmove(g_live, g_live + (WR_LIVE_POINTS - keep), sizeof(WrPoint) * keep);
        g_liveCount = keep;
        last = &g_live[g_liveCount - 1];
    }

    g_live[g_liveCount].pos = pos;
    g_live[g_liveCount].vel = v;
    g_live[g_liveCount].t = elapsed;
    g_liveCount++;
}
