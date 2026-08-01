// wr_energy.cpp  --  see wr_energy.h for the metric and what is approximate.

#include "wr_energy.h"
#include "wr_log.h"

#include <math.h>
#include <string.h>

WrEnergySettings g_energy;

// Velocity baseline. One frame is far too short: at 200 fps a two-unit view bob
// reads as 400 u/s. Three frames back is still under 20 ms of lag at typical
// frame rates and cuts that noise by the same factor.
#define VEL_BASELINE 4
#define VEL_SMOOTH 0.25f            // EMA weight for the new sample

// Ground detection. Source's jump impulse is around 268 u/s, so anything past
// 150 climbing out of a settled state is unambiguously a jump.
#define GROUND_VZ 30.0f
#define GROUND_FRAMES 3
#define GROUND_Z_SLACK 6.0f
#define JUMP_VZ 150.0f

// A camera-differenced velocity past this is not the player moving. The 400
// unit per-sample teleport guard below does not catch everything: a spawn or a
// map load can shift the camera fast but not far enough to trip it, and folding
// one such sample into the EMA used to poison the start reference for the whole
// session. The fastest surf on record is nowhere near this.
#define MAX_SANE_SPEED 10000.0f

// How long to wait before giving up on ever seeing the ground and arming the
// start reference anyway.
#define START_ARM_SECONDS 1.0f
#define START_ARM_SPEED 4000.0f

#define HISTORY 240
#define HISTORY_HZ 20
#define TREND_SAMPLES 8             // ~0.4 s at 20 Hz

struct Sample
{
    Vec3 pos;
    float t;
};

static Sample g_ring[VEL_BASELINE + 1];
static int g_ringCount = 0;
static float g_clock = 0.0f;

static bool g_valid = false;
static Vec3 g_vel;
static Vec3 g_lastPos;
static float g_speed = 0.0f;
static float g_now = 0.0f;          // absolute
static float g_peak = 0.0f;         // peak *relative*

static bool g_haveStart = false;
static float g_start = 0.0f;

// The height everything is measured from: the last ground we were settled on.
// Tracked while grounded, frozen the moment we leave it, which on a surf map
// means it locks to the start pad for the whole run.
static bool g_haveRef = false;
static float g_refZ = 0.0f;

static bool g_haveGround = false;
static float g_groundZ = 0.0f;
static float g_groundEnergy = 0.0f;

static int g_settled = 0;           // consecutive frames looking grounded
static float g_settledZ = 0.0f;
static bool g_onGround = false;

// Absolute energies, not relative. Energy gained or lost is a physical fact and
// does not depend on what we happen to be measuring from, so a reference change
// (landing somewhere new) must not show up as a spike in the trend.
static float g_history[HISTORY];
static float g_historyRel[HISTORY];
static int g_historyCount = 0;
static float g_historyClock = 0.0f;

void WrEnergyDefaults(void)
{
    g_energy.gravity = 800.0f;

    g_energy.showHud = true;
    g_energy.hudOffsetX = 72.0f;
    g_energy.hudOffsetY = 0.0f;
    g_energy.hudScale = 1.0f;
    g_energy.hudBacking = true;

    // Off by default now that the crosshair readout exists -- the corner block
    // is the same numbers somewhere you cannot read them mid-surf.
    g_energy.showOverlay = false;
    g_energy.overlayCorner = 3;

    g_energy.compareToRun = true;
    g_energy.compareRadius = 384.0f;
    g_energy.eyeHeight = 64.0f;
}

float WrEnergyOf(const Vec3 &pos, const Vec3 &vel)
{
    float g = g_energy.gravity;
    if (g < 1.0f)
        g = 1.0f;
    float v2 = vel.x * vel.x + vel.y * vel.y + vel.z * vel.z;
    return pos.z + v2 / (2.0f * g);
}

void WrEnergyReset(void)
{
    g_ringCount = 0;
    g_valid = false;
    g_speed = 0.0f;
    g_peak = 0.0f;
    g_haveStart = false;
    g_haveRef = false;
    g_haveGround = false;
    g_settled = 0;
    g_onGround = false;
    g_historyCount = 0;
    g_clock = 0.0f;
    g_vel = WrVec(0.0f, 0.0f, 0.0f);
}

void WrEnergyRearm(void)
{
    if (!g_valid)
        return;
    g_refZ = g_lastPos.z;
    g_haveRef = true;
    g_start = g_now;
    g_haveStart = true;
    g_peak = 0.0f;
}

void WrEnergySample(const Vec3 &pos, float dt)
{
    if (!WrSaneVec(pos) || !(dt > 0.0f) || dt > 0.5f)
        return;

    g_clock += dt;

    // A teleport is not movement. Restart the baseline rather than report a
    // velocity of tens of thousands of units per second.
    if (g_ringCount > 0 && WrDist(g_ring[g_ringCount - 1].pos, pos) > 400.0f)
    {
        g_ringCount = 0;
        g_settled = 0;
    }

    if (g_ringCount < VEL_BASELINE + 1)
    {
        g_ring[g_ringCount].pos = pos;
        g_ring[g_ringCount].t = g_clock;
        g_ringCount++;
    }
    else
    {
        for (int i = 0; i < VEL_BASELINE; i++)
            g_ring[i] = g_ring[i + 1];
        g_ring[VEL_BASELINE].pos = pos;
        g_ring[VEL_BASELINE].t = g_clock;
    }

    if (g_ringCount < 2)
        return;

    const Sample &oldest = g_ring[0];
    const Sample &newest = g_ring[g_ringCount - 1];
    float span = newest.t - oldest.t;
    if (span <= 1e-5f)
        return;

    Vec3 raw = WrScale(WrSub(newest.pos, oldest.pos), 1.0f / span);
    if (!WrSaneVec(raw))
        return;

    // Throw the whole sample away rather than let one bad frame into the EMA.
    if (WrLength(raw) > MAX_SANE_SPEED)
    {
        g_ringCount = 1;
        g_ring[0].pos = pos;
        g_ring[0].t = g_clock;
        g_settled = 0;
        return;
    }

    if (!g_valid)
        g_vel = raw;
    else
        g_vel = WrAdd(WrScale(g_vel, 1.0f - VEL_SMOOTH), WrScale(raw, VEL_SMOOTH));

    g_valid = true;
    g_lastPos = pos;
    g_speed = WrLength(g_vel);
    g_now = WrEnergyOf(pos, g_vel);

    // --- ground, and the reference height -----------------------------------
    //
    // "Ground" is two conditions, not one: the camera has to be vertically
    // settled (so a ramp, where vz is large and steady, does not qualify) and it
    // has to stay that way for a few frames.
    bool settledNow = (fabsf(g_vel.z) < GROUND_VZ);
    if (settledNow)
    {
        if (g_settled == 0)
            g_settledZ = pos.z;
        if (fabsf(pos.z - g_settledZ) < GROUND_Z_SLACK)
            g_settled++;
        else
        {
            g_settled = 1;
            g_settledZ = pos.z;
        }
    }
    else
    {
        // Leaving a settled state upwards and hard is a jump, which is the one
        // worth recording separately from the plain reference height.
        if (g_settled >= GROUND_FRAMES && g_vel.z > JUMP_VZ)
        {
            g_haveGround = true;
            g_groundZ = g_settledZ;
            g_groundEnergy = g_now;
        }
        g_settled = 0;
    }
    g_onGround = (g_settled >= GROUND_FRAMES);

    // Track the ground exactly while we are on it, so standing still reads zero
    // rather than "within six units of zero", and walking down a slope measures
    // from the slope. Frozen as soon as we leave, whether by jumping or by
    // walking off an edge -- both have to arm it, and on surf both are the pad.
    if (g_onGround)
    {
        g_refZ = pos.z;
        g_haveRef = true;
    }

    // --- start reference ----------------------------------------------------
    //
    // Never on the first valid sample: that lands during the spawn, when the
    // camera-differenced velocity is thousands of units per second of nonsense,
    // and it used to bake a five-figure error into "since start" permanently.
    if (!g_haveStart)
    {
        if (g_onGround ||
            (g_clock > START_ARM_SECONDS && g_speed < START_ARM_SPEED))
        {
            g_haveStart = true;
            g_start = g_now;
        }
    }

    float rel = WrEnergyRelative();
    if (rel > g_peak)
        g_peak = rel;

    // --- history ------------------------------------------------------------
    g_historyClock += dt;
    if (g_historyClock >= 1.0f / (float)HISTORY_HZ)
    {
        g_historyClock = 0.0f;
        if (g_historyCount < HISTORY)
            g_history[g_historyCount++] = g_now;
        else
        {
            memmove(g_history, g_history + 1, sizeof(float) * (HISTORY - 1));
            g_history[HISTORY - 1] = g_now;
        }
    }
}

bool WrEnergyValid(void) { return g_valid; }
float WrEnergyNow(void) { return g_now; }

float WrEnergyRelative(void)
{
    if (!g_valid)
        return 0.0f;
    // Before we have ever touched ground the only honest reference is where we
    // are, which reads zero -- better than measuring from the map origin.
    float ref = g_haveRef ? g_refZ : g_lastPos.z;
    return g_now - ref;
}

float WrEnergyEquivSpeed(void)
{
    float g = g_energy.gravity;
    if (g < 1.0f)
        g = 1.0f;
    float e = WrEnergyRelative();
    // Signed rather than clamped: below the reference you really are down on
    // the deal, and hiding that behind a zero would be the wrong kind of tidy.
    float s = sqrtf(fabsf(e) * 2.0f * g);
    return e < 0.0f ? -s : s;
}

float WrEnergyTrend(void)
{
    if (g_historyCount < 2)
        return 0.0f;
    int back = g_historyCount - 1 - TREND_SAMPLES;
    if (back < 0)
        back = 0;
    return g_now - g_history[back];
}

int WrEnergyTrendDir(void)
{
    float g = g_energy.gravity;
    if (g < 1.0f)
        g = 1.0f;
    // Six units covers the jitter when you are standing still; the rest scales
    // with the kinetic term, which is where the velocity error lives. A good
    // ramp gains several hundred units over the trend window, so this stays
    // well inside what is worth reporting.
    float band = 6.0f + 0.02f * (g_speed * g_speed) / (2.0f * g);
    float t = WrEnergyTrend();
    if (t > band) return 1;
    if (t < -band) return -1;
    return 0;
}

float WrEnergySpeed(void) { return g_speed; }
float WrEnergyHorizontalSpeed(void)
{
    return sqrtf(g_vel.x * g_vel.x + g_vel.y * g_vel.y);
}
float WrEnergySinceGround(void)
{
    return g_haveGround ? (g_now - g_groundEnergy) : 0.0f;
}
float WrEnergySinceStart(void)
{
    return g_haveStart ? (g_now - g_start) : 0.0f;
}
float WrEnergyPeak(void) { return g_peak; }
bool WrEnergyOnGround(void) { return g_onGround; }
bool WrEnergyHaveRef(void) { return g_haveRef; }
float WrEnergyRefZ(void) { return g_haveRef ? g_refZ : 0.0f; }
bool WrEnergyHaveGround(void) { return g_haveGround; }
float WrEnergyGroundZ(void) { return g_groundZ; }

int WrEnergyHistory(const float **out)
{
    // Stored absolute so the trend survives a reference change; rebased here so
    // the plot reads in the same numbers as everything else on screen.
    float ref = g_haveRef ? g_refZ : (g_valid ? g_lastPos.z : 0.0f);
    for (int i = 0; i < g_historyCount; i++)
        g_historyRel[i] = g_history[i] - ref;
    if (out)
        *out = g_historyRel;
    return g_historyCount;
}
