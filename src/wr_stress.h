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
// WHY THIS COLOURS DEMO LINES AND NOT YOUR OWN
//
// Asked for directly, and measured before answering. A stored run differences a
// velocity the demo recorded; your own line has to difference one estimated
// from camera positions. Simulated against twelve real surf_demise runs --
// resample the position at 200 Hz, add view bob, push it through the real
// estimator in wr_smooth.h, record points the way WrLiveRecord does, then
// compare eta against the truth OVER THE SAME WINDOW so only the estimate is
// being judged:
//
//     window    colour agrees    points the wrong way
//     0.25 s        45.2%              26.0%
//     0.40 s        58.2%              24.1%
//     0.60 s        63.5%              21.8%
//     2.00 s        81.5%               8.5%
//
// A quarter of the line drawn backwards is not a metric, and 2 s smears across
// a whole ramp. Two things make this conclusive rather than a tuning problem:
//
// View bob does not matter. At bob = 0 -- a perfectly noiseless camera -- the
// numbers are the same to a point or two. This is not jitter that a filter
// could take out.
//
// And it is WORSE where it matters. Restricted to airborne samples, where eta
// is actually air-strafing efficiency rather than a ramp collision, it agrees
// 45.5% at 0.40 s and points the wrong way 32.1% -- and barely improves with a
// longer window. Contact is easy to sign because the losses are enormous; the
// small numbers are the whole point of the metric, and they are exactly the
// ones a camera-differenced velocity cannot resolve against a 37 units/s
// ceiling.
//
// So: not shipped for the live line, and the legend says so on screen. The
// energy readout and the gap against a run remain the live answer.
//
// THE CEILING IS ON GAIN ONLY, AND THE FIRST VERSION GOT THAT BACKWARDS
//
// P_max bounds how fast energy can be ADDED. Nothing bounds how fast it can be
// taken away: a ramp collision clips the velocity into the surface plane and can
// remove as much as the geometry likes. So the rejection test has to be
// one-sided, and the first version of this file made it symmetric. Measured over
// 865,026 samples from 342 stored runs:
//
//     eta > +3     9,675 samples    1.12%   really is a booster
//     eta < -3   162,601 samples   18.80%   really is a ramp entry
//
// 94.4% of the rejections were negative, and those samples carry 94.6% OF ALL
// ENERGY LOST IN THE WHOLE LIBRARY. Every hard ramp entry -- the most
// informative thing on a surf line, and the reason this file exists -- was being
// collapsed to exactly 0.0 and drawn as "nothing is happening". The reported
// symptom was that the colours made no sense.
//
// So only the positive sentinel survives, and it now reports itself as NO DATA
// rather than sharing a value with free flight.
//
// WHERE IT STILL MISLEADS
//
//   Map boosters and triggers add energy for free -- dE/dt maxima of +726,
//   +3116, +9039 and +13142 units/s appear in real runs, up to 350x the ceiling.
//   Past 3x it is not the player, and it is drawn as a gap rather than as
//   perfect play.
//
//   A ramp collision removes energy with no turning at all, so red on a ramp
//   entry means a bad entry, not over-turning. Every entry costs something, so
//   short red flecks between long green stretches are what a good run looks
//   like.
//
//   Free flight is eta = 0 exactly. That is correct, not a failure.
//
//   On the ground it means nothing at all.
//
// THE DEADSTRAFE PERIOD, AND WHY IT DOES NOT MOVE THE CEILING HERE
//
// Source's CategorizePosition sets m_surfaceFriction to 0.25 while the player is
// airborne with 0 < vz <= 140 over a surface too steep to stand on, and
// AirAccelerate multiplies its accelspeed by that friction. The KZ community
// calls the result a deadstrafe period. It is real, but it only bites if it
// pushes accelspeed BELOW the wishspeed cap, and accelspeed is computed from the
// UNCAPPED wishspeed:
//
//     accelspeed = sv_airaccelerate * maxspeed * tick * surfaceFriction
//
//     CS:GO KZ    accel  12:   46.9 -> 11.7   below 30, so the cap stops binding
//     surf        accel 150:  562.5 -> 140.6  still far above 30: no change
//
// The crossover is around sv_airaccelerate 32 at 250 maxspeed and a 66.7 tick.
// Confirmed in the data: p95 of dE/dt is 38.07 across 69,916 samples inside the
// window and 37.99 across 795,096 outside it, both landing on the theoretical
// 37.50. If the quarter bit here the in-window figure would sit near 30.3.
//
// That measurement shows the achievable GAIN is not reduced. It cannot show
// whether the friction was set, because at these settings both branches predict
// the same ceiling -- but the achievable gain is the only thing this file uses.

#ifndef WR_STRESS_H
#define WR_STRESS_H

#include <math.h>

// sv_air_max_wishspeed. The one Source constant this rests on.
#define WR_AIR_WISHSPEED 30.0f

// Defaults for the two cvars the ceiling depends on. Nothing here reads cvars,
// so these are settings; surf servers run 150, and stock Source is 10.
#define WR_AIR_ACCEL_DEFAULT 150.0f
#define WR_MAXSPEED_DEFAULT 250.0f

// The most energy air strafing can add, in energy units per second.
//
// Two regimes, and which one you are in decides whether the deadstrafe period
// above matters at all. With c = dot(v, wishdir) and a = min(accelspeed, ws-c),
// the per-tick gain in |v|^2 is 2ac + a^2, maximised over c at:
//
//     ws^2               when accelspeed >= ws   -- the wishspeed cap binds
//     A*(2*ws - A)       otherwise               -- acceleration binds
//
// Pass surfaceFriction 0.25 to ask what the deadstrafe period would do.
static inline float WrAirPowerCeilingEx(float gravity, float tickInterval,
                                        float airAccel, float maxSpeed,
                                        float surfaceFriction)
{
    if (gravity < 1.0f) gravity = 1.0f;
    if (tickInterval < 1e-4f) tickInterval = 0.015f;
    if (airAccel < 0.0f) airAccel = 0.0f;
    if (maxSpeed < 0.0f) maxSpeed = 0.0f;
    if (surfaceFriction < 0.0f) surfaceFriction = 0.0f;

    const float ws = WR_AIR_WISHSPEED;
    float a = airAccel * maxSpeed * tickInterval * surfaceFriction;
    float dv2 = (a >= ws) ? (ws * ws) : (a * (2.0f * ws - a));
    return dv2 / (2.0f * gravity * tickInterval);
}

// The common case: full surface friction at the default cvars.
static inline float WrAirPowerCeiling(float gravity, float tickInterval)
{
    return WrAirPowerCeilingEx(gravity, tickInterval, WR_AIR_ACCEL_DEFAULT,
                               WR_MAXSPEED_DEFAULT, 1.0f);
}

// How much speed one tick of air strafing can actually add, done ideally.
//
// Source's AirAccelerate, with the two speeds it uses for two different things:
//
//     wishspd    = min(wishspeed, 30)                   AIR_MAX_WISHSPEED
//     addspeed   = wishspd - dot(velocity, wishdir)
//     accelspeed = sv_airaccelerate * wishspeed * tick  <- the UNCAPPED one
//     gain       = min(accelspeed, addspeed)
//
// Done ideally, dot(velocity, wishdir) is 0 -- that is what wr_stress.h's
// derivation above maximises over -- so addspeed is the full 30 and the gain is
// whichever of the two is smaller.
static inline float WrAirGainPerTick(float tickInterval, float airAccel,
                                     float maxSpeed)
{
    if (tickInterval < 1e-4f) tickInterval = 0.015f;
    if (airAccel < 0.0f) airAccel = 0.0f;
    if (maxSpeed < 0.0f) maxSpeed = 0.0f;

    float a = airAccel * maxSpeed * tickInterval;
    return (a > WR_AIR_WISHSPEED) ? WR_AIR_WISHSPEED : a;
}

// How far your view has to turn each tick to keep strafing ideally, in degrees.
//
// Held perpendicular to the velocity, each tick adds `gain` at a right angle to
// it, so the velocity direction rotates by atan(gain / speed) -- and your view
// has to follow by exactly that much to stay perpendicular to the new one. That
// is the whole derivation, and it is why the ideal turn SLOWS as you speed up:
// the same 30 units a tick buys a smaller and smaller angle.
//
// NOT the formula the strafe-analyzer project uses. Its version is
//
//     accelSpeed = min(tick * 30 * sv_airaccelerate, 30)
//
// which puts the wishspeed cap where sv_maxspeed belongs. The two agree at surf
// settings by coincidence -- both saturate at 30 -- and disagree badly anywhere
// the cap does not bind: at CS:GO KZ's airaccelerate 12 on a 64 tick, theirs
// gives 5.4 units and Source gives min(12 * 250 / 64, 30) = 30. Worth writing
// down, because that is exactly the configuration somebody would reach for this
// to check.
static inline float WrPerfectStrafeDegrees(float speed, float tickInterval,
                                           float airAccel, float maxSpeed)
{
    if (speed < 1.0f)
        speed = 1.0f;
    float gain = WrAirGainPerTick(tickInterval, airAccel, maxSpeed);
    return (float)(atan2((double)gain, (double)speed) * 57.29577951308232);
}

// Past this much of the ceiling on the GAIN side, it was not the player -- it
// was a booster. There is deliberately no matching limit on the loss side; see
// the header. Losing at 350x the ceiling is a wall, and a wall is worth drawing.
#define WR_ETA_NOT_PLAYER 3.0f

// A point with no usable reading: a booster, a window spanning a teleport, or
// the ends of a run where the centred difference has nothing to difference.
// -128 is free -- WrEtaToByte clamps to +-127, so it can never be produced by a
// real measurement.
#define WR_ETA_NO_DATA ((signed char)-128)

// True when dE/dt is too large to have come from a player strafing.
static inline bool WrEtaIsNoData(float dEnergyPerSecond, float ceiling)
{
    if (ceiling <= 1e-6f)
        return true;
    return (dEnergyPerSecond / ceiling) > WR_ETA_NOT_PLAYER;
}

// Efficiency against the ceiling, clamped to [-1, 1] for display. A booster
// returns 0, but callers that can distinguish should ask WrEtaIsNoData first --
// 0 also means free flight, and drawing the two the same is what made this
// unreadable.
static inline float WrEfficiency(float dEnergyPerSecond, float ceiling)
{
    if (ceiling <= 1e-6f)
        return 0.0f;
    float eta = dEnergyPerSecond / ceiling;
    if (eta > WR_ETA_NOT_PLAYER)
        return 0.0f;                    // a booster, not a player
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
// Bucketing, so that neutral is drawable
// ---------------------------------------------------------------------------
//
// The renderer batches by colour, so eta has to be quantised before it becomes
// one. This lived inside the draw loop with an EVEN bucket count over a
// symmetric range, which has no bucket at zero: eta 0 landed on +0.067 and drew
// as a faintly green grey, while the velocity vector -- not bucketed -- drew the
// same eta as true neutral. The two indicators disagreed at the one value a
// player sees most. Out here it can be round-tripped in a test.
//
// n must be ODD for a centre bucket to exist. WrEtaBucket(0, n) == n/2 and
// WrEtaFromBucket(n/2, n) == 0 exactly.

static inline int WrEtaBucket(float eta, int n)
{
    if (n < 3) n = 3;
    if (eta > 1.0f) eta = 1.0f;
    if (eta < -1.0f) eta = -1.0f;
    int b = (int)((eta * 0.5f + 0.5f) * (float)(n - 1) + 0.5f);
    if (b < 0) b = 0;
    if (b >= n) b = n - 1;
    return b;
}

static inline float WrEtaFromBucket(int b, int n)
{
    if (n < 3) n = 3;
    if (b < 0) b = 0;
    if (b >= n) b = n - 1;
    return ((float)b / (float)(n - 1)) * 2.0f - 1.0f;
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
