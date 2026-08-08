// wr_energy.cpp  --  see wr_energy.h for the metric and what is approximate.

#include "wr_energy.h"
#include "wr_smooth.h"
#include "wr_stress.h"
#include "wr_budget.h"
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

// And the trend has to leave this band before there is a state to hold. Twelve
// units of height is under a tenth of a jump, so anything inside it is noise
// rather than a direction.
#define ARROW_BAND 12.0f

// Over how long dE/dt is measured. Long enough that a single ramp tick does not
// dominate, short enough to still be about what you are doing now.
#define POWER_WINDOW 0.30f

// Ground detection, kept for the "last jump" line only. It no longer drives the
// reference height -- that was the bug; see the header.
#define GROUND_VZ 30.0f
#define GROUND_SECONDS 0.05f
#define GROUND_Z_SLACK 6.0f
#define JUMP_VZ 150.0f

// A camera-differenced velocity past this is not the player moving.
#define MAX_SANE_SPEED 10000.0f
#define TELEPORT_UNITS 400.0f

// A teleport that lands you back at the anchor is a RESTART -- a fail trigger
// fired, or the restart key was pressed. WrLines reads no game state beyond the
// camera, so this is the only way either of those can be noticed at all.
//
// Loose, because a start zone is a volume and the anchor is one recorded point
// inside it: the run's first sample can easily be a couple of hundred units from
// where the game respawns you.
#define RESTART_UNITS 384.0f

#define HISTORY 240
#define HISTORY_HZ 20

// Below this much height spent, banked/spent is a ratio of two small noisy
// numbers and swings wildly. Costs about two seconds at the top of a map: the
// surf_demise world record has spent 833 units by 5% of the way through.
#define CARRIED_MIN_SPEND 500.0f

static WrVelWindow g_win;
static WrEma g_velX, g_velY, g_velZ;
static WrEma g_speedEma, g_energyEma, g_viewTurnEma, g_velTurnEma, g_accelEma;
static WrEma g_zEma;                // the height alone, so K can be separated
static WrTrendWindow g_trend;
static WrArrow g_arrow;
static WrSwing g_swing;
static bool g_spliced = false;

// The save-loc seed, and the guard-rail that decides whether to keep it.
//
// A seed is a claim about a velocity, made from a file, about a moment nothing
// on this side witnessed. The very next thing that happens is that the velocity
// window fills and MEASURES that velocity independently, about 35 ms later --
// referring to almost the same instant, and therefore the only comparison worth
// making. Half a second later would compare the seed against half a second of
// the player actually playing, which says nothing about the seed at all.
//
// A seed that survives is kept and filtered from. One that does not is thrown
// out, and the filters snap to the measurement exactly as they would have if
// nothing had been seeded -- so a wrong seed costs the 35 ms it was always going
// to cost, and not the third of a second it would cost if it were allowed to
// bleed through the filters.
#define SEED_TOLERANCE_FRAC 0.20f
#define SEED_TOLERANCE_MIN  150.0f      // u/s; gravity over 35 ms is 28
static float g_seedEnergy = 0.0f, g_seedSpeed = 0.0f;
static bool g_seedPending = false;      // a seed is waiting for its first check
static bool g_seedChecked = false;      // a result exists to report
static bool g_seedRejected = false;
static float g_seedErr = 0.0f, g_seedSpeedErr = 0.0f;
static int g_seedCount = 0, g_seedRejects = 0;

// After a discontinuity, how long the output filter is given to converge before
// the gain/loss accumulator is allowed to bank anything. Three time constants is
// 95% converged.
#define SETTLE_TAUS 3.0f
static float g_settleFor = 0.0f;

// How long the camera must be bit-identical before the readout is held. Two
// frames at any rate would do -- a moving camera never repeats a float -- but a
// little more than that costs nothing and cannot be tripped by a single
// duplicated update.
#define HOLD_SECONDS 0.05f
static float g_stillFor = 0.0f;
static bool g_held = false;
static bool g_haveReading = false;  // a real velocity has been measured since
                                    // the last reset or teleport

static float g_clock = 0.0f;

static bool g_valid = false;
static Vec3 g_vel;

// The consistent pair: the RAW window velocity and the position at the window's
// MIDPOINT, which is the instant that velocity actually refers to. g_now is
// built from these two and not from g_vel, and anything else that wants to
// compute an energy has to use them for the same reason. See WrEnergySampleAt.
static Vec3 g_sampleMid;
static Vec3 g_sampleRaw;
static bool g_sampleOk = false;
static Vec3 g_lastPos;
static bool g_havePos = false;      // g_lastPos holds a real position
static bool g_restart = false;      // a teleport landed back at the anchor
static bool g_teleported = false;   // any teleport, consumed once
static Vec3 g_teleportAt;           // where it landed
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
static bool g_haveLastSpeed = false;
static float g_viewTurn = 0.0f, g_velTurn = 0.0f, g_speedRate = 0.0f;

// Displayed value, quantised with hysteresis so the last digit stops churning.
static float g_shown = 0.0f;
static bool g_haveShown = false;
static float g_zSmooth = 0.0f;
static float g_shownSpent = 0.0f;
static bool g_haveShownSpent = false;
static float g_shownCarried = 0.0f;
static bool g_haveShownCarried = false;

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

    // Exactly the values the #defines above held, so turning these into settings
    // changes nothing until one is moved.
    g_energy.velWindowSeconds = VEL_WINDOW_SECONDS;
    g_energy.velTau = VEL_TAU;
    g_energy.speedTau = SPEED_TAU;
    g_energy.powerSeconds = POWER_WINDOW;
    g_energy.arrowBand = ARROW_BAND;
    g_energy.anchorToRunStart = true;
    g_energy.hudMode = WR_HUD_NET;
    g_energy.airAccelerate = WR_AIR_ACCEL_DEFAULT;
    g_energy.maxSpeed = WR_MAXSPEED_DEFAULT;

    g_energy.showBar = true;
    g_energy.barMode = WR_BAR_ENERGY;
    // Measured across 28,243 matched samples on every map with at least ten
    // clean main-track runs: |energy gap| against the fastest run has a 90th
    // percentile of 701, |speed gap| one of 207. So the bar fills at about the
    // gap you see one time in ten, and the rest of the range is legible.
    g_energy.barMaxEnergy = 700.0f;
    g_energy.barMaxSpeed = 200.0f;
    g_energy.barHeight = 6.0f;

    // On by default. The gap is the number people actually steer by, and it
    // was being read off a point nobody could see.
    g_energy.showComparePoint = true;
    g_energy.comparePointLeader = true;
}

void WrEnergyCycleHudMode(void)
{
    g_energy.hudMode = (g_energy.hudMode + 1) % WR_HUD_MODE_COUNT;
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
    WrEmaReset(&g_speedEma); WrEmaReset(&g_energyEma); WrEmaReset(&g_zEma);
    WrEmaReset(&g_viewTurnEma); WrEmaReset(&g_velTurnEma); WrEmaReset(&g_accelEma);
    WrTrendReset(&g_trend);
    WrArrowReset(&g_arrow);
    WrSwingReset(&g_swing, WR_SWING_HYSTERESIS);
    g_settleFor = 0.0f;
    g_spliced = false;
    g_haveShownSpent = false;
    g_haveShownCarried = false;
    g_seedPending = false;
    g_seedChecked = false;      // a new map's Diagnostics starts with no claim
    g_seedRejected = false;
    g_seedCount = g_seedRejects = 0;

    g_valid = false;
    g_havePos = false;
    g_haveReading = false;
    g_stillFor = 0.0f;
    g_held = false;
    g_restart = false;
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
    g_haveLastSpeed = false;
    g_haveVelDir = false;
    g_haveShown = false;
    g_viewTurn = g_velTurn = g_speedRate = 0.0f;
    g_vel = WrVec(0.0f, 0.0f, 0.0f);
}

// Everything a re-anchor has to forget.
//
// The peak and the start reference were being cleared; the gain/loss swing
// accumulator was not, so re-anchoring mid-session left "gained" and "lost"
// spanning two attempts and quietly growing for ever. The restart path at the
// bottom of WrEnergySample has always done this properly -- these two had
// simply drifted out of step with it.
static void ForgetForNewAttempt(void)
{
    g_peak = 0.0f;
    WrSwingReset(&g_swing, WR_SWING_HYSTERESIS);
    g_spliced = false;
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
    ForgetForNewAttempt();
    WrLogf("energy: anchored here, z %.0f", g_refZ);
}

// The same as anchoring to a run's start, but sourced from the fitted start zone
// rather than from whichever run happens to be nearest this frame.
//
// A separate entry point rather than a parameter on WrEnergyAnchorToFeet,
// because that function has other callers including the test harness, and
// because the difference is worth being able to see in the panel: one of these
// is a guess derived from one run, the other from every run on the leg.
void WrEnergyAnchorToStartZone(const Vec3 &centre)
{
    g_refZ = centre.z + g_energy.eyeHeight;
    g_refPos = centre;
    g_haveRef = true;
    g_refSource = WR_ANCHOR_START_ZONE;
    if (g_valid)
    {
        g_start = g_now;
        g_haveStart = true;
    }
    ForgetForNewAttempt();
    WrLogf("energy: anchored to the fitted start zone (%.0f %.0f %.0f)",
           centre.x, centre.y, centre.z);
}

void WrEnergyAnchorToFeet(const Vec3 &feet)
{
    // A run's points are the player origin. Ours is the eye, about 64 units
    // higher, so without this the whole readout is offset by a player's height.
    g_refZ = feet.z + g_energy.eyeHeight;
    g_refPos = feet;
    g_haveRef = true;
    g_refSource = WR_ANCHOR_RUN_START;
    ForgetForNewAttempt();
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

// A teleport is a discontinuity, not movement. Everything carrying state across
// frames is DROPPED rather than filtered: a filter run across a discontinuity
// produces a number that was never true at either end, and here that would be a
// three-tenths-of-a-second glide from the energy you had when you failed down to
// the energy you have back on the pad.
static void Teleported(const Vec3 &pos)
{
    // Published so there is exactly one teleport detector in the tool. The run
    // clock used to keep its own, and the two disagreed on a slow load.
    g_teleported = true;
    g_teleportAt = pos;

    WrVelReset(&g_win);
    WrEmaReset(&g_velX); WrEmaReset(&g_velY); WrEmaReset(&g_velZ);
    WrEmaReset(&g_speedEma); WrEmaReset(&g_energyEma); WrEmaReset(&g_accelEma);
    WrEmaReset(&g_velTurnEma); WrEmaReset(&g_zEma);
    WrTrendReset(&g_trend);
    WrArrowReset(&g_arrow);

    // The accumulators are SEEDED at the far end, not stepped across the gap. A
    // save-loc load a thousand units down the map is not a thousand units of
    // energy thrown away, and stepping would record exactly that -- the harness
    // runs both and shows the difference.
    //
    // Seeding once at the first post-teleport sample is not enough either. That
    // sample is the output of a filter that has just been reset, so it carries
    // whatever error the first real window happened to have; as the filter then
    // converges towards the truth, the difference gets banked as a genuine gain
    // or loss. Measured at 550 units of phantom gain on a scripted save-loc
    // load. So the pivot is dragged along with the value until the filter has
    // settled, and only then does it start banking.
    g_swing.have = false;
    g_settleFor = SETTLE_TAUS * g_energy.smoothSeconds;
    g_spliced = true;

    g_settledFor = 0.0f;
    g_onGround = false;
    g_haveVelDir = false;
    g_haveShown = false;
    g_haveShownSpent = false;
    g_haveShownCarried = false;
    g_historyCount = 0;

    // These were missed the first time round, and each produced a spike of its
    // own on the frame after a teleport: an acceleration differenced against the
    // speed you had before the jump, and a view turn rate differenced against
    // the angle you were facing before it. Cleared as "no previous value" rather
    // than as zero -- differencing against a zero you never had is the same bug
    // with a different sign.
    g_haveLastSpeed = false;
    g_haveFwd = false;
    g_haveReading = false;      // nothing here yet to hold

    // Abandon a seed still waiting for its check. A second teleport before the
    // window filled means the measurement that would have judged it is now about
    // somewhere else entirely, and scoring the seed against that would be a lie
    // about the one number here whose whole job is to be checkable.
    g_seedPending = false;
    g_stillFor = 0.0f;
    g_held = false;
    g_speedRate = g_viewTurn = g_velTurn = 0.0f;

    if (!g_haveRef)
        return;

    // Horizontally only. g_refPos is a player origin for a run-start anchor and
    // a camera position for a manual one, so a vertical term would mean two
    // different things -- the same asymmetry WrTimerTick works around.
    float dx = pos.x - g_refPos.x, dy = pos.y - g_refPos.y;
    if (sqrtf(dx * dx + dy * dy) >= RESTART_UNITS)
        return;

    g_restart = true;
    g_haveStart = false;    // re-seeds from the first sample of the new attempt
    g_peak = 0.0f;
    // A new attempt gets new totals, the same way the clock goes back to zero.
    WrSwingReset(&g_swing, WR_SWING_HYSTERESIS);
    g_spliced = false;
    WrLogf("energy: teleported back to the anchor, treating it as a restart");
}

// ---------------------------------------------------------------------------
// Landing on a save-loc with the answer already in hand
// ---------------------------------------------------------------------------
//
// Teleported() above resets every filter, and it is right to: a velocity got by
// differencing camera positions cannot be carried across a discontinuity, so the
// only honest thing to do is throw it away and measure again. That costs about a
// third of a second of the headline figure climbing back to the truth, and 0.9 s
// before anything is banked.
//
// A save-loc load is the one discontinuity where the far side is written down.
// Momentum's savedlocs.txt records the velocity it is about to restore -- present
// and finite in all 3239 save-locs on this machine, with 62% of them saved above
// 250 u/s, which is exactly where the climb is most visible. So instead of
// re-deriving a number the game already told us, start from it.
//
// WHY THIS TAKES A POSITION RATHER THAN USING THE FILE'S
//
// The file stores the player ORIGIN -- the feet -- and 78 of those 3239 were
// saved crouched, with duckAmount missing entirely from 552 of them. Recovering
// an eye height from that means modelling the duck. The camera position on the
// landing frame needs no modelling: it is what every other energy figure in this
// file is measured from, and it is already correct. Energy is a height, and
// taking z from the file would put a 64-unit error into the one term that is
// supposed to be exact.
//
// WHO MAY CALL THIS
//
// WrTimerTick, and only WrTimerTick. Not because of the teleport flags -- this
// takes no flag and clears nothing -- but because WrTimerTick is the one place
// that has already consumed them and matched the landing spot to a save-loc.
// Seeding on a teleport that is NOT a save-loc load would be inventing a
// velocity, which is the failure this whole file is arranged to avoid.
void WrEnergySeed(const Vec3 &camPos, const Vec3 &vel, const char *why)
{
    if (!WrSaneVec(camPos) || !WrSaneVec(vel) || WrLength(vel) > MAX_SANE_SPEED)
        return;                 // fall through to measuring it, as before

    float energy = WrEnergyOf(camPos, vel);
    float speed = WrLength(vel);

    g_vel = vel;
    WrEmaSeed(&g_velX, vel.x);
    WrEmaSeed(&g_velY, vel.y);
    WrEmaSeed(&g_velZ, vel.z);

    g_speed = speed;
    WrEmaSeed(&g_speedEma, speed);

    g_now = energy;
    g_nowSmooth = energy;
    WrEmaSeed(&g_energyEma, energy);

    g_zSmooth = camPos.z;
    WrEmaSeed(&g_zEma, camPos.z);

    // The live recorder stores whatever pair this publishes, so it has to agree
    // with the readout rather than nearly agree.
    g_sampleMid = camPos;
    g_sampleRaw = vel;
    g_sampleOk = true;

    // There is something real to show, from this frame.
    //
    // g_valid only. NOT g_haveReading, which looks like it belongs here and does
    // not: it arms the bit-identical hold test, and the comment on that test
    // names this exact state as the reason it is gated. With no measured reading
    // yet, a repeated camera frame has to be PUSHED so the window can fill --
    // and 550 of the 3239 save-locs here were saved standing still, where the
    // camera legitimately repeats. Arming the hold from a seed would freeze the
    // readout on the value it was seeded with and stop the window ever filling,
    // so nothing would ever confirm or correct it.
    g_valid = true;

    // The pivot starts where the number does, so the first banked change is
    // measured against the truth rather than against zero.
    WrSwingSeed(&g_swing, energy);

    // One tau, not three.
    //
    // The long hold exists because a filter converging on a fresh value would
    // otherwise bank the convergence itself as gained energy. Here there is
    // nothing to converge -- but the velocity window still needs about 30 ms to
    // span, and the match that produced this velocity could have picked the
    // wrong save-loc. One tau lets the first measured samples confirm the seed
    // before anything is committed to the totals.
    g_settleFor = g_energy.smoothSeconds;
    g_spliced = true;

    // Checked against the very next measurement -- see the guard-rail in
    // WrEnergySample. Nothing on this side can verify that the game applies the
    // velocity its own file records, so this does not assert it, it measures it,
    // and throws the seed away if it was wrong.
    g_seedEnergy = energy;
    g_seedSpeed = speed;
    g_seedPending = true;

    WrLogf("energy: seeded from %s -- %.0f u/s, energy %.0f", why ? why : "a save-loc",
           speed, energy);
}

// What the seeds on this map turned out to be worth. False until one has been
// checked, so the panel says nothing rather than reporting a zero it has not
// earned.
bool WrEnergySeedReport(WrEnergySeedInfo *out)
{
    if (!g_seedChecked || !out)
        return false;
    out->seedEnergy = g_seedEnergy;
    out->seedSpeed = g_seedSpeed;
    out->energyErr = g_seedErr;
    out->speedErr = g_seedSpeedErr;
    out->rejected = g_seedRejected;
    out->seeds = g_seedCount;
    out->rejects = g_seedRejects;
    return true;
}

bool WrEnergyTakeRestart(void)
{
    bool r = g_restart;
    g_restart = false;
    return r;
}

bool WrEnergyHeld(void) { return g_held; }

bool WrEnergyTakeTeleport(Vec3 *landedAt)
{
    bool t = g_teleported;
    g_teleported = false;
    if (t && landedAt)
        *landedAt = g_teleportAt;
    return t;
}

void WrEnergySample(const Vec3 &pos, float dt)
{
    if (!WrSaneVec(pos) || !(dt > 0.0f) || dt > 0.5f)
        return;

    // Where you were last frame, recorded BEFORE any of the early returns below,
    // and that ordering is the whole fix.
    //
    // This used to be written only on the success path at the bottom of the
    // function, which made a fail trigger freeze the readout permanently. The
    // sequence: you fail, the teleport is detected, the velocity window is
    // emptied; the window then holds a single sample, so WrVelEstimate returns
    // false; the function returns early without recording where you now are. The
    // next frame therefore compares your new position against the PRE-teleport
    // one, sees a jump over 400 units again, and empties the window again --
    // every frame, for the rest of the map.
    //
    // The visible symptom was the number sticking at whatever it read the
    // instant you failed. Since you usually fail at the bottom of the map while
    // the anchor is the start pad, that stuck value was a large negative one,
    // and nothing but the Reset button cleared it -- Reset works only because it
    // clears the flag this test used to be gated on.
    bool havePrev = g_havePos;
    Vec3 prev = g_lastPos;
    g_lastPos = pos;
    g_havePos = true;

    // AN UNCHANGED CAMERA IS NOT A SAMPLE.
    //
    // Watching a demo, pausing it stops the camera dead while frames keep being
    // presented. Feeding those frames in reads as "the player instantaneously
    // stopped", so the whole kinetic term -- thousands of units at surf speed --
    // drains out of the readout over the next third of a second, the arrow
    // latches to falling, and the swing accumulator banks a leg on the pause and
    // another on the unpause. That is the reported "the numbers freak out when
    // you pause".
    //
    // Bit-identical is the test, not "nearly still". A camera that is being
    // written every tick never repeats a float exactly; one that is not being
    // written repeats it forever. Standing still in game reads as held too,
    // which costs nothing -- if the camera is not moving there is genuinely
    // nothing to measure, and the held value is the correct one.
    //
    // Nothing at all advances here, including the ring's own clock. That is what
    // makes the resume seamless rather than a step: the window's newest sample
    // is still the pre-pause one, so when movement resumes the next difference
    // spans real positions over a real interval with the pause simply excised.
    // The return is unconditional, and the timer only decides when to SAY so.
    // Waiting even a few frames before skipping does not work: the velocity
    // window is 40 ms, so eight identical frames at 200 fps fill it completely
    // and the reading has already collapsed by the time any threshold fires.
    // Measured on a scripted pause, arming over 50 ms lost 435 of 3595 units
    // before the hold engaged.
    //
    // Skipping a lone repeated frame is right in its own terms anyway -- at a
    // frame rate above the tick rate the camera genuinely has nothing new to
    // say on some frames, and that is not a pause.
    // Gated on already having a reading to hold, and that is not a detail. With
    // no reading yet -- at spawn, or having just landed somewhere from a
    // save-loc -- skipping repeats would stop the window ever filling, so a
    // player standing perfectly still would never get a readout at all. In that
    // state the repeats are pushed, the window fills with identical positions,
    // and the honest answer falls out: zero velocity, energy is height alone.
    if (g_haveReading && havePrev &&
        pos.x == prev.x && pos.y == prev.y && pos.z == prev.z)
    {
        g_stillFor += dt;
        if (g_stillFor >= HOLD_SECONDS)
            g_held = true;
        return;
    }
    g_stillFor = 0.0f;
    g_held = false;

    g_clock += dt;

    if (havePrev && WrDist(prev, pos) > TELEPORT_UNITS)
        Teleported(pos);

    WrVelPush(&g_win, pos.x, pos.y, pos.z, dt);

    float rx = 0.0f, ry = 0.0f, rz = 0.0f;
    float mx = 0.0f, my = 0.0f, mz = 0.0f;
    if (!WrVelEstimate(&g_win, g_energy.velWindowSeconds,
                       &rx, &ry, &rz, &mx, &my, &mz))
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

    // --- the save-loc seed's guard-rail -------------------------------------
    //
    // This is the first independently measured velocity since a seed was
    // planted, and it is deliberately checked HERE: before the EMA steps below,
    // so that throwing the seed out leaves them unseeded and they snap to this
    // measurement, which is exactly what an unseeded load does.
    //
    // Speed, not energy. The file states a velocity and this measures a
    // velocity; energy is quadratic in it, so a band on energy would have to be
    // derived from a band on speed anyway. The window spans about 35 ms starting
    // at the landing, so the honest residual is small: gravity contributes 28
    // u/s over that, and air acceleration not much more.
    if (g_seedPending)
    {
        float measured = WrLength(raw);
        float dv = measured - g_seedSpeed;
        if (dv < 0.0f) dv = -dv;
        float allow = g_seedSpeed * SEED_TOLERANCE_FRAC;
        if (allow < SEED_TOLERANCE_MIN) allow = SEED_TOLERANCE_MIN;

        g_seedPending = false;
        g_seedChecked = true;
        g_seedCount++;
        g_seedErr = WrEnergyOf(mid, raw) - g_seedEnergy;
        g_seedSpeedErr = measured - g_seedSpeed;
        g_seedRejected = (dv > allow);

        if (g_seedRejected)
        {
            g_seedRejects++;
            WrEmaReset(&g_velX); WrEmaReset(&g_velY); WrEmaReset(&g_velZ);
            WrEmaReset(&g_speedEma); WrEmaReset(&g_energyEma); WrEmaReset(&g_zEma);
            WrTrendReset(&g_trend);
            g_swing.have = false;
            // And back to the full hold, because from here this is an ordinary
            // unseeded teleport and the reasoning for three taus applies again.
            g_settleFor = SETTLE_TAUS * g_energy.smoothSeconds;
            WrLogf("energy: seed REJECTED -- the file said %.0f u/s, the first "
                   "measurement says %.0f. Measuring it instead.",
                   g_seedSpeed, measured);
        }
    }

    g_vel.x = WrEmaStep(&g_velX, raw.x, dt, g_energy.velTau);
    g_vel.y = WrEmaStep(&g_velY, raw.y, dt, g_energy.velTau);
    g_vel.z = WrEmaStep(&g_velZ, raw.z, dt, g_energy.velTau);

    float instSpeed = WrLength(g_vel);
    g_speed = WrEmaStep(&g_speedEma, instSpeed, dt, g_energy.speedTau);

    g_valid = true;
    g_haveReading = true;

    // The RAW window velocity paired with the window's MIDPOINT height, not the
    // smoothed velocity paired with the current height. Both then refer to the
    // same instant, which is what makes E correct while accelerating: measured
    // 20 ms apart, a ballistic arc appeared to lose 46 units of energy over 1.6 s
    // of free fall, and energy is conserved in free fall. See WrVelEstimate.
    g_now = WrEnergyOf(mid, raw);

    // Published for the live recorder, which must store the same pair. It used
    // to be handed the CURRENT feet and the SMOOTHED velocity -- two instants
    // about 80 ms apart -- and every energy computed from a live point was
    // wrong by whatever the trajectory did in between. See WrEnergySampleAt.
    g_sampleMid = mid;
    g_sampleRaw = raw;
    g_sampleOk = true;

    // The headline figure is filtered here rather than at the point of display,
    // so the arrow, the peak and the plot all agree with what is on screen.
    g_nowSmooth = WrEmaStep(&g_energyEma, g_now, dt, g_energy.smoothSeconds);
    WrTrendPush(&g_trend, g_nowSmooth, dt);

    // The height alone, through the SAME filter and from the SAME instant --
    // mid.z, not pos.z. Because an EMA is linear, subtracting it from the
    // filtered energy gives exactly the filtered kinetic term, so the budget
    // numbers agree with the headline figure rather than nearly agreeing.
    g_zSmooth = WrEmaStep(&g_zEma, mid.z, dt, g_energy.smoothSeconds);

    // While the filter is still converging on a fresh value, drag the pivot
    // along instead of measuring against it. Banking nothing is the correct
    // answer for a stretch of time the player did not actually play.
    if (g_settleFor > 0.0f)
    {
        g_settleFor -= dt;
        WrSwingSeed(&g_swing, g_nowSmooth);
    }
    else
    {
        WrSwingStep(&g_swing, g_nowSmooth);
    }

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
    if (dt > 1e-5f && g_haveLastSpeed)
    {
        g_speedRate = WrEmaStep(&g_accelEma, (instSpeed - g_lastSpeedForRate) / dt,
                                dt, ACCEL_TAU);
    }
    g_lastSpeedForRate = instSpeed;
    g_haveLastSpeed = true;

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

bool WrEnergySampleAt(Vec3 *feet, Vec3 *vel)
{
    if (!g_sampleOk)
        return false;
    if (feet)
    {
        *feet = g_sampleMid;
        feet->z -= g_energy.eyeHeight;
    }
    if (vel)
        *vel = g_sampleRaw;
    return true;
}

// dE/dt, for the efficiency figure. Taken over a window rather than per frame,
// because a derivative of a noisy signal is noise. POWER_WINDOW is the default
// the setting starts at, and the fallback if it is ever driven to zero.
float WrEnergyPower(void)
{
    float w = g_energy.powerSeconds > 1e-3f ? g_energy.powerSeconds : POWER_WINDOW;

    // Divided by the span the ring COULD give, not by the one that was asked
    // for. The trend ring is 256 samples: at 300 fps it spans 0.85 s, so a
    // longer window comes back as the change over 0.85 s -- and dividing that
    // by 1.5 would report a rate 43% low, silently, and only at high frame
    // rates. The two agree whenever the window fits, which is nearly always.
    float span = 0.0f;
    float d = WrTrendOverSpan(&g_trend, w, &span);
    if (span < 1e-3f)
        return 0.0f;
    return d / span;
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

bool WrEnergyBudgetNow(WrEnergyBudget *out)
{
    if (!g_valid || !g_haveRef)
        return false;
    if (!out)
        return true;

    // `wasted` is taken as the NEGATED headline figure rather than computed
    // again, so the two can never disagree on screen by a rounding step -- they
    // are the same number. `banked` is then derived, which makes all three add
    // up exactly whatever the quantiser does.
    float rawSpent = g_refZ - g_zSmooth;
    g_shownSpent = WrQuantise(g_shownSpent, rawSpent, g_energy.quantiseStep,
                              &g_haveShownSpent);

    out->spent = g_shownSpent;
    out->wasted = -WrEnergyRelative();
    out->banked = out->spent - out->wasted;

    out->carriedValid = (rawSpent > CARRIED_MIN_SPEND);
    if (out->carriedValid)
    {
        float raw = (out->banked / rawSpent) * 100.0f;
        // A whole percent, with the same hysteresis the other figures get.
        g_shownCarried = WrQuantise(g_shownCarried, raw, 1.0f,
                                    &g_haveShownCarried);
        out->carried = g_shownCarried;
    }
    else
    {
        out->carried = 0.0f;
        g_haveShownCarried = false;
    }
    return true;
}

float WrEnergyGained(void)
{
    float g = 0.0f;
    WrSwingTotals(&g_swing, &g, NULL);
    return g;
}

float WrEnergyLost(void)
{
    float l = 0.0f;
    WrSwingTotals(&g_swing, NULL, &l);
    return l;
}

bool WrEnergyBudgetSpliced(void) { return g_spliced; }
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
    WrArrowStep(&g_arrow, WrEnergyTrend(), g_energy.arrowBand, dt, ARROW_HOLD);
}
