// wr_energy.h  --  "how high could I launch from here", live and per run.
//
// Surfers judge a run by energy rather than speed, because horizontal speed on
// its own is a lie: being higher and slower can be worth more than being lower
// and faster, and a ramp that "feels fast" may have cost you height you never
// get back. The metric everyone uses is the height you would reach if you
// redirected all your momentum straight up:
//
//     E = z + |v|^2 / (2 * g)
//
// Both terms are in world units, so E is a height. E is also *absolute*, which
// is exactly what makes comparing yourself against a loaded run trivial -- two
// players with the same E have the same total mechanical energy, so the
// comparison needs no alignment, no reference point, nothing. Look up the run's
// E near where you are standing and subtract.
//
// WHAT YOU ACTUALLY WATCH IS THE RELATIVE FIGURE
//
// Absolute E is the wrong thing to put on screen. Standing still on a start pad
// 1888 units above the map origin it reads 1888, which is true and useless. So
// it is displayed against an ANCHOR -- a height chosen once and then left alone:
//
//     Erel = z - z_ref + |v|^2 / (2 * g)      how high above that ground I could still get
//     v_eq = sqrt(2 * g * Erel)               the same energy, as a speed
//
// Standing on the pad both are 0. Off the jump, 45 and 268. Five hundred units
// up carrying 2000 u/s, 3000 and 2191. v_eq is sqrt(v^2 + 2*g*h), the speed you
// would have if you traded all that height back, so it reads in the same units
// as the speedometer and can never be below your real speed while you are above
// the reference. Below the reference Erel goes negative and v_eq carries the
// sign rather than clamping, because falling into a pit is a real loss.
//
// THE ANCHOR MUST NOT MOVE, AND THAT WAS THE BUG
//
// The reference used to be "the last ground you were settled on", re-armed every
// frame that a ground heuristic said you were on it. That heuristic was
// |vertical speed| < 30 held for three frames inside a six-unit band -- which is
// satisfied at the APEX OF EVERY ARC. At g=800 the vertical speed passes through
// zero slowly enough that the window holds for around a quarter of a second, so
// the zero point silently re-based itself at the top of every jump and every
// ramp, and stepped again by the whole height difference on landing.
//
// The number was not noisy. Its origin was moving. So the anchor is now set
// once -- from the start of the run you are comparing against, or by hand -- and
// is never re-armed by anything the player does. Nothing about the world can
// move it; only a map change clears it.
//
// In free flight E is CONSERVED, so against a fixed anchor the number should sit
// still through a whole jump and drift down only as real energy is lost. That is
// what makes it readable, and it is what tests\test_energy.cpp asserts.
//
// It also means the number does NOT rise as you fall off the start pad, and that
// is the metric working rather than failing. Falling trades height for speed at
// exactly the rate E is defined to hold constant. E moves when energy is really
// gained or lost -- air strafing adds it, a ramp or a wall takes it -- so in
// practice it moves when your speed changes by more or less than the height
// change accounts for. The figure that rises when you are doing well is the GAP
// against the run you are chasing, not E itself.
//
// A TELEPORT IS A DISCONTINUITY, AND IT USED TO BE A TRAP
//
// The velocity here is differenced from camera positions, so a teleport has to
// be detected and the window dropped. Detecting it is easy; recovering from it
// was not. The last position was recorded at the BOTTOM of the sampler, past an
// early return that fires while the window refills -- so after a teleport the
// next frame still compared against the pre-teleport position, detected the same
// teleport again, and emptied the window again. Every frame. Permanently.
//
// The readout froze at whatever it said the instant you hit a fail trigger, and
// because you fail at the bottom of a map while the anchor is the start pad, it
// froze at a large negative number. Nothing recovered it but the Reset button.
// The last position is now recorded before every early return, and a teleport
// that lands back at the anchor is treated as a fresh attempt.
//
// The eye offset cancels in Erel when the anchor is a camera height. It does NOT
// cancel against a run's first point, which is a player origin at the feet, nor
// in the you-versus-run gap -- hence eyeHeight below, added in both places.
//
// WHAT IS APPROXIMATE HERE, AND WHY
//
// For loaded runs, nothing: .wrpath stores a real velocity per point, so their
// energy is exact.
//
// For you, live, the velocity is differenced from the camera position, because
// that is the only thing WrLines knows about you -- the world-to-screen matrix
// gives a camera, not a player. Differencing one frame at 200 fps turns a
// two-unit view bob into a 400 u/s spike, so it is measured over a few frames
// and smoothed. Expect the live number to be right to within a few percent and
// to lag a fast change slightly.

#ifndef WR_ENERGY_H
#define WR_ENERGY_H

#include "wr_common.h"

struct WrEnergySettings
{
    float gravity;          // sv_gravity; 800 is the Source default

    // The readout beside the crosshair. This is the one you read while surfing;
    // the corner panel is the one you read while standing still.
    bool showHud;
    float hudOffsetX;       // from screen centre; negative puts it left of the
                            // crosshair and right-aligns the block
    float hudOffsetY;
    float hudScale;
    bool hudBacking;        // dark plate behind the text

    bool showOverlay;       // the older corner panel, off by default now
    int overlayCorner;      // 0 TL, 1 TR, 2 BL, 3 BR

    bool compareToRun;      // show the reference run's energy at your position
    float compareRadius;    // don't compare against a run this far from you
    float eyeHeight;        // camera above feet

    float overlayScale;     // the corner block had no scale control at all

    // How hard the readout is filtered, in seconds. Exposed because "readable"
    // is a matter of taste and the right answer differs between watching a ramp
    // and standing still reading numbers.
    float smoothSeconds;    // the headline figure
    float trendSeconds;     // window the arrow judges over
    float quantiseStep;     // round the displayed figure to this

    // The rest of the filter chain, which used to be four #defines.
    //
    // Reported as "the numbers feel slow", and the smoothing slider above was
    // not the whole story: the reading is filtered four times over before it is
    // shown, and only one of those stages was reachable. The full chain is
    //
    //     velWindowSeconds   position differenced over a window -> velocity
    //     velTau             that velocity, smoothed  (vector and turn rates)
    //     speedTau           the speedometer
    //     smoothSeconds      the energy figure itself
    //     quantiseStep       and then rounded, with 0.75-step hysteresis
    //
    // and the end-to-end lag is about velWindowSeconds/2 + smoothSeconds. The
    // defaults are the values those #defines held, so nothing moves until a
    // slider does.
    //
    // Shortening velWindowSeconds is not free: it is a finite difference, so
    // halving the window doubles the noise in the result. 40 ms was chosen
    // because a two-unit view bob reads as 50 u/s over it rather than the
    // 400 u/s a single frame at 200 fps would give.
    float velWindowSeconds;
    float velTau;
    float speedTau;
    float powerSeconds;     // window dE/dt is measured over
    float arrowBand;        // trend inside +-this shows no arrow at all

    // Anchor the readout to the start of the run being compared against, so its
    // clock and yours start in the same place. Off means only manual anchors.
    bool anchorToRunStart;

    // Which pair of numbers the crosshair readout is showing. The block stays
    // three lines in every mode -- the modes change what the lines mean rather
    // than adding any, because a compact readout is the whole point of it.
    int hudMode;            // WrHudMode

    // The two cvars the air-strafing ceiling depends on. Settings, not reads:
    // nothing here touches a cvar. See wr_stress.h for what they change.
    float airAccelerate;    // sv_airaccelerate, 150 on surf servers
    float maxSpeed;         // sv_maxspeed

    // A bar that leans toward whoever is ahead. Reads at a glance where a signed
    // number does not, which is the point of it beside a crosshair.
    bool showBar;
    int barMode;            // WrBarMode
    float barMaxEnergy;     // gap that fills the bar, in energy units
    float barMaxSpeed;      // ... or in units per second
    float barHeight;        // pixels, before hudScale

    // Where the comparison is reading from. Every gap on screen is measured at
    // ONE point of the reference line -- the point nearest you, re-picked each
    // frame -- and until this was drawn there was no way to see which point, or
    // even which line, had won.
    bool showComparePoint;
    bool comparePointLeader;    // and a thread from you to it
};

enum WrBarMode
{
    WR_BAR_ENERGY = 0,      // the default; measured the better discriminator
    WR_BAR_SPEED,
    WR_BAR_MODE_COUNT
};

enum WrHudMode
{
    WR_HUD_NET = 0,     // relative energy and the same figure as a speed
    WR_HUD_CARRIED,     // the share of the height you spent that is still speed
    WR_HUD_BUDGET,      // spent / banked / wasted
    WR_HUD_GAINED,      // gross gained and gross lost since the anchor
    WR_HUD_MODE_COUNT
};

// Where the energy went, as three numbers that add up.
//
// The net figure falls for everyone on a descending map, which reads as failure
// when it means "the map goes down". These say the same thing with the big
// numbers rising:
//
//     spent   the height you have cashed in since the anchor
//     banked  what you still have, written as a height  (= |v|^2/2g)
//     wasted  the difference -- and exactly the negated net figure
//     carried banked/spent, a percentage: how much of the drop you kept
//
// It is an identity, not a new measurement: E_rel = (z - z_a) + (K - K_a), so
// K - K_a = H + E_rel with H = z_a - z. Nothing here can drift or accumulate.
//
// `carried` above 100% is not an error. It means air strafing put in more than
// the map gave you, which happens on maps that climb -- the fastest surf_utopia
// run finishes at 293%. H is deliberately NOT clamped monotone: measured median
// backtrack from the running maximum is 1,465 units on surf_demise and 31,160
// on surf_vacant, and clamping would break the identity to hide it.
struct WrEnergyBudget
{
    float spent;
    float banked;
    float wasted;
    float carried;          // banked / spent
    bool carriedValid;      // false until enough height has been spent
};

// Where the anchor came from, for the UI to say plainly.
enum WrAnchorSource
{
    WR_ANCHOR_NONE = 0,
    WR_ANCHOR_RUN_START,
    WR_ANCHOR_START_ZONE,   // fitted from every run on the leg, not just one
    WR_ANCHOR_MANUAL
};

extern WrEnergySettings g_energy;

void WrEnergyDefaults(void);

// Feed the camera position once per frame. dt is the frame time in seconds.
void WrEnergySample(const Vec3 &pos, float dt);

// Advance the arrow's hysteresis. Separate from sampling because whether an
// arrow is shown is a display decision, not a measurement.
void WrEnergyTickArrow(float dt);

// Forget the run so far. Called on map change and from the Reset button.
void WrEnergyReset(void);

// Anchor here, by hand. The camera z is used directly, since the reference and
// the live figure are then both eye heights and the offset cancels.
void WrEnergyRearm(void);

// Anchor to a world position that is a PLAYER ORIGIN (a run's first point), not
// a camera. eyeHeight is added so it is comparable with our own eye height.
void WrEnergyAnchorToFeet(const Vec3 &feet);

// The same, from the start zone fitted to every run on the leg. See wr_start.h
// for why that is a better point than any single run's first sample.
void WrEnergyAnchorToStartZone(const Vec3 &centre);

WrAnchorSource WrEnergyAnchorSource(void);
bool WrEnergyAnchorPos(Vec3 *out);   // false when there is no anchor

// True once after a teleport that landed back at the anchor -- a fail trigger,
// or the restart key. Reading it clears it, so exactly one caller may consume
// it; that caller is WrTimerTick, which zeroes the clock for the new attempt.
//
// This is the only restart signal the tool has. WrLines reads the camera and
// nothing else, so it cannot see a trigger fire, a key press, or a zone enter.
bool WrEnergyTakeRestart(void);

// True while the camera is not being written at all -- a paused demo, or the
// game not simulating. Everything is frozen at its last real value rather than
// draining toward zero, and the run clock stops with it.
//
// The test is bit-identical position, which is stronger than it looks: a camera
// being updated every tick never repeats a float exactly, and one that is not
// repeats it forever. A player standing still reads as held too, and that costs
// nothing -- if the camera is not moving there is nothing to measure.
bool WrEnergyHeld(void);

// True once after ANY teleport, with where it landed. Same consume-once contract
// as WrEnergyTakeRestart, and the same reason for living here: WrLines has one
// camera history, so it has one teleport detector. The run clock used to keep a
// second one, and the two disagreed on exactly the frame that mattered -- a load
// slow enough to produce a frame over half a second made the timer throw its
// last position away while the sampler kept its, so only one of them saw the
// teleport, and a slow load is what loading actually looks like.
bool WrEnergyTakeTeleport(Vec3 *landedAt);

// --- landing on a save-loc with the answer already in hand ---------------------

// Start the readout from a velocity that is KNOWN rather than one that has to be
// re-derived by differencing the camera over the next third of a second.
//
// Every filter here is reset on a teleport, because a camera-differenced velocity
// cannot cross a discontinuity. A save-loc load is the one discontinuity where
// the far side is on record: Momentum's savedlocs.txt writes down the velocity it
// is about to restore, for every save-loc, and threw it away only because nothing
// here used to read it.
//
// `camPos` is the camera on the landing frame, NOT the origin stored in the file
// -- the file records feet, and a crouched save-loc's eye height is not
// recoverable from it. `why` goes in the log.
//
// Takes no flag and clears none, so it is not a second consumer of the teleport
// signal. Called by WrTimerTick, which has already consumed that signal and
// matched the landing spot to a save-loc; seeding on any other teleport would be
// inventing a velocity.
void WrEnergySeed(const Vec3 &camPos, const Vec3 &vel, const char *why);

// What the seeds turned out to be worth, measured rather than asserted.
//
// A seed is checked against the first independently measured velocity after it,
// about 35 ms later -- near enough the same instant to be a fair comparison,
// where half a second later would be comparing it against half a second of the
// player playing. A seed that disagrees is thrown out and the filters fall back
// to measuring, so `rejects` is the count of times the file was not to be
// believed. False until at least one seed has been checked.
struct WrEnergySeedInfo
{
    float seedEnergy, seedSpeed;    // what the file said
    float energyErr, speedErr;      // measured minus seeded, signed
    bool rejected;                  // the last one was thrown out
    int seeds, rejects;             // since the last reset
};
bool WrEnergySeedReport(WrEnergySeedInfo *out);

// --- the budget ---------------------------------------------------------------

// False when there is nothing to measure from. The three numbers are quantised
// so they agree with each other and with the net readout exactly on screen.
bool WrEnergyBudgetNow(WrEnergyBudget *out);

// Gross energy added and gross energy thrown away since the anchor, banked only
// once a swing has reversed by WR_SWING_HYSTERESIS. See wr_budget.h for why the
// obvious per-sample version reads in the thousands on a trajectory where
// nothing at all is happening.
float WrEnergyGained(void);
float WrEnergyLost(void);

// True when a teleport has crossed the accumulators since the last restart, so
// the totals cover more than one continuous attempt. Shown rather than hidden.
bool WrEnergyBudgetSpliced(void);

// Cycle the crosshair readout. Bound to a key so it can be changed mid-run.
void WrEnergyCycleHudMode(void);

// Convert a position and velocity into an absolute energy height.
float WrEnergyOf(const Vec3 &pos, const Vec3 &vel);

// The position and velocity of THE SAME INSTANT, for anything that computes an
// energy from a pair rather than reading the finished number.
//
// This is not a convenience accessor. The window velocity refers to the middle
// of its window and the smoothed velocity trails that by another EMA, so the
// obvious pairing -- where you are now, and the velocity readout -- describes
// two moments about 80 ms apart. Energy is quadratic in speed, so on a ramp
// that mismatch is worth hundreds of units and it does not average out: the
// same 20 ms mistake made a ballistic arc appear to lose 46 units over 1.6 s of
// free fall, which is what put the mid-window position in the sampler in the
// first place. Feet, not eye, because that is what a run stores.
//
// False until a sample has succeeded.
bool WrEnergySampleAt(Vec3 *feet, Vec3 *vel);

// --- live state -------------------------------------------------------------

bool WrEnergyValid(void);
float WrEnergyNow(void);            // absolute, i.e. z + v^2/2g
float WrEnergyRelative(void);       // vs the reference height -- the headline
float WrEnergyEquivSpeed(void);     // the same energy expressed as a speed
float WrEnergyTrend(void);          // signed change over the last ~0.4 s
// -1 falling, 0 steady, +1 rising. Not just the sign of the trend: the live
// velocity carries a few percent of error, and a few percent of the kinetic
// term at surf speeds is worth a hundred units of energy, so the dead band has
// to widen with speed or the arrow strobes.
int WrEnergyTrendDir(void);
float WrEnergySpeed(void);          // smoothed |v|, units/second
float WrEnergyHorizontalSpeed(void);
bool WrEnergyVelocity(Vec3 *out);   // smoothed velocity, for the world vector

// How fast you are turning, in degrees per second.
//
// Two different measurements, and the difference matters. The VIEW rate is
// exact: it is the angle between successive camera forward vectors, i.e.
// literally how fast the mouse is moving. The VELOCITY rate is where your
// momentum is turning, which is what a demo line can also be measured for --
// see wr_stress.h.
// dE/dt in energy units per second, over a rolling window. Divided by
// WrAirPowerCeiling this is the efficiency figure -- how much of the energy air
// strafing could physically have added was actually added. Read it as a rolling
// average, never as a per-frame verdict: the residual noise is comparable to the
// largest genuine excursion over the window.
float WrEnergyPower(void);

float WrEnergyViewTurnRate(void);
float WrEnergyVelTurnRate(void);
float WrEnergySpeedRate(void);      // d|v|/dt, units per second per second
float WrEnergySinceGround(void);    // vs the last flat ground you jumped from
float WrEnergySinceStart(void);
float WrEnergyPeak(void);           // peak relative energy
bool WrEnergyOnGround(void);
bool WrEnergyHaveRef(void);
float WrEnergyRefZ(void);
bool WrEnergyHaveGround(void);
float WrEnergyGroundZ(void);

// Recent history of the relative figure, oldest first.
int WrEnergyHistory(const float **out);

#endif // WR_ENERGY_H
