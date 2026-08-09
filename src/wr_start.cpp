// wr_start.cpp  --  see wr_start.h for what this can and cannot know.

#include "wr_start.h"
#include "wr_path.h"
#include "wr_energy.h"
#include "wr_engine.h"
#include "wr_log.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

WrStartSettings g_start;

// The medoid is quadratic in the members, so it gets a ceiling. The store is
// sorted fastest-first, so the first 128 of a leg are also its best-recovered
// runs; past that the answer stops moving and only the cost does.
#define MEDOID_CAP 128

// A member further than this from the medoid is not a start, it is a recovery
// that went wrong. Dropped, and the medoid recomputed on what is left.
#define OUTLIER_UNITS 1024.0f

// The arming radius, clamped. The ceiling is deliberately near RESTART_UNITS
// (384) in wr_energy.cpp: that is the distance at which a teleport counts as
// landing back at the anchor, and if the two numbers drift apart the restart
// signal and this one start disagreeing about the same piece of floor.
#define RADIUS_MIN 128.0f
#define RADIUS_MAX 512.0f

// Debounced in seconds, like every other state in this tool, so it behaves the
// same at 60 fps and at 300.
#define DWELL_SECONDS 0.20f
#define ARM_SECONDS 0.20f
#define LOCKOUT_SECONDS 1.0f
#define AWAY_SECONDS 2.0f

// After a teleport or a map change, nothing may fire until the velocity filter
// has converged -- otherwise the first frame after a respawn has a garbage speed
// and can arm or fire on it.
#define SETTLE_SECONDS 1.0f

// Below this the mean start heading is not a direction, and the plane it would
// define is noise. Falls back to leaving the cylinder.
#define OUTDIR_MIN_SPEED 60.0f

static WrStartZone g_zones[WR_MAX_START_ZONES];
static int g_zoneCount = 0;
static unsigned int g_builtFrom = 0;

static WrStartState g_state = WR_START_AWAY;
static int g_inZone = -1;           // index into g_zones, or -1
static float g_dwell = 0.0f;
static float g_armFor = 0.0f;
static float g_awayFor = 0.0f;
static float g_lockout = 0.0f;
static float g_settle = 0.0f;
static float g_sPrev = 0.0f;        // last frame's signed distance along outDir
static bool g_haveS = false;
static bool g_crossed = false;
static const WrStartZone *g_crossedZone = NULL;
static float g_since = -1.0f;
static char g_why[192] = {0};

// ---------------------------------------------------------------------------
// Building the zones
// ---------------------------------------------------------------------------

static float HorizDistSqr(const Vec3 &a, const Vec3 &b)
{
    float dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

// p90 of a set of distances, through a fixed histogram.
//
// No sort, no scratch array, no allocation -- and the answer only has to be good
// enough to pick a radius that is then clamped to a 384-unit window anyway.
static float P90(const float *d, int n, float bucketWidth, int buckets)
{
    if (n <= 0)
        return 0.0f;
    int hist[16];
    if (buckets > 16)
        buckets = 16;
    memset(hist, 0, sizeof(hist));
    for (int i = 0; i < n; i++)
    {
        int b = (int)(d[i] / bucketWidth);
        if (b < 0) b = 0;
        if (b >= buckets) b = buckets - 1;
        hist[b]++;
    }
    int want = (n * 9) / 10;
    int seen = 0;
    for (int b = 0; b < buckets; b++)
    {
        seen += hist[b];
        if (seen > want)
            return (float)(b + 1) * bucketWidth;
    }
    return (float)buckets * bucketWidth;
}

static void BuildZones(void)
{
    g_zoneCount = 0;

    // One pass per leg. Quadratic in the legs, which are at most a couple of
    // dozen, and the inner work is where the cost is.
    for (int i = 0; i < WrRunCount() && g_zoneCount < WR_MAX_START_ZONES; i++)
    {
        const WrRun *seed = WrRunAt(i);
        if (!seed || seed->pointCount < 2)
            continue;

        bool already = false;
        for (int z = 0; z < g_zoneCount; z++)
            if (g_zones[z].trackType == seed->trackType &&
                g_zones[z].trackNum == seed->trackNum)
                already = true;
        if (already)
            continue;

        // Gather this leg's start positions. Trusted starts only, if there are
        // any: points[0] is about three quarters of a second of walking in
        // earlier, which on a start pad is tens of units rather than thousands
        // -- close enough to be worth using when nothing better exists, and not
        // close enough to mix with the good ones.
        Vec3 pos[MEDOID_CAP];
        Vec3 vel[MEDOID_CAP];
        int n = 0, members = 0, trusted = 0;
        bool anyTrusted = false;

        for (int pass = 0; pass < 2 && n == 0; pass++)
        {
            bool trustedOnly = (pass == 0);
            n = 0;
            // The whole store, every pass. The cap bounds what goes into the
            // ARRAY, not what gets counted -- capping the loop made a leg of
            // 300 runs report "128 of 128" in the panel and in the log, which
            // is a number that exists to say how much evidence there is.
            for (int k = 0; k < WrRunCount(); k++)
            {
                const WrRun *r = WrRunAt(k);
                if (!r || r->pointCount < 2)
                    continue;
                if (r->trackType != seed->trackType || r->trackNum != seed->trackNum)
                    continue;
                if (pass == 0)
                {
                    members++;
                    if (r->startTrusted)
                        trusted++;
                }
                if (trustedOnly && !r->startTrusted)
                    continue;
                if (n >= MEDOID_CAP)
                    continue;
                int at = r->startTrusted ? r->startIndex : 0;
                if (at < 0 || at >= r->pointCount)
                    at = 0;
                pos[n] = r->points[at].pos;
                vel[n] = r->points[at].vel;
                n++;
            }
            if (pass == 0 && n > 0)
                anyTrusted = true;
        }
        if (n == 0)
            continue;

        // Medoid, twice: once over everything, then again with the far outliers
        // dropped. One badly recovered start is enough to matter and this is the
        // cheapest robust estimator that needs no scratch space.
        int best = 0;
        for (int round = 0; round < 2; round++)
        {
            float bestCost = -1.0f;
            for (int a = 0; a < n; a++)
            {
                float cost = 0.0f;
                for (int b = 0; b < n; b++)
                    cost += WrDist(pos[a], pos[b]);
                if (bestCost < 0.0f || cost < bestCost)
                {
                    bestCost = cost;
                    best = a;
                }
            }
            if (round == 1)
                break;
            int kept = 0;
            for (int a = 0; a < n; a++)
                if (WrDist(pos[a], pos[best]) <= OUTLIER_UNITS)
                {
                    pos[kept] = pos[a];
                    vel[kept] = vel[a];
                    kept++;
                }
            if (kept == n || kept == 0)
                break;          // nothing dropped, or everything -- keep as is
            n = kept;
        }

        WrStartZone *z = &g_zones[g_zoneCount];
        memset(z, 0, sizeof(*z));
        z->trackType = seed->trackType;
        z->trackNum = seed->trackNum;
        z->centre = pos[best];
        z->members = members;
        z->trusted = trusted;
        z->approx = !anyTrusted;

        // The way out: the mean heading at t = 0, horizontal.
        //
        // Horizontal on purpose. A surf start is a drop-in, so the full velocity
        // points steeply down and a plane with that normal would count falling
        // as leaving. What the trigger is about is crossing the threshold, and
        // that is a thing you do in the xy plane.
        Vec3 sum = WrVec(0.0f, 0.0f, 0.0f);
        for (int a = 0; a < n; a++)
        {
            Vec3 h = WrVec(vel[a].x, vel[a].y, 0.0f);
            float len = WrLength(h);
            if (len > 1.0f)
                sum = WrAdd(sum, WrScale(h, 1.0f / len));
        }
        float sumLen = WrLength(sum);
        if (sumLen > 0.35f * (float)n)      // the headings actually agree
            z->outDir = WrScale(sum, 1.0f / sumLen);
        else
            z->outDir = WrVec(0.0f, 0.0f, 0.0f);

        // Spread, radius, and the floor band.
        float dist[MEDOID_CAP];
        float along[MEDOID_CAP];
        float zLo = z->centre.z, zHi = z->centre.z;
        for (int a = 0; a < n; a++)
        {
            dist[a] = sqrtf(HorizDistSqr(pos[a], z->centre));
            Vec3 d = WrSub(pos[a], z->centre);
            float s = WrDot(d, z->outDir);
            along[a] = s < 0.0f ? -s : s;
            if (pos[a].z < zLo) zLo = pos[a].z;
            if (pos[a].z > zHi) zHi = pos[a].z;
        }
        z->spread = P90(dist, n, 64.0f, 16);
        z->alongSpread = P90(along, n, 16.0f, 16);

        // Stored UNSCALED. The "circle size" slider is applied by ZoneRadius
        // below, at the moment the radius is used, because baking it in here
        // meant the slider did nothing until the store happened to reload --
        // which on a map you are already standing in is never.
        float want = z->spread * 1.5f + 64.0f;
        z->radiusCapped = (want > RADIUS_MAX);
        z->radius = WrClampF(want, RADIUS_MIN, RADIUS_MAX);

        // A cylinder, not a sphere. You can stand on a ledge directly above a
        // start pad and that is not being in the start zone; every other spatial
        // test in this tool is horizontal for the same reason.
        z->zLo = zLo - 128.0f;
        z->zHi = zHi + 192.0f;

        g_zoneCount++;
    }
}

static void EnsureBuilt(void)
{
    unsigned int gen = WrRunStoreGeneration();
    if (gen == g_builtFrom)
        return;
    g_builtFrom = gen;
    BuildZones();
    if (g_zoneCount > 0)
        WrLogf("start: %d zone%s from the loaded runs", g_zoneCount,
               g_zoneCount == 1 ? "" : "s");
    g_state = WR_START_AWAY;
    g_inZone = -1;
    g_haveS = false;
}

// ---------------------------------------------------------------------------

float WrStartZoneRadius(const WrStartZone *z)
{
    if (!z)
        return 0.0f;
    return WrClampF(z->radius * g_start.radiusScale, RADIUS_MIN * 0.4f,
                    RADIUS_MAX * 3.0f);
}

static bool Inside(const WrStartZone *z, const Vec3 &cam)
{
    if (cam.z < z->zLo || cam.z > z->zHi)
        return false;
    float r = WrStartZoneRadius(z);
    return HorizDistSqr(cam, z->centre) <= r * r;
}

// Which zone you are in. Ties happen constantly on staged maps -- stage 1's
// start IS the main start -- and matter less than they look: the two centres are
// a few units apart, so the anchor is the same either way and only the label is
// ambiguous. Preferring the one you are already in keeps it from flickering.
static int PickZone(const Vec3 &cam)
{
    if (g_inZone >= 0 && g_inZone < g_zoneCount && Inside(&g_zones[g_inZone], cam))
        return g_inZone;

    int best = -1;
    for (int i = 0; i < g_zoneCount; i++)
    {
        if (!Inside(&g_zones[i], cam))
            continue;
        if (best < 0 ||
            g_zones[i].trusted > g_zones[best].trusted ||
            (g_zones[i].trusted == g_zones[best].trusted &&
             g_zones[i].radius < g_zones[best].radius))
            best = i;
    }
    return best;
}

void WrStartDefaults(void)
{
    g_start.enabled = true;
    g_start.autoAnchor = true;
    g_start.autoZeroClock = true;
    // OFF by default. The ring, the arrow and the plane are a diagnostic for
    // checking that the fitted zone sits where the real one does; once it does,
    // they are three large pieces of geometry drawn over the part of the map you
    // spend the most time looking at.
    g_start.showZone = false;
    g_start.radiusScale = 1.0f;
    g_start.leaveSpeed = 150.0f;
    g_start.stillSpeed = 200.0f;
}

void WrStartReset(void)
{
    g_state = WR_START_AWAY;
    g_inZone = -1;
    g_dwell = g_armFor = g_awayFor = 0.0f;
    g_lockout = 0.0f;
    g_settle = SETTLE_SECONDS;
    g_haveS = false;
    g_crossed = false;
    g_crossedZone = NULL;
    g_since = -1.0f;
    g_builtFrom = 0;            // rebuild against whatever store exists now
}

void WrStartTick(const Vec3 &cam, float dt, bool teleported)
{
    if (dt > 0.0f)
    {
        if (g_lockout > 0.0f) g_lockout -= dt;
        if (g_settle > 0.0f) g_settle -= dt;
        if (g_since >= 0.0f) g_since += dt;
    }

    EnsureBuilt();

    g_why[0] = '\0';
    if (!g_start.enabled)
    {
        strcpy_s(g_why, sizeof(g_why), "start detection is off");
        return;
    }
    if (g_zoneCount == 0)
    {
        strcpy_s(g_why, sizeof(g_why),
                 "no runs loaded, so there is nothing to say where the start is");
        return;
    }

    // A teleport moves you without moving through anything, so every edge this
    // machine watches for is meaningless across one. Reset rather than reason.
    if (teleported || WrEnergyHeld())
    {
        g_state = WR_START_AWAY;
        g_inZone = -1;
        g_dwell = g_armFor = g_awayFor = 0.0f;
        g_haveS = false;
        g_settle = SETTLE_SECONDS;
        return;
    }

    int here = PickZone(cam);
    float speed = WrEnergyHorizontalSpeed();

    if (here < 0)
    {
        // Outside everything. Getting properly clear is what allows the next
        // arm, so this is timed rather than instant.
        g_dwell = 0.0f;
        g_armFor = 0.0f;

        // The no-heading fallback fires HERE, and it has to: leaving the circle
        // is what it is waiting for, and by the time you have left it PickZone
        // no longer returns the zone. Written inside the ARMED branch it looked
        // right and could never run, so a leg whose runs left in disagreeing
        // directions would arm and then sit there.
        if (g_state == WR_START_ARMED && g_inZone >= 0)
        {
            const WrStartZone *z = &g_zones[g_inZone];
            if (WrLength(z->outDir) < 0.5f && speed >= g_start.leaveSpeed &&
                g_lockout <= 0.0f)
            {
                g_state = WR_START_LEFT;
                g_crossed = true;
                g_crossedZone = z;
                g_lockout = LOCKOUT_SECONDS;
                g_since = 0.0f;
                WrLogf("start: left the %s start circle at %.0f u/s -- no "
                       "consistent heading on this leg, so this reads about "
                       "%.0f ms late",
                       WrTrackNameOf(z->trackType, z->trackNum), speed,
                       WrStartZoneRadius(z) / (speed > 1.0f ? speed : 1.0f)
                           * 1000.0f);
                return;
            }
        }

        if (g_state != WR_START_AWAY)
        {
            const WrStartZone *z = (g_inZone >= 0) ? &g_zones[g_inZone] : NULL;
            bool clear = true;
            if (z)
            {
                float r2 = WrStartZoneRadius(z) * 2.0f;
                clear = HorizDistSqr(cam, z->centre) > r2 * r2;
            }
            g_awayFor = clear ? g_awayFor + dt : 0.0f;
            if (g_awayFor >= AWAY_SECONDS)
            {
                g_state = WR_START_AWAY;
                g_inZone = -1;
                g_haveS = false;
            }
        }
        if (g_state == WR_START_AWAY)
        {
            float d = 0.0f;
            const WrStartZone *near_ = WrStartZoneNearest(&d);
            if (near_)
                _snprintf_s(g_why, sizeof(g_why), _TRUNCATE,
                            "%.0f units from the nearest start", d);
        }
        return;
    }

    g_inZone = here;
    const WrStartZone *z = &g_zones[here];
    g_awayFor = 0.0f;

    if (g_state == WR_START_AWAY || g_state == WR_START_LEFT)
    {
        if (g_state == WR_START_LEFT)
        {
            // Back inside without having got properly clear. That is milling
            // about at the start, not a new attempt, so it does not re-arm.
            strcpy_s(g_why, sizeof(g_why),
                     "already left this start once; move well clear to re-arm");
            return;
        }
        g_dwell += dt;
        if (g_dwell >= DWELL_SECONDS)
        {
            g_state = WR_START_INSIDE;
            g_haveS = false;
        }
        else
        {
            strcpy_s(g_why, sizeof(g_why), "just arrived in the start");
        }
        return;
    }

    if (g_state == WR_START_INSIDE)
    {
        // WrEnergyOnGround is a heuristic that also fires at the apex of every
        // arc -- its own header says so. It is safe HERE and would not be as a
        // trigger: an apex inside a 512-unit cylinder at under 200 u/s is the
        // top of a hop on the start pad, so the wrong answer and the right
        // answer are the same answer.
        bool still = (speed < g_start.stillSpeed) && WrEnergyOnGround();
        g_armFor = still ? g_armFor + dt : 0.0f;
        if (g_armFor >= ARM_SECONDS && g_settle <= 0.0f)
        {
            g_state = WR_START_ARMED;
            g_haveS = false;
        }
        else if (g_settle > 0.0f)
            strcpy_s(g_why, sizeof(g_why), "settling after a teleport");
        else if (!WrEnergyOnGround())
            strcpy_s(g_why, sizeof(g_why), "in the start, but not on the ground");
        else
            _snprintf_s(g_why, sizeof(g_why), _TRUNCATE,
                        "in the start, but moving at %.0f u/s", speed);
        return;
    }

    // ARMED. Watch for the plane crossing.
    bool haveDir = WrLength(z->outDir) > 0.5f;
    if (haveDir)
    {
        float s = WrDot(WrSub(cam, z->centre), z->outDir);
        bool outward = g_haveS && g_sPrev < 0.0f && s >= 0.0f;
        g_sPrev = s;
        g_haveS = true;

        if (outward && speed >= g_start.leaveSpeed && g_lockout <= 0.0f)
        {
            g_state = WR_START_LEFT;
            g_crossed = true;
            g_crossedZone = z;
            g_lockout = LOCKOUT_SECONDS;
            g_since = 0.0f;
            WrLogf("start: crossed the %s start line outward at %.0f u/s "
                   "(plane good to +-%.0f units, %d of %d runs placed it)",
                   WrTrackNameOf(z->trackType, z->trackNum), speed,
                   z->alongSpread, z->trusted, z->members);
            return;
        }
        _snprintf_s(g_why, sizeof(g_why), _TRUNCATE,
                    "armed -- %s", s < 0.0f ? "waiting for you to cross the line"
                                            : "past the line, going back would re-arm");
    }
    else
    {
        // No usable heading: the starts all point different ways, or everyone
        // drops straight down. The trigger is then simply leaving the circle,
        // which happens in the here < 0 branch above -- this state cannot see
        // it, because being outside is what makes PickZone stop returning the
        // zone. All that is left to do here is say so.
        strcpy_s(g_why, sizeof(g_why),
                 "armed -- no consistent start heading on this leg, so leaving "
                 "the circle is the trigger and it reads slightly late");
    }
}

bool WrStartTakeCrossed(const WrStartZone **which)
{
    if (!g_crossed)
        return false;
    g_crossed = false;
    if (which)
        *which = g_crossedZone;
    return true;
}

int WrStartZoneCount(void) { return g_zoneCount; }

const WrStartZone *WrStartZoneAt(int i)
{
    return (i >= 0 && i < g_zoneCount) ? &g_zones[i] : NULL;
}

const WrStartZone *WrStartZoneHere(void)
{
    return (g_inZone >= 0 && g_inZone < g_zoneCount) ? &g_zones[g_inZone] : NULL;
}

const WrStartZone *WrStartZoneNearest(float *dist)
{
    EnsureBuilt();
    Vec3 cam;
    if (!WrCameraOrigin(&cam) || g_zoneCount == 0)
        return NULL;
    const WrStartZone *best = NULL;
    float bestD = 0.0f;
    for (int i = 0; i < g_zoneCount; i++)
    {
        float d = sqrtf(HorizDistSqr(cam, g_zones[i].centre));
        if (!best || d < bestD)
        {
            best = &g_zones[i];
            bestD = d;
        }
    }
    if (dist)
        *dist = bestD;
    return best;
}

WrStartState WrStartStateNow(void) { return g_state; }

const char *WrStartStateName(void)
{
    switch (g_state)
    {
    case WR_START_INSIDE: return "in the start";
    case WR_START_ARMED:  return "armed";
    case WR_START_LEFT:   return "gone";
    default:              return "away";
    }
}

const char *WrStartWhyNot(void) { return g_why; }
float WrStartSince(void) { return g_since; }
