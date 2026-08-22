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

// Momentum's surf-family HandleDuckingSpeedCrop scales the MOVEMENT COMMAND,
// not mv->m_flMaxSpeed. AirMove therefore hands AirAccelerate 34% of the normal
// uncapped wishspeed while duck is held (or the duck transition/flag is live).
// This matters in the acceleration-limited regime; at the stock surf settings
// both values still reach the 30 u/s air cap.
#define WR_AIR_DUCK_MULTIPLIER 0.34f

// Momentum's NON_JUMP_VELOCITY for non-TF2 modes. CategorizePosition skips its
// ground trace above this speed; at 0 < vz <= 140 over no standable surface it
// instead leaves the player airborne and quarters m_surfaceFriction.
#define WR_DEADSTRAFE_MAX_RISE 140.0f

// Defaults for the two cvars the ceiling depends on. Nothing here reads cvars,
// so these are settings; surf servers run 150, and stock Source is 10.
#define WR_AIR_ACCEL_DEFAULT 150.0f
#define WR_MAXSPEED_DEFAULT 250.0f

// The two live state adjustments AirAccelerate receives. Callers establish
// that air movement is active before asking for the friction; ground friction
// is a different equation and must not be fed into an air-strafe indicator.
static inline float WrAirWishSpeed(float maxSpeed, bool crouched)
{
    if (maxSpeed < 0.0f) maxSpeed = 0.0f;
    return crouched ? maxSpeed * WR_AIR_DUCK_MULTIPLIER : maxSpeed;
}

static inline float WrAirSurfaceFriction(float verticalSpeed)
{
    return (verticalSpeed > 0.0f &&
            verticalSpeed <= WR_DEADSTRAFE_MAX_RISE) ? 0.25f : 1.0f;
}

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
// surfaceFriction is the same factor WrAirPowerCeilingEx takes and for the same
// reason: AirAccelerate's line is
//
//     accelspeed = accel * wishspeed * frametime * player->m_surfaceFriction
//
// and this function used to be the one place in the file that dropped it. It is
// 1 everywhere except the deadstrafe period described in the header, so this
// changes no shipped number -- but two functions modelling one line of engine
// code differently is how they come apart later.
static inline float WrAirAccelerationPerTickEx(float tickInterval,
                                                float airAccel,
                                                float wishSpeed,
                                                float surfaceFriction)
{
    if (tickInterval < 1e-4f) tickInterval = 0.015f;
    if (airAccel < 0.0f) airAccel = 0.0f;
    if (wishSpeed < 0.0f) wishSpeed = 0.0f;
    if (surfaceFriction < 0.0f) surfaceFriction = 0.0f;
    return airAccel * wishSpeed * tickInterval * surfaceFriction;
}

static inline float WrAirGainPerTickEx(float tickInterval, float airAccel,
                                       float maxSpeed, float surfaceFriction)
{
    float a = WrAirAccelerationPerTickEx(tickInterval, airAccel, maxSpeed,
                                         surfaceFriction);
    return (a > WR_AIR_WISHSPEED) ? WR_AIR_WISHSPEED : a;
}

static inline float WrAirGainPerTick(float tickInterval, float airAccel,
                                     float maxSpeed)
{
    return WrAirGainPerTickEx(tickInterval, airAccel, maxSpeed, 1.0f);
}

// DOES THE WISHSPEED CAP BIND? Everything below needs this to be true.
//
// Two regimes, the same pair WrAirPowerCeilingEx splits on. When accelspeed
// reaches the 30-unit cap the cap is what limits the gain, the optimum sits at
// dot(velocity, wishdir) = 0, and the turn rate determines the alignment. When
// it does not, `a` is accelspeed regardless of alignment -- and then the turn
// rate says nothing about how well you are strafing, because it is pinned by the
// acceleration rather than by where you are pointing.
//
// Surf is nowhere near the boundary (562.5 against 30). CS:GO KZ's airaccelerate
// 12 is on the wrong side of it, which is the same configuration this file
// already singles out twice.
static inline bool WrAirCapBinds(float tickInterval, float airAccel,
                                 float maxSpeed, float surfaceFriction)
{
    if (tickInterval < 1e-4f) tickInterval = 0.015f;
    return (airAccel * maxSpeed * tickInterval * surfaceFriction)
           >= WR_AIR_WISHSPEED;
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
static inline float WrPerfectStrafeDegreesEx(float speed, float tickInterval,
                                             float airAccel, float wishSpeed,
                                             float surfaceFriction);

static inline float WrPerfectStrafeDegrees(float speed, float tickInterval,
                                           float airAccel, float maxSpeed)
{
    return WrPerfectStrafeDegreesEx(speed, tickInterval, airAccel, maxSpeed,
                                    1.0f);
}

// The state-aware form. `wishSpeed` is the uncapped value AirMove passes after
// crouch cropping; `surfaceFriction` is normally 1 and is 0.25 only in the
// deadstrafe window. Keeping those separate is essential: the 30 u/s cap is
// applied to wishspd, while both values below multiply the acceleration.
static inline float WrPerfectStrafeDegreesEx(float speed, float tickInterval,
                                             float airAccel, float wishSpeed,
                                             float surfaceFriction)
{
    if (speed < 1.0f)
        speed = 1.0f;
    float gain = WrAirGainPerTickEx(tickInterval, airAccel, wishSpeed,
                                     surfaceFriction);
    return (float)(atan2((double)gain, (double)speed) * 57.29577951308232);
}

// THE SAME THING WHILE TOUCHING A RAMP, WHICH IS NOT THE SAME NUMBER
//
// A surf ramp is air movement: the engine only calls you grounded above
// normal.z 0.7, so on anything steeper AirAccelerate runs every tick exactly as
// it does in free flight. What changes is that TryPlayerMove then clips the
// result back onto the plane, and the clip turns your velocity as well.
//
// One tick, with v already lying in the plane and P = I - n n^T:
//
//     v' = v + g*P(w) - G*P(z)              G = gravity * tick
//
// Work in the horizontal frame where x is along v_h and w is perpendicular to
// it. Gravity contributes nothing along w by itself, but the clip couples it in
// through the plane's own tilt, and the clip also takes a bite out of the gain:
//
//     rotating component = g(1 - wn^2) + G*nz*wn          wn = dot(w, n)
//
// so the ideal view rate is that over the horizontal speed. wn = 0 gives back
// g/speed, which is the flat case above -- the air formula is this one on a
// plane you are not touching.
//
// THIS IS A LARGE CORRECTION, NOT A REFINEMENT. Checked against a literal
// AirAccelerate -> gravity -> ClipVelocity tick at nz = 0.6 and 1000 u/s
// horizontal, ridden across the fall line so wn = +-0.8:
//
//     wn = +0.8    simulated 0.016558486    this 0.016560000    55.2% of air
//     wn = -0.8    simulated 0.005039957    this 0.005040000    16.8% of air
//
// Agreement is 1.5e-6 rad, which is atan against the small-angle form. Reusing
// the flat number on a ramp would overstate the target by between 1.8x and 6x,
// which is why this readout refused to answer on ramps at all until there was a
// real surface normal to hand.
//
// `n` must point AWAY from the surface, out towards the player. wn's sign is
// the whole difference between the two rows above, so an unoriented plane normal
// gives a confidently wrong answer rather than a slightly wrong one.
static inline float WrPerfectStrafeDegreesOnPlaneEx(float speed,
                                                    float tickInterval,
                                                    float airAccel,
                                                    float wishSpeed,
                                                    float surfaceFriction,
                                                    float gravity,
                                                    const float n[3],
                                                    float wn);

static inline float WrPerfectStrafeDegreesOnPlane(float speed,
                                                  float tickInterval,
                                                  float airAccel, float maxSpeed,
                                                  float gravity,
                                                  const float n[3],
                                                  float wn)
{
    return WrPerfectStrafeDegreesOnPlaneEx(speed, tickInterval, airAccel,
                                           maxSpeed, 1.0f, gravity, n, wn);
}

static inline float WrPerfectStrafeDegreesOnPlaneEx(float speed,
                                                    float tickInterval,
                                                    float airAccel,
                                                    float wishSpeed,
                                                    float surfaceFriction,
                                                    float gravity,
                                                    const float n[3],
                                                    float wn)
{
    if (speed < 1.0f)
        speed = 1.0f;
    if (tickInterval < 1e-4f)
        tickInterval = 0.015f;
    if (!n)
        return WrPerfectStrafeDegreesEx(speed, tickInterval, airAccel,
                                        wishSpeed, surfaceFriction);

    if (wn < -1.0f) wn = -1.0f;
    if (wn > 1.0f) wn = 1.0f;

    const float g = WrAirGainPerTickEx(tickInterval, airAccel, wishSpeed,
                                       surfaceFriction);
    const float G = gravity * tickInterval;
    float rot = g * (1.0f - wn * wn) + G * n[2] * wn;
    if (rot < 0.0f) rot = -rot;

    return (float)(atan2((double)rot, (double)speed) * 57.29577951308232);
}

// The steady-state c that a turn-rate ratio implies, scaled for a plane.
//
// WrStrafeQuality's third step sets w*|v|tick = 30 - c and reads off
// c = 30(1 - r). Redo it with the rotating component above and the 30 picks up
// the same two terms:
//
//     g(1-wn^2) + G*nz*wn = r * [30(1-wn^2) + G*nz*wn]
//     c = 30 - g = (1 - r) * (30 + K)          K = G*nz*wn / (1 - wn^2)
//
// K = 0 is the flat case and gives back 30 exactly. On the nz = 0.6 ramp above
// K is about 16, so the same fractional error costs 2.35x as much quality --
// which is right: the ramp is doing part of the turning, so the part you are
// responsible for is more sensitive to getting it wrong.
//
// wn^2 -> 1 is a wall, where no amount of air acceleration rotates anything;
// the guard hands back the flat scale rather than an infinity.
static inline float WrStrafeCScale(float gravity, float tickInterval,
                                   const float n[3], float wn)
{
    if (!n)
        return WR_AIR_WISHSPEED;
    if (tickInterval < 1e-4f)
        tickInterval = 0.015f;
    if (wn < -1.0f) wn = -1.0f;
    if (wn > 1.0f) wn = 1.0f;

    const float denom = 1.0f - wn * wn;
    if (denom < 0.05f)
        return WR_AIR_WISHSPEED;

    float k = (gravity * tickInterval) * n[2] * wn / denom;
    float s = WR_AIR_WISHSPEED + k;
    // A negative scale would flip the grade's sense; a huge one would make every
    // reading zero. Both are outside the geometry this is defined on.
    if (s < 1.0f) s = 1.0f;
    if (s > 8.0f * WR_AIR_WISHSPEED) s = 8.0f * WR_AIR_WISHSPEED;
    return s;
}

// HOW GOOD IS A TURN RATE, ON A SCALE, RATHER THAN IN OR OUT OF A BAND
//
// The readout beside this one shows the measured turn rate against the ideal.
// Colouring it needed a quality, and picking a tolerance would have been an
// invention -- so it comes out of AirAccelerate instead, in three steps.
//
// ONE. The gain, with c = dot(velocity, wishdir) and A = accel*maxspeed*tick:
//
//     a       = min(A, 30 - c)
//     d|v|^2  = 2ac + a^2
//
// TWO. When the cap binds -- WrAirCapBinds, true of every surf configuration --
// a is 30 - c and the whole thing collapses:
//
//     d|v|^2 = 2(30-c)c + (30-c)^2 = 900 - c^2
//
// Maximal at c = 0, worth exactly 900. That is WrAirPowerCeilingEx's ws^2 branch
// arrived at from the other end, which is the check that this is the same model.
//
// THREE. In steady state the view turns with the velocity, so with w* the ideal
// rate and r = w/w*:
//
//     w*|v|*tick = a*cos(theta) ~= 30 - c    ->    c ~= 30*(1 - r)
//
//     quality = (900 - c^2) / 900 = 1 - (1 - r)^2
//
// Symmetric about r = 1. Zero at r = 0 -- not turning at all adds nothing -- and
// zero again at r = 2, where wishdir has swung far enough that it is behind the
// velocity. Note it reproduces the +-20% band it replaced: r in [0.8, 1.2] is
// quality >= 0.96, so the green that shipped before means what it always did and
// there is now a scale either side of it.
//
// THE cos(theta) ~= 1 IS THE ONE APPROXIMATION and it is small: c never exceeds
// 30 while |v| is in the hundreds, so the error is order (c/|v|)^2, which is
// 0.09% at 1000 u/s. Everything else here is exact.
//
// Returns -1 for "no answer" rather than zero, so a caller shows neutral instead
// of a bad grade. Callers must ALSO check WrAirCapBinds -- this function cannot,
// since it is handed two rates and not the three cvars they came from.
// `cScale` is the 30 in step THREE, which is only 30 when the surface is not
// taking a share of the turning. See WrStrafeCScale. Passing WR_AIR_WISHSPEED
// reproduces the flat formula exactly, term for term, and there is a test that
// says so.
static inline float WrStrafeQualityEx(float turnRate, float idealRate,
                                      float cScale)
{
    if (!(idealRate > 1e-3f) || turnRate < 0.0f)
        return -1.0f;
    if (!(cScale > 1e-3f))
        cScale = WR_AIR_WISHSPEED;

    // Normalised before squaring rather than after. c^2 / 900 is the same
    // number in algebra, but this way cScale == WR_AIR_WISHSPEED makes the
    // factor exactly 1.0f and the result BIT-identical to the flat formula this
    // replaced -- so switching to it cannot move a single existing reading.
    float r = turnRate / idealRate;
    float m = (1.0f - r) * (cScale / WR_AIR_WISHSPEED);
    float q = 1.0f - m * m;
    if (q < 0.0f) q = 0.0f;
    if (q > 1.0f) q = 1.0f;
    return q;
}

static inline float WrStrafeQuality(float turnRate, float idealRate)
{
    return WrStrafeQualityEx(turnRate, idealRate, WR_AIR_WISHSPEED);
}

// Reconstruct the turn-rate ratio represented by one tick of horizontal air
// acceleration. A stored path has no view yaw, but dv points along wishdir;
// therefore c = dot(v, wishdir), and in the wishspeed-capped regime the same
// AirAccelerate identity used above gives r = 1 - c/ws.
//
// This is only an AIR identity. A ramp clips the result afterwards and destroys
// the component this inversion needs, so callers must establish that separately.
static inline bool WrStrafeRatioFromAirDelta(float vx, float vy,
                                              float dvx, float dvy,
                                              float *ratioOut)
{
    float d = sqrtf(dvx * dvx + dvy * dvy);
    if (!(d > 0.05f) || !ratioOut)
        return false;
    float c = vx * (dvx / d) + vy * (dvy / d);
    *ratioOut = 1.0f - c / WR_AIR_WISHSPEED;
    return true;
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
