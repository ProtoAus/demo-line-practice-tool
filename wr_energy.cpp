// wr_energy.cpp  --  see wr_energy.h for the metric and what is approximate.

#include "wr_energy.h"
#include "wr_smooth.h"
#include "wr_stress.h"
#include "wr_engine.h"
#include "wr_log.h"

#include <math.h>
#include <string.h>

WrEnergySettings g_energy;

// --- filter time constants, in SECONDS ---------------------------------------
//
// Every one of these used to be a frame count, which made the readout behave
// like a different instrument at 60 fps than at 300 -- and the frame rate on a
// surf map moves with how many lines are drawn, so turning lines on changed how
// the number moved. See wr_smooth.h.

// Velocity is differenced over a fixed time window rather than a fixed number of
// frames. 40 ms is long enough that a two-unit view bob reads as 50 u/s instead
// of the 400 u/s a single 200 fps frame would give, and short enough to follow a
// ramp entry.
#define VEL_WINDOW_SECONDS 0.040f
#define VEL_TAU 0.060f              // then smoothed, still in seconds

#define SPEED_TAU 0.100f            // a speedometer may not lag much
#define TURN_TAU 0.120f             // turn rates are noisy; this is a readout
#define ACCEL_TAU 0.150f

// The arrow has to hold a new state this long before it is allowed on screen.
#define ARROW_HOLD 0.20f

// Ground detection, kept for the "last jump" line only. It no longer drives the
// reference height -- that was the bug; see the header.
#define GROUND_VZ 30.0f
#define GROUND_SECONDS 0.05f
#define GROUND_Z_SLACK 6.0f
#define JUMP_VZ 150.0f

// A camera-differenced velocity past this is not the player moving.
#define MAX_SANE_SPEED 10000.0f
#define TELEPORT_UNITS 400.0f

#define HISTORY 240
#define HISTORY_HZ 20

static WrVelWindow g_win;
static WrEma g_velX, g_velY, g_velZ;
static WrEma g_speedEma, g_energyEma, g_viewTurnEma, g_velTurnEma, g_accelEma;
static WrTrendWindow g_trend;
static WrArrow g_arrow;

static float g_clock = 0.0f;

static bool g_valid = false;
static Vec3 g_vel;
static Vec3 g_lastPos;
static float g_speed = 0.0f;
static float g_now = 0.0f;          // absolute, instantaneous
static float g_nowSmooth = 0.0f;    // absolute, filtered -- what gets displayed
static float g_peak = 0.0f;         // peak *relative*

static bool g_haveStart = false;
static float g_start = 0.0f;

// The anchor. Set once, never re-armed by anything the player does.
static bool g_haveRef = false;
static float g_refZ = 0.0f;
static Vec3 g_refPos;
static WrAnchorSource g_refSource = WR_ANCHOR_NONE;

static bool g_haveGround = false;
static float g_groundZ = 0.0f;
static float g_groundEnergy = 0.0f;

static float g_settledFor = 0.0f;   // seconds looking grounded
static float g_settledZ = 0.0f;
static bool g_onGround = false;

// Turn rates.
static bool g_haveFwd = false;
static Vec3 g_lastFwd;
static bool g_haveVelDir = false;
static Vec3 g_lastVelDir;
static float g_lastSpeedForRate = 0.0f;
static float g_viewTurn = 0.0f, g_velTurn = 0.0f, g_speedRate = 0.0f;

// Displayed value, quantised with hysteresis so the last digit stops churning.
static float g_shown = 0.0f;
static bool g_haveShown = false;

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
    g_energy.hudScale = 1.4f;
    g_energy.hudBacking = true;

    g_energy.showOverlay = false;
    g_energy.overlayCorner = 3;
    g_energy.overlayScale = 1.0f;

    g_energy.compareToRun = true;
    g_energy.compareRadius = 384.0f;
    g_energy.eyeHeight = 64.0f;

    // 0.30 s reads as "settling towards a value" rather than "a number". Below
    // about 0.15 the residual velocity noise is visible again; above about 0.5
    // it lags a ramp exit enough to be misleading.
    g_energy.smoothSeconds = 0.30f;
    g_energy.trendSeconds = 0.75f;
    g_energy.quantiseStep = 5.0f;
    g_energy.anchorToRunStart = true;
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
    WrVelReset(&g_win);
    WrEmaReset(&g_velX); WrEmaReset(&g_velY); WrEmaReset(&g_velZ);
    WrEmaReset(&g_speedEma); WrEmaReset(&g_energyEma);
    WrEmaReset(&g_viewTurnEma); WrEmaReset(&g_velTurnEma); WrEmaReset(&g_accelEma);
    WrTrendReset(&g_trend);
    WrArrowReset(&g_arrow);

    g_valid = false;
    g_speed = 0.0f;
    g_peak = 0.0f;
    g_haveStart = false;
    g_haveRef = false;
    g_refSource = WR_ANCHOR_NONE;
    g_haveGround = false;
    g_settledFor = 0.0f;
    g_onGround = false;
    g_historyCount = 0;
    g_clock = 0.0f;
    g_haveFwd = false;
    g_haveVelDir = false;
    g_haveShown = false;
    g_viewTurn = g_velTurn = g_speedRate = 0.0f;
    g_vel = WrVec(0.0f, 0.0f, 0.0f);
}

void WrEnergyRearm(void)
{
    if (!g_valid)
        return;
    g_refZ = g_lastPos.z;
    g_refPos = g_lastPos;
    g_haveRef = true;
    g_refSource = WR_ANCHOR_MANUAL;
    g_start = g_now;
    g_haveStart = true;
    g_peak = 0.0f;
    WrLogf("energy: anchored here, z %.0f", g_refZ);
}

void WrEnergyAnchorToFeet(const Vec3 &feet)
{
    // A run's points are the player origin. Ours is the eye, about 64 units
    // higher, so without this the whole readout is offset by a player's height.
    g_refZ = feet.z + g_energy.eyeHeight;
    g_refPos = feet;
    g_haveRef = true;
    g_refSource = WR_ANCHOR_RUN_START;
    g_peak = 0.0f;
    if (g_valid)
    {
        g_start = g_now;
        g_haveStart = true;
    }
    WrLogf("energy: anchored to the run's start (%.0f %.0f %.0f), reference z %.0f",
           feet.x, feet.y, feet.z, g_refZ);
}

WrAnchorSource WrEnergyAnchorSource(void) { return g_refSource; }

bool WrEnergyAnchorPos(Vec3 *out)
{
    if (!g_haveRef)
        return false;
    if (out)
        *out = g_refPos;
    return true;
}

void WrEnergySample(const Vec3 &pos, float dt)
{
    if (!WrSaneVec(pos) || !(dt > 0.0f) || dt > 0.5f)
        return;

    g_clock += dt;

    // A teleport is not movement. Drop the window rather than report a velocity
    // of tens of thousands of units per second.
    if (g_valid && WrDist(g_lastPos, pos) > TELEPORT_UNITS)
    {
        WrVelReset(&g_win);
        g_settledFor = 0.0f;
        g_haveVelDir = false;
    }

    WrVelPush(&g_win, pos.x, pos.y, pos.z, dt);

    float rx = 0.0f, ry = 0.0f, rz = 0.0f;
    float mx = 0.0f, my = 0.0f, mz = 0.0f;
    if (!WrVelEstimate(&g_win, VEL_WINDOW_SECONDS, &rx, &ry, &rz, &mx, &my, &mz))
        return;

    Vec3 raw = WrVec(rx, ry, rz);
    Vec3 mid = WrVec(mx, my, mz);
    if (!WrSaneVec(raw))
        return;

    // Throw the whole sample away rather than let one bad frame into the filter.
    if (WrLength(raw) > MAX_SANE_SPEED)
    {
        WrVelReset(&g_win);
        WrVelPush(&g_win, pos.x, pos.y, pos.z, 0.0f);
        g_settledFor = 0.0f;
        return;
    }

    g_vel.x = WrEmaStep(&g_velX, raw.x, dt, VEL_TAU);
    g_vel.y = WrEmaStep(&g_velY, raw.y, dt, VEL_TAU);
    g_vel.z = WrEmaStep(&g_velZ, raw.z, dt, VEL_TAU);

    float instSpeed = WrLength(g_vel);
    g_speed = WrEmaStep(&g_speedEma, instSpeed, dt, SPEED_TAU);

    g_valid = true;
    g_lastPos = pos;

    // The RAW window velocity paired with the window's MIDPOINT height, not the
    // smoothed velocity paired with the current height. Both then refer to the
    // same instant, which is what makes E correct while accelerating: measured
    // 20 ms apart, a ballistic arc appeared to lose 46 units of energy over 1.6 s
    // of free fall, and energy is conserved in free fall. See WrVelEstimate.
    g_now = WrEnergyOf(mid, raw);

    // The headline figure is filtered here rather than at the point of display,
    // so the arrow, the peak and the plot all agree with what is on screen.
    g_nowSmooth = WrEmaStep(&g_energyEma, g_now, dt, g_energy.smoothSeconds);
    WrTrendPush(&g_trend, g_nowSmooth, dt);

    // --- turn rates ---------------------------------------------------------
    //
    // The view rate is exact -- the camera forward vector comes straight out of
    // the matrix we already solved, so this really is how fast the mouse moved.
    Vec3 fwd;
    if (WrCameraForward(&fwd))
    {
        if (g_haveFwd && dt > 1e-5f)
        {
            float d = WrDot(WrNormalize(fwd), WrNormalize(g_lastFwd));
            d = WrClampF(d, -1.0f, 1.0f);
            float deg = acosf(d) * 57.2957795f;
            g_viewTurn = WrEmaStep(&g_viewTurnEma, deg / dt, dt, TURN_TAU);
        }
        g_lastFwd = fwd;
        g_haveFwd = true;
    }

    // Where the momentum is turning, which is the thing a demo line can also be
    // measured for. Horizontal only: a surf ramp constantly changes vz, and
    // counting that would swamp the mouse movement we are looking for.
    Vec3 flat = WrVec(g_vel.x, g_vel.y, 0.0f);
    float flatLen = WrLength(flat);
    if (flatLen > 1.0f)
    {
        Vec3 dir = WrScale(flat, 1.0f / flatLen);
        if (g_haveVelDir && dt > 1e-5f)
        {
            float d = WrClampF(WrDot(dir, g_lastVelDir), -1.0f, 1.0f);
            float deg = acosf(d) * 57.2957795f;
            g_velTurn = WrEmaStep(&g_velTurnEma, deg / dt, dt, TURN_TAU);
        }
        g_lastVelDir = dir;
        g_haveVelDir = true;
    }
    if (dt > 1e-5f)
    {
        g_speedRate = WrEmaStep(&g_accelEma, (instSpeed - g_lastSpeedForRate) / dt,
                                dt, ACCEL_TAU);
    }
    g_lastSpeedForRate = instSpeed;

    // --- ground -------------------------------------------------------------
    //
    // Debounced in seconds, not frames. This is now only used for the "last
    // jump" line and the on-ground indicator; it deliberately does NOT touch the
    // anchor, because it fires at the apex of every arc -- see the header.
    if (fabsf(g_vel.z) < GROUND_VZ)
    {
        if (g_settledFor <= 0.0f)
            g_settledZ = pos.z;
        if (fabsf(pos.z - g_settledZ) < GROUND_Z_SLACK)
            g_settledFor += dt;
        else
        {
            g_settledFor = dt;
            g_settledZ = pos.z;
        }
    }
    else
    {
        if (g_settledFor >= GROUND_SECONDS && g_vel.z > JUMP_VZ)
        {
            g_haveGround = true;
            g_groundZ = g_settledZ;
            g_groundEnergy = g_now;
        }
        g_settledFor = 0.0f;
    }
    g_onGround = (g_settledFor >= GROUND_SECONDS);

    if (!g_haveStart && g_haveRef)
    {
        g_haveStart = true;
        g_start = g_now;
    }

    float rel = WrEnergyRelative();
    if (rel > g_peak)
        g_peak = rel;

    g_historyClock += dt;
    if (g_historyClock >= 1.0f / (float)HISTORY_HZ)
    {
        g_historyClock -= 1.0f / (float)HISTORY_HZ;
        if (g_historyCount < HISTORY)
            g_history[g_historyCount++] = g_nowSmooth;
        else
        {
            memmove(g_history, g_history + 1, sizeof(float) * (HISTORY - 1));
            g_history[HISTORY - 1] = g_nowSmooth;
        }
    }
}

bool WrEnergyValid(void) { return g_valid; }
float WrEnergyNow(void) { return g_now; }

float WrEnergyRelative(void)
{
    if (!g_valid)
        return 0.0f;
    // With no anchor the only honest reference is where you are, which reads
    // zero. Better than measuring from the map origin and showing five figures.
    float ref = g_haveRef ? g_refZ : g_lastPos.z;
    float raw = g_nowSmooth - ref;
    g_shown = WrQuantise(g_shown, raw, g_energy.quantiseStep, &g_haveShown);
    return g_shown;
}

float WrEnergyEquivSpeed(void)
{
    float g = g_energy.gravity;
    if (g < 1.0f)
        g = 1.0f;
    float e = WrEnergyRelative();
    // sqrt has an unbounded derivative at zero: ±6 units of dither about the
    // origin used to swing this ±98 u/s AND flip its sign while standing still.
    // Below the dead zone there is no meaningful energy to express as a speed.
    if (fabsf(e) < 8.0f)
        return 0.0f;
    float s = sqrtf(fabsf(e) * 2.0f * g);
    return e < 0.0f ? -s : s;
}

float WrEnergyTrend(void)
{
    return WrTrendOver(&g_trend, g_energy.trendSeconds);
}

int WrEnergyTrendDir(void)
{
    // A fixed band, not one that grows with speed. The old band reached 159
    // units at 3500 u/s, which made the arrow blind exactly when it mattered --
    // it had to be that wide because it was judging an unfiltered value. The
    // trend is now taken from the smoothed figure, which does not carry that
    // noise, so 12 units is enough. WrArrowStep supplies the hysteresis.
    return g_arrow.shown;
}

float WrEnergySpeed(void) { return g_speed; }
float WrEnergyHorizontalSpeed(void)
{
    return sqrtf(g_vel.x * g_vel.x + g_vel.y * g_vel.y);
}

bool WrEnergyVelocity(Vec3 *out)
{
    if (!g_valid)
        return false;
    if (out)
        *out = g_vel;
    return true;
}

// dE/dt, for the efficiency figure. Taken over a window rather than per frame,
// because a derivative of a noisy signal is noise.
#define POWER_WINDOW 0.30f
float WrEnergyPower(void)
{
    return WrTrendOver(&g_trend, POWER_WINDOW) / POWER_WINDOW;
}

float WrEnergyViewTurnRate(void) { return g_viewTurn; }
float WrEnergyVelTurnRate(void) { return g_velTurn; }
float WrEnergySpeedRate(void) { return g_speedRate; }

float WrEnergySinceGround(void)
{
    return g_haveGround ? (g_nowSmooth - g_groundEnergy) : 0.0f;
}
float WrEnergySinceStart(void)
{
    return g_haveStart ? (g_nowSmooth - g_start) : 0.0f;
}
float WrEnergyPeak(void) { return g_peak; }
bool WrEnergyOnGround(void) { return g_onGround; }
bool WrEnergyHaveRef(void) { return g_haveRef; }
float WrEnergyRefZ(void) { return g_haveRef ? g_refZ : 0.0f; }
bool WrEnergyHaveGround(void) { return g_haveGround; }
float WrEnergyGroundZ(void) { return g_groundZ; }

int WrEnergyHistory(const float **out)
{
    float ref = g_haveRef ? g_refZ : (g_valid ? g_lastPos.z : 0.0f);
    for (int i = 0; i < g_historyCount; i++)
        g_historyRel[i] = g_history[i] - ref;
    if (out)
        *out = g_historyRel;
    return g_historyCount;
}

// Advance the arrow. Separate from the sample path because it needs the trend
// that WrEnergySample has just pushed, and because the dead band is a display
// decision rather than a measurement.
void WrEnergyTickArrow(float dt)
{
    if (!g_valid)
        return;
    WrArrowStep(&g_arrow, WrEnergyTrend(), 12.0f, dt, ARROW_HOLD);
}
