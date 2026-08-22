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
static WrEma g_yawTurnEma;      // yaw alone, for the strafe readout
static WrEma g_yawSignedEma;    // and again with its sign, for the ramp ideal
static WrEma g_zEma;                // the height alone, so K can be separated
static WrTrendWindow g_trend;

// The strafe gauge's ring and its 20 Hz clock. See the push in WrEnergySample
// for why it cannot share g_trend.
#define GAUGE_INTERVAL 0.05f
static WrTrendWindow g_gauge;
static float g_gaugeClock = 0.0f;

// Vertical velocity per frame, for telling whether you are touching anything.
//
// THE ONE LIVE READING THAT SURVIVED THE CAMERA. wr_stress.h records that
// efficiency did not: differencing the camera gives a velocity good to a few
// percent, and against a 37 units/s ceiling that agreed with the truth 45% of
// the time and pointed the WRONG WAY 26%. Contact is a different size of signal
// entirely -- a surface changes the vertical acceleration by hundreds of units
// per second squared, where air strafing changes it by nothing at all.
//
// Measured the same way, by resampling 250 real runs at 200 Hz, adding view bob
// and pushing them through the real wr_smooth.h estimator:
//
//     window  tol   agrees   missed a ramp   invented one
//     0.10    150   90.6%    0.1%            9.2%
//     0.10    250   92.9%    0.3%            6.8%
//     0.10    320   93.6%    0.4%            5.9%
//
// And view bob does not matter here either: 92.9% with two units of it, 93.1%
// with none. Same finding as the efficiency work, reached independently.
//
// Re-derive any of it with tests\phase_sweep.exe --live.
//
// The error is one-sided in the direction that matters. It essentially never
// misses a ramp; what it does, a few percent of the time, is claim a surface
// during free flight. A readout that occasionally says "ramp" a moment early is
// a different kind of wrong from one that stays silent through a whole ramp.
#define PHASE_LIVE_WINDOW 0.10f
#define PHASE_LIVE_TOL 250.0f
static WrTrendWindow g_vzTrend;

// What the map said about the player's surroundings this frame.
//
// WR_GEOM_UNKNOWN unless somebody pushes an answer in, and nothing outside the
// DLL does -- so every harness that links this file gets exactly the kinematic
// behaviour that was measured at 92.9%, with no geometry attached.
static int g_geomTouch = WR_GEOM_UNKNOWN;
static float g_geomNormal[3] = { 0.0f, 0.0f, 0.0f };
static bool g_geomHaveNormal = false;

// AND THE BEST RAMP CANDIDATE, WHICH IS A DIFFERENT ANSWER.
//
// g_geomNormal is the nearest surface of any facing, because that is the
// question the touch verdict was measured on. A board wants the nearest surface
// it could have RIDDEN, and on a side entry beside a wall those are not the
// same polygon -- the wall wins on distance and the board is then refused for
// not being a ramp, silently, which is the defect this pair exists to end.
//
// Four floats: the plane's normal, then its own distance, so the board can
// solve where the crossing was rather than assume when it happened.
static float g_geomRampPlane[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
static bool g_geomHaveRamp = false;
static float g_geomRampDist = -1.0f;
static float g_geomNearDist = -1.0f;

// HOW MUCH FURTHER THE RAMP CANDIDATE MAY BE THAN THE NEAREST SURFACE.
//
// Preferring the ramp unconditionally fixes the reported defect and introduces
// a smaller one going the other way. Land on a FLOOR that happens to have a
// ramp twenty units off, and there is a rideable plane in range, and the board
// would be graded against a surface the player never touched -- a board
// invented where the old code merely stayed quiet.
//
// The case being rescued is a hull straddling a junction, where the ramp and
// whatever beat it to the query meet and are both within a few units of the
// feet. Twelve is inside the player hull's own half-width of sixteen, so it
// covers that and excludes a surface across the room.
//
// This one is a judgement rather than a measurement: the corpus that could
// settle it is live geometry queries against recorded runs, which is
// bsp_sweep's territory and not something the .wrpath library can answer.
#define GEOM_RAMP_PREFER 12.0f

// ---------------------------------------------------------------------------
// Live boards
// ---------------------------------------------------------------------------
//
// See WrEnergyBoard in the header for what this is and why it is not a port of
// the demo detector. The mechanism here is only the bookkeeping.
//
// HOW FAR BACK TO REACH FOR THE ARRIVING VELOCITY
//
// The phase test reads vertical acceleration over PHASE_LIVE_WINDOW (0.10 s), so
// by the time it says "contact" the clip is already inside its window and the
// velocity now is part clipped. Reaching back one whole window puts the sample
// before the event with the window's own margin -- and no further, because in
// 0.10 s of free fall gravity alone has added 80 u/s of downward speed, and
// charging that to the board would read every landing as worse than it was.
// ...AND WHY THAT IS ONLY THE FALLBACK NOW.
//
// Reaching back a fixed distance was always an estimate of an instant nobody
// had measured, and it has three defects that all point the same way. The
// detector's own lag is not fixed -- it is a threshold crossing on a smoothed
// signal, so contact is located to perhaps +-50 ms, and 50 ms of free fall is
// 40 u/s of vertical speed unaccounted for. The walk below returned the first
// sample PAST the threshold rather than the sample AT it, so the age came out
// in [back - dt, back) and the answer depended on the frame rate: 13 u/s of
// gravity at 60 fps against 2.7 at 300. And nothing corrected for the gravity
// that did accumulate between the sample and the surface.
//
// Where the map can offer the plane, none of that is necessary. The player was
// in free flight -- that is what AIR means -- so the crossing of a known plane
// by a known parabola has a closed form, and the velocity there is exact up to
// the estimator's own noise. WrBoardEntry solves it and falls back to this
// constant, gravity-corrected, only when it cannot bracket a crossing.
#define BOARD_LOOKBACK 0.10f

// How far back the solve is allowed to look for the crossing. Generous against
// the detector's worst lag (about 180 ms end to end) and short enough that it
// cannot reach past the previous ramp -- beyond this the free-flight model it
// rests on is no longer describing what happened.
#define BOARD_SOLVE_REACH 0.35f

// How far off the plane a sample has to be before its SIDE means anything. A
// player riding a ramp sits within a fraction of a unit of it and the sign
// there is float noise; two units is past every source of that and well inside
// one frame of approach at any speed worth grading.
#define BOARD_SOLVE_CLEAR 2.0f

// About 0.85 s at 300 fps, which is far more than BOARD_LOOKBACK needs. The ring
// is sized for the frame rate rather than the window so that a stall cannot walk
// the read off the end of it.
#define BOARD_RING 256

static Vec3  g_bRingV[BOARD_RING];
static Vec3  g_bRingP[BOARD_RING];      // the FEET the velocity belonged to
static float g_bRingDt[BOARD_RING];
static int   g_bRingHead = 0, g_bRingCount = 0;

// The game's own pair, when the scanner has proved one. Both or neither -- see
// WrEnergySetTruePlayer. `g_trueLive` is what the panel reports, and it is a
// property of the last frame rather than of the last board.
static Vec3 g_trueOrigin, g_trueVel;
static bool g_trueLive = false;
static float g_trueEye = -1.0f;

// The transition detector. A board is AIR that has lasted, then contact -- the
// same shape as the demo rule (PHASE_BOARD_AIR_BEFORE), expressed in seconds
// because live has no ticks.
#define BOARD_AIR_BEFORE 0.10f

// AND A GRACE ON IT, because one frame used to be able to cost a whole board.
//
// The air accumulator was zeroed on every contact frame including refused ones,
// so a single fake contact -- and the phase readout fakes one on about 0.4% of
// frames -- emptied it, and the genuine landing 50 ms later then failed
// BOARD_AIR_BEFORE and reported nothing. Air that has already been earned is
// kept for this long across a contact that produced no board, which cannot
// manufacture a board on a sustained ride: a ride is contact frame after
// contact frame, and the grace is spent by the second one.
#define BOARD_AIR_GRACE 0.05f

static int   g_bPrevPhase = WR_PHASE_UNKNOWN;
static float g_bAirFor = 0.0f;
static float g_bAirGrace = 0.0f;

// WHY THE LAST LANDING PRODUCED NOTHING.
//
// Thirteen tests stand between a landing and a number and every one of them
// used to fail into the same grey "(none yet)", which reads as "you have not
// boarded" rather than "I refused". Reported as boards that "sometimes don't
// report numbers" -- the honest answer to which is that they were reported and
// then thrown away, and the tool knew which test did it every single time.
//
// This is the same treatment the strafe row got when it printed "no surface"
// on a map it had never read.
static int   g_bWhy = WR_BOARD_WHY_NONE;
static int   g_bWhyCount[WR_BOARD_WHY__COUNT] = { 0 };
static float g_bWhyNz = 0.0f;           // what the refused plane actually was

static WrBoardStats g_bLast;
static bool  g_bHave = false;
static float g_bAge = 0.0f;
static bool  g_bAvailable = false;
static bool  g_bExact = false;          // the entry was solved, not guessed

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

// How far the camera has to leave the landing spot before a seed is judged, and
// how long it may sit there first.
//
// Holding Momentum's save-loc load key puts you at the stored position and
// FREEZES you there until you let go. Nothing is moving, so the only velocity
// available to measure is zero -- and judging the file's answer against zero
// throws out every seed of a save-loc made at speed, which is 62% of them. Two
// units is far below anything a moving player covers in a frame and far above
// float noise; the timeout only exists so that some other way of standing
// perfectly still cannot stand a seed up for ever.
#define SEED_MOVED_UNITS 2.0f
#define SEED_HOLD_MAX 15.0f

static float g_seedEnergy = 0.0f, g_seedSpeed = 0.0f;
static Vec3 g_seedAt = { 0.0f, 0.0f, 0.0f };
static float g_seedHeldFor = 0.0f;
static bool g_seedFrozen = false;       // put somewhere and not yet let go of
static bool g_seedPending = false;      // a seed is waiting for its first check
static bool g_seedChecked = false;      // a result exists to report
static bool g_seedRejected = false;
static float g_seedErr = 0.0f, g_seedSpeedErr = 0.0f;
static int g_seedCount = 0, g_seedRejects = 0;

// What the file said, and how long the check waited. Kept only so the log line
// on a rejection can be read afterwards -- see the guard-rail in
// WrEnergySample. Nothing here takes part in the decision.
static Vec3 g_seedVel = { 0.0f, 0.0f, 0.0f };
static float g_seedAge = 0.0f;
static int g_seedFrames = 0;

// After a discontinuity, how long the output filter is given to converge before
// the gain/loss accumulator is allowed to bank anything. Three time constants is
// 95% converged.
#define SETTLE_TAUS 3.0f
static float g_settleFor = 0.0f;

// How long the camera must be bit-identical before it is described as still to
// the matrix scanner. This is only a stale-matrix hint now: it never stops the
// sampler, the HUD, the live recorder, or the run clock.
#define HOLD_SECONDS 0.05f
static float g_stillFor = 0.0f;
static bool g_cameraStill = false;
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
static float g_yawTurn = 0.0f;
static float g_yawSigned = 0.0f;

// Displayed fallback values use hysteresis so their noisy last digit does not
// churn. The exact-player path publishes the raw values directly.
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

    // The glanceable readout is part of the default practice view. It starts on
    // the actionable pair -- actual turn rate and the ideal -- instead of the
    // general energy summary.
    g_energy.showHud = true;
    g_energy.hudOffsetX = 0.0f;
    g_energy.hudOffsetY = 0.0f;
    // The editor moves the centre of the block. Directional edge anchors made
    // the same saved position mean four different things and are no longer a
    // user-facing choice.
    g_energy.hudAlignX = WR_HUD_CENTRE_X;
    g_energy.hudAnchorY = WR_HUD_CENTRE_Y;
    g_energy.hudScale = 1.4f;
    g_energy.hudBacking = true;
    g_energy.showHudClock = false;
    g_energy.showHudSpeed = false;
    g_energy.hudWidth = 0.0f;       // as wide as the reserved rows need

    // The corner block; OFF by default, and reachable with END without opening
    // the panel. It is the detailed readout rather than the glanceable one --
    // the centre box is what you play with.
    g_energy.showOverlay = false;
    g_energy.overlayCorner = 3;
    g_energy.overlayScale = 1.0f;
    g_energy.overlayMargin = 16.0f;
    g_energy.overlayPosX = 1.0f;
    g_energy.overlayPosY = 1.0f;

    g_energy.compareToRun = false;
    g_energy.compareRadius = 384.0f;
    g_energy.eyeHeight = 64.0f;

    // Snappy preset. When the proved player-memory velocity is available the
    // sampler bypasses these filters entirely; these values are the responsive
    // fallback for the machines/builds where only the camera can be read.
    g_energy.smoothSeconds = 0.12f;
    g_energy.trendSeconds = 0.75f;
    g_energy.quantiseStep = 5.0f;

    g_energy.velWindowSeconds = 0.020f;
    g_energy.velTau = 0.030f;
    g_energy.speedTau = 0.050f;
    g_energy.powerSeconds = POWER_WINDOW;
    g_energy.showPhase = true;
    g_energy.showStrafe = false;
    // The window, not the physics curve, because the curve is red more often
    // than it is useful once you are quick. 30 deg/s is the default because that
    // is the slack a good strafe actually wanders by; the curve is one combo
    // away and unchanged.
    g_energy.strafeColour = WR_STRAFE_COLOUR_BAND;
    g_energy.strafeBand = WR_STRAFE_BAND_DEGREES;
    g_energy.strafeTolerance = 30.0f;
    g_energy.showStrafePercent = true;
    g_energy.showStrafeArrow = true;

    // ON, unlike most things here, and for a reason that is not a preference:
    // it was asked for, and unlike the numbers beside it there is nothing to
    // read -- a bar you are not using costs a glance you do not spend. It also
    // sits above the crosshair rather than in the HUD block, so it is visible
    // even for the majority of players who never turn the block on.
    g_energy.showStrafeBar = true;
    g_energy.showReferenceStrafeBar = false;
    g_energy.strafeBarSensitivity = 1.0f;
    g_energy.strafeBarWidth = 0.0f;     // 0 = match the HUD block's width
    g_energy.strafeBarHeight = 7.0f;
    // Above the crosshair. Below it is spoken for: WR_BOARD_FLASH_DROP puts the
    // board grade at 46, and a strafe bar landing on top of a board flash would
    // hide the one readout that only exists for a tenth of a second.
    g_energy.strafeBarRise = 54.0f;
    g_energy.strafeBarOffsetX = 0.0f;

    g_energy.showBoard = false;
    g_energy.boardFlashSeconds = 2.0f;
    g_energy.boardUnit = WR_BOARD_UNIT_SPEED;
    g_energy.boardPercent = true;
    // Two, which is what the solved entry earns. See BoardDecimals.
    g_energy.boardDecimals = 2;
    g_energy.gaugeSeconds = 2.0f;
    g_energy.arrowBand = ARROW_BAND;
    g_energy.anchorToRunStart = true;
    g_energy.hudMode = WR_HUD_TURN;
    g_energy.airAccelerate = WR_AIR_ACCEL_DEFAULT;
    g_energy.maxSpeed = WR_MAXSPEED_DEFAULT;

    g_energy.showBar = false;
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

void WrEnergyCycleHudMode(int step)
{
    // Both directions, because cycling forward past the mode you wanted costs a
    // full lap mid-ramp. The double modulus is the usual guard against C's
    // truncating negative remainder.
    int n = WR_HUD_MODE_COUNT;
    int m = g_energy.hudMode;
    if (m < 0 || m >= n)
        m = 0;
    g_energy.hudMode = ((m + step) % n + n) % n;
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
    WrEmaReset(&g_yawTurnEma);
    WrEmaReset(&g_yawSignedEma);
    WrTrendReset(&g_trend);
    WrTrendReset(&g_gauge);
    WrTrendReset(&g_vzTrend);

    // A map change comes through here, and the geometry answer belongs to the
    // level that was on screen when it was taken.
    g_geomTouch = WR_GEOM_UNKNOWN;
    g_geomHaveNormal = false;
    g_geomHaveRamp = false;
    g_geomRampDist = -1.0f;
    g_geomNearDist = -1.0f;

    // The scanner's answer dies with the level too -- a heap object moves and
    // the address that held the origin holds something else. wr_player.cpp
    // notices on its own, but it must not be believed for the frames in
    // between.
    g_trueLive = false;
    g_trueEye = -1.0f;

    // So does the last board. Showing the previous map's board after a level
    // change is the same class of mistake as drawing its ramps.
    g_bRingHead = 0;
    g_bRingCount = 0;
    g_bPrevPhase = WR_PHASE_UNKNOWN;
    g_bAirFor = 0.0f;
    g_bAirGrace = 0.0f;
    g_bHave = false;
    g_bAge = 0.0f;
    g_bAvailable = false;
    g_bExact = false;

    // And so does the tally of what went wrong: it is a fact about this level's
    // ramps, and carrying it across a map change would make it a fact about
    // nothing.
    g_bWhy = WR_BOARD_WHY_NONE;
    g_bWhyNz = 0.0f;
    for (int i = 0; i < WR_BOARD_WHY__COUNT; i++)
        g_bWhyCount[i] = 0;

    g_gaugeClock = 0.0f;
    WrArrowReset(&g_arrow);
    WrSwingReset(&g_swing, WR_SWING_HYSTERESIS);
    g_settleFor = 0.0f;
    g_spliced = false;
    g_haveShownSpent = false;
    g_haveShownCarried = false;
    g_seedPending = false;
    g_seedFrozen = false;
    g_seedChecked = false;      // a new map's Diagnostics starts with no claim
    g_seedRejected = false;
    g_seedCount = g_seedRejects = 0;

    g_valid = false;
    g_havePos = false;
    g_haveReading = false;
    g_stillFor = 0.0f;
    g_cameraStill = false;
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
    g_yawTurn = 0.0f;
    g_yawSigned = 0.0f;
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
    WrTrendReset(&g_gauge);
    WrTrendReset(&g_vzTrend);
    g_gaugeClock = 0.0f;
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
    g_cameraStill = false;
    g_speedRate = g_viewTurn = g_velTurn = 0.0f;
    g_yawTurn = 0.0f;
    g_yawSigned = 0.0f;

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

    g_seedVel = vel;
    g_seedAge = 0.0f;
    g_seedFrames = 0;

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
    g_seedAt = camPos;
    g_seedHeldFor = 0.0f;
    g_seedFrozen = true;
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

bool WrEnergyCameraStill(void) { return g_cameraStill; }

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

    // A repeated camera used to return here and freeze every published number.
    // That was necessary while velocity was inferred from camera motion: a
    // paused demo otherwise looked like an instantaneous stop. It is actively
    // wrong with the game's own origin/velocity pair -- the exact values remain
    // meaningful, and returning here also prevents a changed view angle from
    // updating the turn-rate readout while the player stands still.
    //
    // Keep only the stillness fact for the stale-matrix safety net. Sampling,
    // clocks, trends and recording all continue normally.
    if (g_haveReading && havePrev &&
        pos.x == prev.x && pos.y == prev.y && pos.z == prev.z)
    {
        g_stillFor += dt;
        if (g_stillFor >= HOLD_SECONDS)
            g_cameraStill = true;
    }
    else
    {
        g_stillFor = 0.0f;
        g_cameraStill = false;
    }

    g_clock += dt;

    // Counted here rather than beside the check, deliberately: the camera
    // fallback can still return while a save-loc is held, while its window is
    // filling, or after an insane sample. How many frames passed is precisely
    // what the rejection log needs in order to explain a measurement of zero.
    if (g_seedPending)
    {
        g_seedAge += dt;
        g_seedFrames++;
    }

    if (havePrev && WrDist(prev, pos) > TELEPORT_UNITS)
        Teleported(pos);

    // --- a save-loc that has been loaded but not let go of ---------------------
    //
    // Momentum's load key holds you at the stored position while it is held
    // down. The player is not moving, so nothing here can measure a velocity --
    // and the honest answer used to be zero, because there was nothing else. Now
    // there is: the velocity in the file, already seeded.
    //
    // So while a seed is waiting to be judged and the camera has not left where
    // it landed, keep showing it and measure nothing. Not doing this had two
    // effects, both bad: the window filled with identical positions and the
    // readout collapsed to height-alone, and then the guard-rail compared the
    // file's velocity against that zero and threw out a seed that was right.
    if (g_seedFrozen && !g_trueLive)
    {
        g_seedHeldFor += dt;
        if (WrDist(pos, g_seedAt) < SEED_MOVED_UNITS &&
            g_seedHeldFor < SEED_HOLD_MAX)
            return;

        // Moving again. Cleared FIRST, and that is not tidiness: the window is
        // reset on this transition and on no other frame. Leaving this armed
        // would reset it every frame for as long as the seed was unjudged, so it
        // could never span the 30 ms it needs to produce an estimate, and the
        // seed would never be judged at all -- each condition keeping the other
        // true.
        g_seedFrozen = false;

        // The window must start from HERE. It still holds the frozen samples,
        // and differencing across them would charge the whole stationary stretch
        // to the first movement, read far too low, and reject the seed for
        // disagreeing with a number that only looks that way because the freeze
        // is still inside the measurement.
        WrVelReset(&g_win);
    }
    else if (g_seedFrozen)
    {
        // The exact player pair can judge the seed immediately even while the
        // save-loc key is holding the origin still. Do not freeze a known value
        // merely because the camera fallback would have had nothing to measure.
        g_seedFrozen = false;
    }

    WrVelPush(&g_win, pos.x, pos.y, pos.z, dt);

    float rx = 0.0f, ry = 0.0f, rz = 0.0f;
    float mx = 0.0f, my = 0.0f, mz = 0.0f;
    float midLead = 0.0f;
    const bool haveCameraEstimate =
        WrVelEstimate(&g_win, g_energy.velWindowSeconds,
                      &rx, &ry, &rz, &mx, &my, &mz, &midLead);
    if (!haveCameraEstimate && !g_trueLive)
        return;

    // Prefer the game's own proved pair. It is already the velocity we were
    // trying to reconstruct, so putting it through a camera-difference window
    // and two EMAs would add lag to an exact value without removing any error.
    //
    // Keep the energy on the historical eye-height basis: every run anchor is
    // a player origin plus this same setting, and WrEnergySampleAt subtracts it
    // back off when publishing feet. Using the measured crouched eye height
    // here would make ducking look like losing 36 units of mechanical energy.
    const bool exactPlayer = g_trueLive;
    Vec3 raw = exactPlayer ? g_trueVel : WrVec(rx, ry, rz);
    Vec3 mid = exactPlayer ? g_trueOrigin : WrVec(mx, my, mz);
    if (exactPlayer)
    {
        mid.z += g_energy.eyeHeight;
        midLead = 0.0f;
    }
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

        // THE REJECTION IS NOT RARE AND THE LOG DID NOT SAY ENOUGH TO EXPLAIN IT.
        //
        // Over two recorded sessions, 1026 of 1052 seeds were thrown out and 880
        // of 914 before that -- 97% and 96% -- every one of them reading "the
        // file said 2187 u/s, the first measurement says 0". A rejection resets
        // the whole filter chain including the trend the phase readout runs on,
        // and holds for three taus, which lands squarely on the first third of a
        // second of every save-loc practice attempt. That is the same third of a
        // second a player is watching for a board.
        //
        // What the old line could not distinguish: a game that genuinely does
        // not restore the velocity, from a measurement taken while the player
        // was still held at the load position, from a window that filled with
        // repeated frames. So the vector and the elapsed time go in the log
        // too. This CHANGES NOTHING about the decision -- the tolerance is
        // untouched -- it only makes the next session's log able to answer the
        // question.
        if (g_seedRejected)
            WrLogf("energy: seed check -- file (%.1f %.1f %.1f) |v| %.0f, "
                   "measured (%.1f %.1f %.1f) |v| %.0f, %.0f ms and %d frames "
                   "after the load, window span %.0f ms",
                   g_seedVel.x, g_seedVel.y, g_seedVel.z, g_seedSpeed,
                   raw.x, raw.y, raw.z, measured,
                   g_seedAge * 1000.0f, g_seedFrames,
                   g_energy.velWindowSeconds * 1000.0f);

        if (g_seedRejected)
        {
            g_seedRejects++;
            WrEmaReset(&g_velX); WrEmaReset(&g_velY); WrEmaReset(&g_velZ);
            WrEmaReset(&g_speedEma); WrEmaReset(&g_energyEma); WrEmaReset(&g_zEma);
            WrTrendReset(&g_trend);
            WrTrendReset(&g_gauge);
    WrTrendReset(&g_vzTrend);
            g_gaugeClock = 0.0f;
            g_swing.have = false;
            // And back to the full hold, because from here this is an ordinary
            // unseeded teleport and the reasoning for three taus applies again.
            g_settleFor = SETTLE_TAUS * g_energy.smoothSeconds;
            WrLogf("energy: seed REJECTED -- the file said %.0f u/s, the first "
                   "measurement says %.0f. Measuring it instead.",
                   g_seedSpeed, measured);
        }
    }

    if (exactPlayer)
    {
        g_vel = raw;
        // Keep the fallback filters warm. If the proved address disappears,
        // the next camera-derived frame continues from the last true value
        // instead of climbing out of zero.
        WrEmaSeed(&g_velX, raw.x);
        WrEmaSeed(&g_velY, raw.y);
        WrEmaSeed(&g_velZ, raw.z);
    }
    else
    {
        g_vel.x = WrEmaStep(&g_velX, raw.x, dt, g_energy.velTau);
        g_vel.y = WrEmaStep(&g_velY, raw.y, dt, g_energy.velTau);
        g_vel.z = WrEmaStep(&g_velZ, raw.z, dt, g_energy.velTau);
    }

    float instSpeed = WrLength(g_vel);
    if (exactPlayer)
    {
        g_speed = instSpeed;
        WrEmaSeed(&g_speedEma, instSpeed);
    }
    else
    {
        g_speed = WrEmaStep(&g_speedEma, instSpeed, dt, g_energy.speedTau);
    }

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
    if (exactPlayer)
    {
        g_nowSmooth = g_now;
        WrEmaSeed(&g_energyEma, g_now);
    }
    else
    {
        g_nowSmooth = WrEmaStep(&g_energyEma, g_now, dt,
                                g_energy.smoothSeconds);
    }
    WrTrendPush(&g_trend, g_nowSmooth, dt);

    // The strafe gauge's own ring, fed at about 20 Hz.
    //
    // It cannot share the one above. That ring is 256 samples of every frame,
    // which is 0.85 s at 300 fps and 1.28 s at 200 -- and the gauge's whole
    // justification is a window of about two seconds, because that is where a
    // camera-differenced velocity starts agreeing with the truth (81.5% at 2 s
    // against 45% at 0.4; see wr_stress.h). Asking WrTrendOverSpan for 2 s from
    // the fast ring returns the change over 1.3 s and calls it 2 s, which reads
    // the RATE low by the ratio it was short by. At 20 Hz the same 256 slots
    // hold 12.8 seconds, so the window always fits at any frame rate.
    g_gaugeClock += dt;
    if (g_gaugeClock >= GAUGE_INTERVAL)
    {
        WrTrendPush(&g_gauge, g_nowSmooth, g_gaugeClock);
        g_gaugeClock = 0.0f;
    }

    // Vertical velocity, per frame, for the phase test.
    //
    // Unlike the gauge this one WANTS the fast ring: the window is 0.10 s, which
    // fits in 256 frames at any frame rate anybody plays at, and a longer window
    // makes it worse rather than better -- measured, agreement peaks around 0.10
    // and falls away by 0.30. Contact is a short-lived thing and smearing it
    // over a third of a second smears it across the transition.
    WrTrendPush(&g_vzTrend, g_vel.z, dt);

    // The RAW window velocity for the board lookback, not the EMA beside it. The
    // EMA's whole job is to be steady, which here means it is still carrying the
    // pre-clip speed for a tau after the clip and the post-clip speed for a tau
    // before this reaches back far enough -- it would flatten exactly the edge
    // the board is measuring.
    //
    // The POSITION goes in beside it, and it is the window's midpoint less the
    // eye height -- the feet, at the instant `raw` refers to. Pairing the raw
    // velocity with the current camera would be pairing two moments about 20 ms
    // apart, which is the defect WrEnergySampleAt exists to have stopped. This
    // is what lets the board solve where the plane was crossed.
    //
    // ...unless the game's own pair has been found, in which case that is used
    // for both. It is exact, it refers to one instant, it carries no view bob
    // and no duck, and it needs no eye height at all. Both or neither, for the
    // pairing reason above.
    g_bRingHead = (g_bRingHead + 1) % BOARD_RING;
    if (g_trueLive)
    {
        g_bRingV[g_bRingHead] = g_trueVel;
        g_bRingP[g_bRingHead] = g_trueOrigin;
    }
    else
    {
        // `mid` is the stored sample NEAREST the window's centre, not the centre
        // itself, so it trails the instant `raw` refers to by up to half a
        // frame -- eight units at 60 fps and 1000 u/s. Everywhere else that is
        // invisible behind the output filter; here it is the position half of
        // the pair the plane crossing is solved from, so it is carried forward
        // to the velocity's own instant. Linear is enough: the quadratic term
        // over half a frame is three hundredths of a unit.
        const float eye = (g_trueEye > 0.0f) ? g_trueEye : g_energy.eyeHeight;
        g_bRingV[g_bRingHead] = raw;
        g_bRingP[g_bRingHead] = WrVec(mid.x + raw.x * midLead,
                                      mid.y + raw.y * midLead,
                                      mid.z + raw.z * midLead - eye);
    }
    g_bRingDt[g_bRingHead] = dt;
    if (g_bRingCount < BOARD_RING)
        g_bRingCount++;

    // The height alone, through the SAME filter and from the SAME instant --
    // mid.z, not pos.z. Because an EMA is linear, subtracting it from the
    // filtered energy gives exactly the filtered kinetic term, so the budget
    // numbers agree with the headline figure rather than nearly agreeing.
    if (exactPlayer)
    {
        g_zSmooth = mid.z;
        WrEmaSeed(&g_zEma, mid.z);
    }
    else
    {
        g_zSmooth = WrEmaStep(&g_zEma, mid.z, dt,
                              g_energy.smoothSeconds);
    }

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
            const float rate = deg / dt;
            if (exactPlayer)
            {
                g_viewTurn = rate;
                WrEmaSeed(&g_viewTurnEma, rate);
            }
            else
            {
                g_viewTurn = WrEmaStep(&g_viewTurnEma, rate, dt, TURN_TAU);
            }

            // And the YAW rate on its own, which is the one that strafing is
            // about. The rate above is the full angle between two forward
            // vectors, so it counts pitch -- and a surfer moves pitch constantly
            // while riding a ramp, which would be charged to their strafing.
            //
            // Unwrapped through the +-180 seam, because the raw difference
            // between +179 and -179 is 358 degrees of a two-degree turn, and at
            // 200 fps that lands in the readout as a 70,000 deg/s spike.
            float yaw = 0.0f, lastYaw = 0.0f;
            float lf[3] = { g_lastFwd.x, g_lastFwd.y, g_lastFwd.z };
            yaw = atan2f(fwd.y, fwd.x) * 57.2957795f;
            lastYaw = atan2f(lf[1], lf[0]) * 57.2957795f;
            float dy = yaw - lastYaw;
            while (dy > 180.0f) dy -= 360.0f;
            while (dy < -180.0f) dy += 360.0f;

            // The sign, kept separately and smoothed separately, because the
            // ramp ideal needs to know WHICH WAY you are strafing: wn is
            // dot(wishdir, normal), and the two directions give ideals that
            // differ by a factor of three on the same surface. Taking the sign
            // off an EMA of the magnitude would be no answer at all.
            //
            // Smoothing the signed rate also does the right thing through a
            // strafe switch: it crosses zero rather than staying high, which is
            // exactly when there is no meaningful strafe direction to report.
            const float signedRate = dy / dt;
            if (exactPlayer)
            {
                g_yawSigned = signedRate;
                WrEmaSeed(&g_yawSignedEma, signedRate);
            }
            else
            {
                g_yawSigned = WrEmaStep(&g_yawSignedEma, signedRate, dt,
                                        TURN_TAU);
            }

            if (dy < 0.0f) dy = -dy;
            const float yawRate = dy / dt;
            if (exactPlayer)
            {
                g_yawTurn = yawRate;
                WrEmaSeed(&g_yawTurnEma, yawRate);
            }
            else
            {
                g_yawTurn = WrEmaStep(&g_yawTurnEma, yawRate, dt, TURN_TAU);
            }
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
            const float rate = deg / dt;
            if (exactPlayer)
            {
                g_velTurn = rate;
                WrEmaSeed(&g_velTurnEma, rate);
            }
            else
            {
                g_velTurn = WrEmaStep(&g_velTurnEma, rate, dt, TURN_TAU);
            }
        }
        g_lastVelDir = dir;
        g_haveVelDir = true;
    }
    if (dt > 1e-5f && g_haveLastSpeed)
    {
        const float rate = (instSpeed - g_lastSpeedForRate) / dt;
        if (exactPlayer)
        {
            g_speedRate = rate;
            WrEmaSeed(&g_accelEma, rate);
        }
        else
        {
            g_speedRate = WrEmaStep(&g_accelEma, rate, dt, ACCEL_TAU);
        }
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
    if (g_trueLive)
    {
        // No temporal filter and no sticky display buckets on an exact value.
        // The latter was not technically smoothing, but its 0.75-step hysteresis
        // made the crosshair figure visibly pause and jump in five-unit steps.
        g_shown = raw;
        g_haveShown = true;
    }
    else
    {
        g_shown = WrQuantise(g_shown, raw, g_energy.quantiseStep,
                             &g_haveShown);
    }
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
    // the camera-fallback trend is taken from its filtered figure. On the exact
    // path there is no estimator noise to hide. WrArrowStep supplies the state
    // hysteresis in either case.
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

// ---------------------------------------------------------------------------
// The strafe gauge
// ---------------------------------------------------------------------------
//
// The same metric that colours the demo lines -- dE/dt against the most air
// strafing could physically add -- measured live, over a window long enough for
// a camera-differenced velocity to be worth reading.
//
// THE THING IT IS NOT
//
// It is not a turn-rate meter, and it does not need the ramp's angle. Turn rate
// was the first attempt and it fires on a tenth to a quarter of the samples of
// record-class runs, because a ramp turns the velocity through the surface
// normal far faster than air acceleration ever can. The ceiling below bounds the
// CONSEQUENCE instead -- how much energy could have been added -- and that bound
// is ws^2 / (2*g*tick), independent of speed and of the geometry you are on. See
// wr_stress.h for the derivation and the measurements.
//
// It is also not per-segment. The window is the whole point: at 0.40 s a
// camera-derived reading agrees with the truth 58% of the time and points the
// wrong way 24% of the time, and airborne -- where it would actually mean
// strafing rather than a ramp collision -- it is 45% and 32%. At 2 s it is 81.5%
// and 8.5%. So this is one rolling number and the line stays uncoloured.
//
// The tick comes in as a parameter rather than being looked up here, and that is
// a link constraint rather than taste: the live caller reads Momentum's current
// interval_per_tick, while this arithmetic is linked into harnesses that have no
// engine or timer reader. Passing it in also makes both tickrates directly
// testable. Stored demo paths use their own recorded tick in their separate
// derived-data path.
float WrEnergyEta(float tickInterval, bool *noReading)
{
    if (noReading)
        *noReading = true;

    float w = g_energy.gaugeSeconds > 0.25f ? g_energy.gaugeSeconds : 2.0f;

    float span = 0.0f;
    float d = WrTrendOverSpan(&g_gauge, w, &span);
    if (span < 0.25f)
        return 0.0f;            // not enough history yet, e.g. just after a load

    float rate = d / span;
    const float wishSpeed = WrAirWishSpeed(g_energy.maxSpeed,
                                           WrEnergyCrouched());
    const float friction = WrEnergyAirFriction();
    float ceiling = WrAirPowerCeilingEx(g_energy.gravity, tickInterval,
                                        g_energy.airAccelerate,
                                        wishSpeed, friction);

    // A booster is not perfect play, and reporting it as 0 would put it in the
    // same bucket as free flight. Asked first, exactly as wr_path.cpp does.
    if (WrEtaIsNoData(rate, ceiling))
        return 0.0f;

    if (noReading)
        *noReading = false;
    return WrEfficiency(rate, ceiling);
}

float WrEnergyGaugeSpan(void)
{
    float w = g_energy.gaugeSeconds > 0.25f ? g_energy.gaugeSeconds : 2.0f;
    float span = 0.0f;
    WrTrendOverSpan(&g_gauge, w, &span);
    return span;
}

float WrEnergyViewTurnRate(void) { return g_viewTurn; }
float WrEnergyYawRate(void) { return g_yawTurn; }
float WrEnergyYawRateSigned(void) { return g_yawSigned; }
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
    if (g_trueLive)
    {
        g_shownSpent = rawSpent;
        g_haveShownSpent = true;
    }
    else
    {
        g_shownSpent = WrQuantise(g_shownSpent, rawSpent,
                                  g_energy.quantiseStep,
                                  &g_haveShownSpent);
    }

    out->spent = g_shownSpent;
    out->wasted = -WrEnergyRelative();
    out->banked = out->spent - out->wasted;

    out->carriedValid = (rawSpent > CARRIED_MIN_SPEND);
    if (out->carriedValid)
    {
        float raw = (out->banked / rawSpent) * 100.0f;
        // A whole percent, with the same hysteresis the other figures get.
        if (g_trueLive)
        {
            g_shownCarried = raw;
            g_haveShownCarried = true;
        }
        else
        {
            g_shownCarried = WrQuantise(g_shownCarried, raw, 1.0f,
                                        &g_haveShownCarried);
        }
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
// Are you in the air, on a ramp, or on the ground -- right now?
//
// The air/contact half is the vertical acceleration against gravity, exactly as
// on a stored line, but with the tolerance opened from 60 to 250 because the
// velocity behind it is differenced from the camera rather than recorded. See
// the measurements at PHASE_LIVE_TOL.
//
// The ramp/ground half composes this with the old |vz| ground test, and the
// composition is worth stating because it repairs something. That test settles
// whenever the vertical speed is small for a moment, which wr_energy.h records
// as firing "at the apex of every arc" -- at g = 800 the vertical speed passes
// through zero slowly enough to hold the window for a quarter of a second, and
// that is why it was taken off the anchor logic. It cannot do that here: an apex
// is free flight, so the contact test rules it out before the ground test is
// ever asked. Two heuristics, each with a known failure, and the failure of one
// is exactly what the other is sure about.
//
// AND THE MAP TAKES THE INVENTED ONES BACK, once it is holding enough of the
// map to be believed. A contact the kinematics found, with no map surface within
// WR_BSP_TOUCH_RADIUS of the feet, is a contact with nothing to touch.
//
// At the shipped window and tolerance, on maps with no displacements:
//
//     kinematics alone               92.2%  (miss 0.3  fake 7.6)
//     ...vetoed, clip brushes SKIPPED 73.8%  (miss 26.0 fake 0.2)
//     ...vetoed, clip brushes kept   98.3%  (miss  1.3 fake 0.4)
//
// WHICH GATE THOSE THREE WERE MEASURED THROUGH, said plainly because it has
// since changed. phase_sweep kept a private copy of the completeness test and it
// was stale: it refused every displacement map rather than every partly-built
// one, and it left out the entity brushes the shipped test counts. That copy is
// gone -- it calls WrBspGeometryComplete now -- but these numbers predate the
// call, so they describe the OLD gate. Re-deriving them needs the corpus they
// came from, which is thousands of runs and not the forty directories a
// development machine happens to hold; until somebody runs phase_sweep over that
// corpus again, read them as the shape of the result and not as this build's.
//
// THE MIDDLE ROW IS THE POINT. The veto was written, measured, found to be much
// worse, and backed out -- and the reason turned out not to be the idea. It was
// that this reader skipped 141,841 PLAYERCLIP brushes, and on a surf map a clip
// brush is very often the thing being ridden, so during genuine contact there
// was no polygon anywhere near the player a quarter of the time. Including them
// (g_wrBspIncludeClip) turns the same veto from a disaster into the largest
// single accuracy gain this readout has had.
//
// Two gates, both load-bearing:
//
//   WrBspLoadGeometryComplete   every displacement BUILT, and enough of the map
//                               owned by worldspawn or by a solid entity. On a
//                               map with unbuilt geometry the same veto reads
//                               83.2% against 93.6% for leaving it alone,
//                               because absence there means "not read". It used
//                               to say "no displacements at all", which refused
//                               597 maps that are in fact complete -- and on one
//                               of them, four unbuilt displacements out of 756,
//                               it cost the whole level its live map query.
//   one-sided                   it may only turn contact into AIR, never the
//                               reverse. If the vertical acceleration is
//                               gravity then nothing is pushing, whatever is
//                               beside your feet.
//
// WR_PHASE_UNKNOWN when there is no camera, or not enough history yet.
int WrEnergyPhase(void)
{
    if (!g_valid)
        return WR_PHASE_UNKNOWN;

    float span = 0.0f;
    float dvz = WrTrendOverSpan(&g_vzTrend, PHASE_LIVE_WINDOW, &span);

    // Refuse a short span rather than dividing by the window that was asked
    // for. The ring is 256 frames, so this only bites in the first tenth of a
    // second after a reset -- but reading a rate off a span half the size of the
    // one it is divided by is the exact defect WrEnergyPower documents.
    if (span < PHASE_LIVE_WINDOW * 0.75f)
        return WR_PHASE_UNKNOWN;

    float az = dvz / span;
    float d = az + g_energy.gravity;
    if (d < 0.0f) d = -d;

    if (d < PHASE_LIVE_TOL)
        return WR_PHASE_AIR;

    // Contact, kinematically. Now the map, if it is in a position to be asked.
    if (g_geomTouch == WR_GEOM_NOTHING)
        return WR_PHASE_AIR;

    return g_onGround ? WR_PHASE_GROUND : WR_PHASE_RAMP;
}

// The surface being ridden, oriented OUT of it and towards the player.
//
// A BSP plane normal points whichever way the brush side was written, and there
// is nothing in the file that says which side of it a player is on. For drawing
// a ramp that does not matter and the reader folds the sign away; for the strafe
// ideal it decides the answer, because wn = dot(wishdir, n) changes sign with it
// and the ideal turn rate on a 53-degree ramp is 55% of the flat one one way
// round and 17% the other.
//
// So it is resolved from the physics rather than from the file: a surface can
// only push, and it pushes along +n. The vertical part of that push is what
// stops you falling at g, so sign(n.z) has to agree with sign(a_z + gravity),
// which the trend this file already keeps for the phase readout measures
// directly. That is also what makes head ramps come out right -- pinned to the
// underside of one, the push is downward and the normal genuinely points down.
//
// Refuses rather than guesses when the lift is too small to have a sign, which
// is the free-flight case, and when the plane is outside the band a ramp lives
// in.
bool WrEnergySurfaceNormal(float out[3])
{
    if (!out || !g_geomHaveNormal || g_geomTouch != WR_GEOM_TOUCHING)
        return false;

    const float nz = (g_geomNormal[2] < 0.0f) ? -g_geomNormal[2]
                                              : g_geomNormal[2];
    if (nz < WR_PHASE_MIN_RAMP_NZ || nz > WR_PHASE_STANDABLE)
        return false;

    float span = 0.0f;
    const float dvz = WrTrendOverSpan(&g_vzTrend, PHASE_LIVE_WINDOW, &span);
    if (span < PHASE_LIVE_WINDOW * 0.75f)
        return false;

    const float lift = dvz / span + g_energy.gravity;

    // Half the phase tolerance. Below this the surface is barely holding you and
    // the sign is noise, which is worse than no answer.
    if (lift > -PHASE_LIVE_TOL * 0.5f && lift < PHASE_LIVE_TOL * 0.5f)
        return false;

    const float s = (lift * g_geomNormal[2] < 0.0f) ? -1.0f : 1.0f;
    out[0] = g_geomNormal[0] * s;
    out[1] = g_geomNormal[1] * s;
    out[2] = g_geomNormal[2] * s;
    return true;
}

int WrEnergyGeomTouch(void)
{
    return g_geomTouch;
}

bool WrEnergyGeomNormalRaw(float out[3])
{
    if (!out || !g_geomHaveNormal)
        return false;
    out[0] = g_geomNormal[0];
    out[1] = g_geomNormal[1];
    out[2] = g_geomNormal[2];
    return true;
}

// Accept a plane only if it is a unit normal, and then MAKE it one.
//
// The band was +-1% on |n|^2 and whatever arrived was used as it stood. A plane
// at |n| = 1.005 passes and inflates every dot product taken against it by half
// a percent, which lands directly on lossPct and so directly on the number the
// player reads. The band is a sanity test, not a licence to skip the division:
// anything inside it is close enough to be real and is then normalised exactly.
static bool AcceptPlane(const float *in, int n, float *out)
{
    if (!in)
        return false;
    double m = (double)in[0] * in[0] + (double)in[1] * in[1] +
               (double)in[2] * in[2];
    if (!(m > 0.98 && m < 1.02))
        return false;
    const double inv = 1.0 / sqrt(m);
    out[0] = (float)(in[0] * inv);
    out[1] = (float)(in[1] * inv);
    out[2] = (float)(in[2] * inv);
    // The plane's own distance scales with the normal it was written beside, so
    // it has to be divided by the same thing or the plane MOVES.
    if (n > 3)
        out[3] = (float)(in[3] * inv);
    return true;
}

void WrEnergySetTruePlayer(const Vec3 *origin, const Vec3 *velocity,
                           float eyeHeight)
{
    g_trueLive = false;
    g_trueEye = (eyeHeight > 0.0f) ? eyeHeight : -1.0f;

    if (!origin || !velocity)
        return;
    if (!WrSaneVec(*origin) || !WrSaneVec(*velocity))
        return;
    if (WrLength(*velocity) > MAX_SANE_SPEED)
        return;

    g_trueOrigin = *origin;
    g_trueVel = *velocity;
    g_trueLive = true;
}

bool WrEnergyTrueVelocityLive(void) { return g_trueLive; }

bool WrEnergyCrouched(void)
{
    if (!(g_trueEye > 0.0f))
        return false;

    // Momentum surf uses a 64-unit standing view and 28-unit ducked view. The
    // eye transition is measured live, so split those states halfway rather
    // than treating a one-unit camera bob as a crouch. In air Momentum completes
    // the hull transition immediately; the ambiguous middle is normally never
    // sampled, and choosing normal there is the safe fallback.
    float standing = g_energy.eyeHeight;
    if (standing < 32.0f)
        standing = 64.0f;
    const float ducked = 28.0f;
    return g_trueEye < (standing + ducked) * 0.5f;
}

float WrEnergyAirFriction(void)
{
    const int phase = WrEnergyPhase();
    if (phase != WR_PHASE_AIR && phase != WR_PHASE_RAMP)
        return 1.0f;

    Vec3 vel;
    if (!WrEnergyVelocity(&vel))
        return 1.0f;
    return WrAirSurfaceFriction(vel.z);
}

void WrEnergySetGeometryTouch(int state, const float *normal,
                              const float *rampPlane, float rampDist,
                              float nearDist)
{
    g_geomTouch = (state == WR_GEOM_NOTHING || state == WR_GEOM_TOUCHING)
                ? state : WR_GEOM_UNKNOWN;

    g_geomHaveNormal = false;
    g_geomHaveRamp = false;
    g_geomRampDist = -1.0f;
    g_geomNearDist = nearDist;
    if (g_geomTouch == WR_GEOM_TOUCHING)
    {
        // Trust nothing that is not a unit vector. A degenerate plane in a map
        // file would otherwise flow straight into a dot product and produce an
        // approach angle out of nothing.
        g_geomHaveNormal = AcceptPlane(normal, 3, g_geomNormal);

        if (AcceptPlane(rampPlane, 4, g_geomRampPlane))
        {
            g_geomHaveRamp = true;
            g_geomRampDist = rampDist;
        }
    }
}

// The raw velocity from `back` seconds ago, walked out of the ring by summing
// the frame times actually recorded. Frame times vary, so counting frames would
// mean a different duration at every frame rate -- the defect wr_smooth.h exists
// to have stopped happening.
// It INTERPOLATES rather than returning the first sample past the mark, and
// that is a correctness fix rather than a polish one. Returning `V[idx]` after
// the accumulator had already passed `back` gave a sample whose real age was
// somewhere in [back - dt, back) -- so the same landing read differently at 60
// fps and at 300, by the amount gravity moves in one frame. `ageOut` reports
// the age actually delivered, which the caller needs to correct for gravity.
static bool WrBoardLookback(float back, Vec3 *out, Vec3 *posOut, float *ageOut)
{
    float acc = 0.0f;
    int idx = g_bRingHead;
    for (int i = 0; i < g_bRingCount; i++)
    {
        const float step = g_bRingDt[idx];
        const float prev = acc;
        acc += step;
        if (acc >= back)
        {
            // `idx` is `acc` old and the one after it is `prev` old, so the
            // wanted age sits between them. A frame with no duration cannot be
            // interpolated across; take its endpoint.
            const int later = (idx + 1) % BOARD_RING;
            float t = (step > 1e-6f) ? (acc - back) / step : 0.0f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            const bool haveLater = (i > 0);
            const Vec3 &va = g_bRingV[idx];
            const Vec3 &vb = haveLater ? g_bRingV[later] : va;
            const Vec3 &pa = g_bRingP[idx];
            const Vec3 &pb = haveLater ? g_bRingP[later] : pa;
            if (out)
                *out = WrVec(va.x + (vb.x - va.x) * t,
                             va.y + (vb.y - va.y) * t,
                             va.z + (vb.z - va.z) * t);
            if (posOut)
                *posOut = WrVec(pa.x + (pb.x - pa.x) * t,
                                pa.y + (pb.y - pa.y) * t,
                                pa.z + (pb.z - pa.z) * t);
            if (ageOut)
                *ageOut = haveLater ? back : prev + step;
            return true;
        }
        idx = (idx - 1 + BOARD_RING) % BOARD_RING;
    }
    return false;       // not enough history yet
}

// THE ARRIVING VELOCITY, SOLVED AGAINST THE SURFACE RATHER THAN GUESSED AT.
//
// Before contact the player is in free flight -- that is what AIR means -- so
// between two ring samples the feet follow a parabola with a known second
// derivative, and the plane is known exactly because it was read out of the
// file. Where the two meet has a closed form, and the velocity there needs no
// window, no lookback constant and no assumption about how late the detector
// fired.
//
// Walk back from now until a sample is found on the free side of the plane,
// then solve inside the one frame that crossed it:
//
//     f(s) = n . (p + v s + 0.5 g s^2) - d,   f(0) < 0 wanted, f(step) >= 0
//
// A quadratic, taken by bisection rather than by the closed form -- the
// discriminant of a nearly-tangent approach is a subtraction of two close
// numbers, and a surf entry is nearly tangent by definition. Twenty halvings of
// a frame is under a microsecond of time resolution and cannot cancel anything.
//
// `nrm` must already point AWAY from the side the player came in on. False when
// no crossing is bracketed inside the history held, which is the ordinary case
// on a ramp already being ridden.
static bool WrBoardSolveEntry(const float plane[4], float gravity, Vec3 *velOut,
                              float *ageOut)
{
    if (g_bRingCount < 2)
        return false;

    const float n[3] = { plane[0], plane[1], plane[2] };
    const float d = plane[3];

    // WHICH SIDE IS "OUTSIDE", AND WHY IT MAY NOT BE ASKED OF THE PRESENT.
    //
    // A brush side's plane faces out of its brush and a displacement triangle's
    // faces whichever way its grid was wound, so the file does not say which
    // side a player rides on and it has to be read off the trajectory.
    //
    // The obvious place to read it is where the player is now -- and that is
    // exactly wrong, because "now" is ON the surface. n.p - d is zero there to
    // within float noise, so the sign came out of rounding and the solve found
    // its crossing on whichever side the noise pointed at. It looked like it
    // worked, at one frame rate out of three.
    //
    // The side is taken from the last sample that is unambiguously clear of the
    // plane instead. That one is airborne by definition, which is the side the
    // player arrived from, which is the side the question is about.
    float sgn = 0.0f;
    {
        float acc0 = 0.0f;
        int scan = g_bRingHead;
        for (int i = 0; i < g_bRingCount && acc0 <= BOARD_SOLVE_REACH; i++)
        {
            const Vec3 &q = g_bRingP[scan];
            const float f = n[0] * q.x + n[1] * q.y + n[2] * q.z - d;
            if (f > BOARD_SOLVE_CLEAR || f < -BOARD_SOLVE_CLEAR)
            {
                sgn = (f > 0.0f) ? 1.0f : -1.0f;
                break;
            }
            acc0 += g_bRingDt[scan];
            scan = (scan - 1 + BOARD_RING) % BOARD_RING;
        }
    }
    if (sgn == 0.0f)
        return false;       // never clear of it; this is a ride, not an arrival

    float acc = 0.0f;
    int idx = g_bRingHead;
    for (int i = 0; i < g_bRingCount - 1; i++)
    {
        const int older = (idx - 1 + BOARD_RING) % BOARD_RING;
        const float step = g_bRingDt[idx];
        acc += step;

        // Beyond this there is no free flight to model -- a board reached out
        // of a ride is a different event and the demo detector's own
        // PHASE_BOARD_AIR_BEFORE bounds it the same way.
        if (acc > BOARD_SOLVE_REACH)
            return false;
        if (!(step > 1e-6f))
        {
            idx = older;
            continue;
        }

        const Vec3 &p0 = g_bRingP[older];
        const Vec3 &v0 = g_bRingV[older];
        const float f0 = sgn * (n[0] * p0.x + n[1] * p0.y + n[2] * p0.z - d);

        // CLEARLY outside, not merely outside, and clear by a distance that
        // depends on how fast this sample is closing on the plane.
        //
        // Two separate things are being avoided. A sample taken while riding
        // the ramp sits within float noise of the plane and falls on whichever
        // side rounding put it -- bracket on that and the solve starts ON the
        // surface, finds its crossing at s = 0 and reports the SLIDING velocity
        // as the arriving one, which reads as no board at all.
        //
        // The second is subtler and cost more. The velocity stored here is a
        // backward difference over velWindowSeconds, and a sample taken half a
        // window before the clip has the clip INSIDE its own window: the
        // position and the velocity are then both averages across an event, and
        // no amount of integrating forward from them recovers what was
        // happening before it. So the bracket has to be a whole window clear,
        // which in distance is how far this sample travels towards the plane in
        // that time. Being further back costs nothing -- free flight is a
        // parabola and the solve integrates along it exactly.
        const float closing = -(v0.x * n[0] + v0.y * n[1] + v0.z * n[2]) * sgn;
        float need = BOARD_SOLVE_CLEAR;
        // A camera-differenced velocity needs to be a whole difference window
        // clear so the clip is not inside its own sample. The game's velocity
        // is instantaneous and has no such window; imposing one on that path
        // can skip the only clean bracket and force the less accurate fixed
        // lookback fallback.
        if (closing > 0.0f && !g_trueLive)
            need += closing * g_energy.velWindowSeconds * 1.5f;
        if (f0 <= need)
        {
            idx = older;
            continue;       // still on or past it, or too close to trust
        }

        // This sample is genuinely in flight, and it is the last one that is --
        // the walk runs newest to oldest. Find where its own arc meets the
        // plane. The span is allowed to run past one frame because the stored
        // position is the velocity window's midpoint and so trails the frame it
        // was recorded on; capping the search at exactly one step would make
        // the answer depend on that lag.
        // The far end of the search is `acc` -- this sample's own age. The
        // crossing cannot be later than now, because the phase readout has
        // already said contact, and it cannot be earlier than this sample,
        // because this sample is clear of the plane. Bounding it by that rather
        // than by a count of doublings is what makes the answer independent of
        // the frame rate: a fixed number of steps reaches half a second at
        // 60 fps and fifty milliseconds at 300, so the same landing solved on
        // one machine and quietly fell back to the estimate on another.
        const float ez = -0.5f * gravity;
        float hi = step;
        float fHi = 0.0f;
        bool bracketed = false;
        for (;;)
        {
            if (hi > acc) hi = acc;
            fHi = sgn * (n[0] * (p0.x + v0.x * hi) +
                         n[1] * (p0.y + v0.y * hi) +
                         n[2] * (p0.z + v0.z * hi + ez * hi * hi) - d);
            if (fHi <= 0.0f) { bracketed = true; break; }
            if (hi >= acc) break;
            hi *= 2.0f;
        }
        if (!bracketed)
        {
            return false;   // it never got there; the contact was something else
        }

        // Bisection rather than the quadratic formula: a surf entry is nearly
        // tangent by definition, so the discriminant is a subtraction of two
        // close numbers exactly where the answer matters most. Twenty halvings
        // resolve the crossing to under a microsecond and cancel nothing.
        float lo = 0.0f;
        for (int k = 0; k < 20; k++)
        {
            const float s = 0.5f * (lo + hi);
            const float f = sgn * (n[0] * (p0.x + v0.x * s) +
                                   n[1] * (p0.y + v0.y * s) +
                                   n[2] * (p0.z + v0.z * s + ez * s * s) - d);
            if (f > 0.0f) lo = s; else hi = s;
        }
        const float s = 0.5f * (lo + hi);

        if (velOut)
            *velOut = WrVec(v0.x, v0.y, v0.z - gravity * s);
        if (ageOut)
            *ageOut = acc - s;
        return true;
    }
    return false;
}

// Record why a landing produced nothing. Only the FIRST refusal of a contact
// event is counted -- the same landing hitting the same wall for twenty frames
// is one story, not twenty -- which is what makes the tally readable as
// "eleven boards went to the wall" rather than as a frame count.
static void BoardRefuse(int why, bool fresh)
{
    g_bWhy = why;
    if (fresh && why > WR_BOARD_WHY_NONE && why < WR_BOARD_WHY__COUNT)
        g_bWhyCount[why]++;
}

void WrEnergyTickBoards(float dt)
{
    if (g_bHave)
        g_bAge += dt;
    if (g_bAirGrace > 0.0f)
        g_bAirGrace -= dt;

    // Only where all three inputs exist. Without a normal from the map there is
    // nothing to project against, and guessing one from the velocity change is
    // the thing the header says this deliberately does not do.
    g_bAvailable = (g_geomTouch != WR_GEOM_UNKNOWN);

    const int ph = WrEnergyPhase();
    if (ph == WR_PHASE_UNKNOWN)
    {
        // Not a refusal anybody can act on -- it is the first fraction of a
        // second after a load or a teleport -- so it is named but not tallied.
        g_bWhy = WR_BOARD_WHY_UNKNOWN_PHASE;
        g_bPrevPhase = ph;
        return;
    }

    if (ph == WR_PHASE_AIR)
    {
        g_bAirFor += dt;
        g_bPrevPhase = ph;
        return;
    }

    // Contact. A board only if there was sustained air before it -- otherwise
    // every flicker of the phase readout on a ramp already being ridden would
    // be graded as an arrival.
    //
    // The air is spent through a grace rather than zeroed outright: see
    // BOARD_AIR_GRACE. `fresh` is what makes the tally count landings instead
    // of frames.
    const float airHad = (g_bAirFor > g_bAirGrace) ? g_bAirFor : g_bAirGrace;
    const bool fresh = (g_bPrevPhase == WR_PHASE_AIR);
    const bool arriving = (airHad >= BOARD_AIR_BEFORE);
    g_bPrevPhase = ph;
    g_bAirFor = 0.0f;

    if (!arriving)
    {
        // Recorded ONLY on the transition frame, and this is the difference
        // between a diagnostic and a nuisance. Every frame of a sustained ride
        // is a contact with no air behind it, so writing the cause here
        // unconditionally overwrote whatever the landing itself had said within
        // one frame -- the row would name the real reason for 3 ms and then
        // spend the next four seconds saying "no air before it", which is true,
        // useless, and hides the answer.
        if (fresh)
            BoardRefuse(WR_BOARD_WHY_NO_AIR, true);
        return;
    }

    // Earned air survives a contact that produced nothing, so one fake frame in
    // flight cannot cost the landing that follows it.
    g_bAirGrace = BOARD_AIR_GRACE;

    if (g_geomTouch == WR_GEOM_UNKNOWN)
    {
        BoardRefuse(WR_BOARD_WHY_MAP_OFF, fresh);
        return;
    }
    if (g_geomTouch == WR_GEOM_NOTHING)
    {
        BoardRefuse(WR_BOARD_WHY_NO_SURFACE, fresh);
        return;
    }
    if (!g_geomHaveNormal)
    {
        BoardRefuse(WR_BOARD_WHY_BAD_PLANE, fresh);
        return;
    }

    // WHICH PLANE TO GRADE AGAINST.
    //
    // The nearest surface of any facing is the right answer to "am I touching
    // something" and the wrong one to "what did I land on". Entering a ramp
    // from the side puts a wall nearer the feet than the ramp, and this used to
    // take the wall, fail the ramp test below and report nothing at all -- with
    // the ramp sitting right there in the same query, unasked for.
    //
    // So the ramp candidate is preferred where the map offered one AND it is
    // close enough to the nearest surface to be the same contact -- see
    // GEOM_RAMP_PREFER, which is what stops this inventing a board on a floor
    // landing that merely has a ramp in range. The nearest-of-any is the
    // fallback.
    //
    // The ramp band is still checked afterwards rather than assumed: preferring
    // a candidate is not the same as trusting it, and a map with no rideable
    // surface anywhere near must still refuse.
    bool useRamp = g_geomHaveRamp;
    if (useRamp && g_geomNearDist >= 0.0f && g_geomRampDist >= 0.0f &&
        g_geomRampDist > g_geomNearDist + GEOM_RAMP_PREFER)
        useRamp = false;
    const float *plane = useRamp ? g_geomRampPlane : g_geomNormal;

    const float nz = plane[2] < 0.0f ? -plane[2] : plane[2];
    g_bWhyNz = nz;
    if (nz < WR_PHASE_MIN_RAMP_NZ || nz > WR_PHASE_STANDABLE)
    {
        BoardRefuse(WR_BOARD_WHY_NOT_RAMP, fresh);
        return;
    }

    // THE ARRIVING VELOCITY. Solved against that plane where the geometry
    // allows it, and reached back for otherwise -- with the gravity that
    // accumulated over the reach put back, which the fixed lookback never did.
    Vec3 vIn;
    float age = 0.0f;
    bool exact = false;
    if (useRamp &&
        WrBoardSolveEntry(g_geomRampPlane, g_energy.gravity, &vIn, &age))
    {
        exact = true;
    }
    else
    {
        Vec3 unusedPos;
        if (!WrBoardLookback(BOARD_LOOKBACK, &vIn, &unusedPos, &age))
        {
            BoardRefuse(WR_BOARD_WHY_NO_HISTORY, fresh);
            return;
        }
        // The sample is `age` seconds before the detector fired, and over that
        // stretch gravity has been acting. Carrying it forward is not a
        // correction to the estimate, it is the estimate finished: without it
        // the board is judged on a velocity the player never had at the
        // surface, and by an amount that varies with the frame rate.
        vIn.z -= g_energy.gravity * age;
    }

    // vOut is unused by the grading now that the loss is projected rather than
    // subtracted -- WrPhaseBoard still wants a sample, so it gets the current
    // one, which is the honest thing it is: where the velocity actually went.
    const Vec3 &out = g_trueLive ? g_trueVel : g_vel;
    const float vi[3] = { vIn.x, vIn.y, vIn.z };
    const float vo[3] = { out.x, out.y, out.z };

    WrBoardStats s;
    if (!WrPhaseBoard(vi, vo, plane, &s))
    {
        BoardRefuse(WR_BOARD_WHY_DEGENERATE, fresh);
        return;
    }
    if (s.speedIn < WR_BOARD_MIN_SPEED)
    {
        BoardRefuse(WR_BOARD_WHY_TOO_SLOW, fresh);
        return;
    }
    if (s.intoPlane < WR_BOARD_MIN_INTO_PLANE)
    {
        BoardRefuse(WR_BOARD_WHY_TOO_GLANCING, fresh);
        return;
    }

    g_bLast = s;
    g_bHave = true;
    g_bAge = 0.0f;
    g_bExact = exact;
    g_bWhy = WR_BOARD_WHY_NONE;
}

bool WrEnergyBoard(WrBoardStats *out, float *ageOut, float maxAge)
{
    if (!g_bHave)
        return false;
    if (maxAge > 0.0f && g_bAge > maxAge)
        return false;
    if (out) *out = g_bLast;
    if (ageOut) *ageOut = g_bAge;
    return true;
}

bool WrEnergyBoardAvailable(void) { return g_bAvailable; }
bool WrEnergyBoardExact(void) { return g_bExact; }
int  WrEnergyBoardWhy(void) { return g_bWhy; }
float WrEnergyBoardWhyNz(void) { return g_bWhyNz; }

int WrEnergyBoardWhyCount(int why)
{
    if (why < 0 || why >= WR_BOARD_WHY__COUNT)
        return 0;
    return g_bWhyCount[why];
}

// Phrases, not sentences: these go in a HUD row beside the word "board", and
// the row has to stay one line at every HUD scale.
static const char *kBoardWhy[] = {
    "none yet",
    "map not read",
    "nothing to touch",
    "plane unusable",
    "still settling",
    "no air before it",
    "not enough history",
    "not a ramp",
    "cannot grade that",
    "too slow",
    "too glancing"
};
WR_TABLE_IS_FULL(kBoardWhy, WR_BOARD_WHY__COUNT);

const char *WrEnergyBoardWhyName(int why)
{
    if (why < 0 || why >= WR_BOARD_WHY__COUNT)
        return "?";
    return kBoardWhy[why];
}

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
