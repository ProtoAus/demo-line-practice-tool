// wr_stress.h  --  is this stretch of line adding energy, or throwing it away?
//
// Pure float arithmetic so the same code measures a stored run at load time and
// the live player every frame, and so it can be driven by a script in a test.
//
// THE OBVIOUS METRIC IS WRONG, AND THE DATA SAYS SO
//
// The intuition is "colour the line where the player turned too fast", since
// Source's air acceleration only adds speed while the wish direction is still
// ahead of the velocity. So the first version of this file measured the turn
// rate of the velocity vector and flagged it where speed was falling.
//
// Measured against record-class runs, that fires constantly on perfect play. Air
// acceleration can turn a velocity by at most ws/|v| radians per tick, which at
// 2000 u/s and a 0.015 tick is 57 deg/s -- but a surf RAMP turns the velocity
// too, through the surface normal, and it can turn it far faster. The fraction
// of samples turning faster than air accel alone could manage:
//
//     surf_demise WR (37.170s)     10% of samples
//     surf_vacant WR (89.310s)     ~20%
//     surf_666    (43.575s)        24%
//
// A metric that fires on a tenth of the world record is not a mistake detector.
// Turn rate is kept below, but only as a disambiguator for the live readout --
// never as the thing that colours a line.
//
// WHAT IS ACTUALLY MEASURABLE
//
// Not the air-accel term itself: that needs the wish direction, which needs the
// view angles AND the movement keys, and a .wrpath records neither. But the
// CONSEQUENCE is exactly measurable from position and velocity, and it has a
// hard physical ceiling to measure it against.
//
// Per tick, with wishspeed capped at ws and c = dot(v, wishdir):
//
//     2g dE = |v + a*w|^2 - |v|^2 = 2ac + a^2 = 2(ws-c)c + (ws-c)^2 = ws^2 - c^2
//
// maximised at c = 0, so the most energy air strafing can add is
//
//     P_max = ws^2 / (2 * g * tick)  =  900 / (2*800*0.015)  =  37.5 units/s
//
// independent of speed. Checked against twelve record-class runs on three maps:
// the 99th percentile of dE/dt lands between +38.18 and +38.88 against a theory
// of 37.50. The ceiling is real and it is tight.
//
// So the metric is efficiency against that ceiling:
//
//     eta = (dE/dt) / P_max          1.0 = strafing as well as physics allows
//
// which discriminates enormously: median eta on the surf_demise world record is
// +0.531, and on the two slowest runs in the same set (55.9 s and 57.8 s) it is
// +0.010 and +0.003. Across 51 surf_demise runs, corr(run time, mean eta) is
// -0.787.
//
// WHERE IT MISLEADS, AND IT DOES
//
//   Map boosters and triggers add energy for free -- dE/dt maxima of +726,
//   +3116, +9039 and +13142 units/s appear in real runs, up to 350x the ceiling.
//   Anything past 3x is not the player and is drawn neutral.
//
//   A ramp collision removes energy with no turning at all, so a red patch on a
//   ramp entry means a bad entry, not over-turning.
//
//   Free flight is eta = 0 exactly. That is correct, not a failure.
//
//   On the ground it means nothing at all.

#ifndef WR_STRESS_H
#define WR_STRESS_H

#include <math.h>

// sv_air_max_wishspeed. The one Source constant this rests on.
#define WR_AIR_WISHSPEED 30.0f

// The most energy air strafing can add, in energy units per second. See above.
static inline float WrAirPowerCeiling(float gravity, float tickInterval)
{
    if (gravity < 1.0f) gravity = 1.0f;
    if (tickInterval < 1e-4f) tickInterval = 0.015f;
    return (WR_AIR_WISHSPEED * WR_AIR_WISHSPEED) /
           (2.0f * gravity * tickInterval);
}

// Past this much of the ceiling, it was not the player -- it was a booster.
#define WR_ETA_NOT_PLAYER 3.0f

// Efficiency against the ceiling, clamped to [-1, 1] for display. Values beyond
// WR_ETA_NOT_PLAYER return exactly 0 and should be drawn neutral.
static inline float WrEfficiency(float dEnergyPerSecond, float ceiling)
{
    if (ceiling <= 1e-6f)
        return 0.0f;
    float eta = dEnergyPerSecond / ceiling;
    if (eta > WR_ETA_NOT_PLAYER || eta < -WR_ETA_NOT_PLAYER)
        return 0.0f;                    // a trigger, not a player
    if (eta > 1.0f) eta = 1.0f;
    if (eta < -1.0f) eta = -1.0f;
    return eta;
}

// Stored per point as a signed byte.
static inline signed char WrEtaToByte(float eta)
{
    float v = eta * 127.0f;
    if (v > 127.0f) v = 127.0f;
    if (v < -127.0f) v = -127.0f;
    return (signed char)(v < 0.0f ? v - 0.5f : v + 0.5f);
}

static inline float WrEtaFromByte(signed char b)
{
    return (float)b / 127.0f;
}

// ---------------------------------------------------------------------------
// Turn rate -- a live disambiguator only, never a line colour
// ---------------------------------------------------------------------------

// Degrees per second between two horizontal velocities.
static inline float WrTurnRateDeg(float vx0, float vy0, float vx1, float vy1,
                                  float dt)
{
    if (dt <= 1e-6f)
        return 0.0f;
    float l0 = sqrtf(vx0 * vx0 + vy0 * vy0);
    float l1 = sqrtf(vx1 * vx1 + vy1 * vy1);
    // Below walking pace a direction is noise, and so is its angle.
    if (l0 < 32.0f || l1 < 32.0f)
        return 0.0f;
    float d = (vx0 * vx1 + vy0 * vy1) / (l0 * l1);
    if (d > 1.0f) d = 1.0f;
    if (d < -1.0f) d = -1.0f;
    return acosf(d) * 57.2957795f / dt;
}

// The fastest air acceleration ALONE could turn this velocity, in deg/s. Turning
// faster than this is not a mistake -- a ramp does it constantly -- it just
// means something other than your strafing is doing the turning.
static inline float WrTurnCeilingDeg(float horizontalSpeed, float tickInterval)
{
    if (horizontalSpeed < 1.0f || tickInterval < 1e-4f)
        return 0.0f;
    float radPerTick = WR_AIR_WISHSPEED / horizontalSpeed;
    if (radPerTick > 1.0f) radPerTick = 1.0f;
    return radPerTick * 57.2957795f / tickInterval;
}

#endif // WR_STRESS_H
