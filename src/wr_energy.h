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
#include "wr_phase.h"   // WR_PHASE_*, for WrEnergyPhase

struct WrEnergySettings
{
    float gravity;          // sv_gravity; 800 is the Source default

    // The readout beside the crosshair. This is the one you read while surfing;
    // the corner panel is the one you read while standing still.
    bool showHud;
    float hudOffsetX;       // from screen centre
    float hudOffsetY;
    // Which part of the block the offset positions.
    //
    // Alignment used to be an unwritten side effect of the offset's SIGN --
    // negative right-aligned it -- which meant there was no way to centre the
    // block on the crosshair at all, and no way to sit it above without it
    // creeping as rows came and went. Both are now said outright.
    int hudAlignX;          // WrHudAlignX
    int hudAnchorY;         // WrHudAnchorY
    float hudScale;
    bool hudBacking;        // dark plate behind the text
    bool showHudClock;      // the run clock, as a fourth row

    // The second line -- in the default "net" mode the energy expressed as a
    // speed, which is the headline in different units and nothing more. Off,
    // because two ways of saying one number is one too many on a readout you
    // glance at mid-ramp. The other three modes put different figures on that
    // line and are not affected.
    bool showHudSpeed;

    // Width of the block in pixels before hudScale, or 0 for "however wide the
    // reserved rows need to be".
    //
    // It exists because the comparison row prints the other player's name, the
    // name has no bound, and the block used to be measured from it -- so the
    // plate, the position when centred or right-aligned, AND the lean bar all
    // changed size depending on who you happened to be nearest. The bar was the
    // worst of it: its fill is a fraction of the width, so the same energy gap
    // drew a physically longer bar for a player with a longer name.
    float hudWidth;

    // A row on the corner block saying whether you are in the air, on a ramp or
    // on the ground. See WrEnergyPhase for how well it works and how that was
    // measured; it is the only live reading derived from the camera that is
    // accurate enough to state plainly.
    bool showPhase;

    // A row comparing how fast you are turning against how fast Source's own
    // AirAccelerate says you should be. In the air and, since the map reader can
    // supply a surface normal, on ramps -- where the ideal is a different number
    // and not a nearby one. See WrPerfectStrafeDegreesOnPlane.
    bool showStrafe;

    // HOW THAT COMPARISON IS COLOURED.
    //
    // The physics grade is exact and it is unforgiving on purpose: quality is
    // zero at twice the ideal rate, and the ideal FALLS as you speed up -- about
    // 115 deg/s at 1000 u/s and 44 at 2600 -- so an ordinary flick at speed is
    // genuinely off the scale and genuinely costing you. Correct, and not much
    // use to look at when it is red for minutes at a time.
    //
    // So the colour can come instead from a window you set. It says nothing new
    // about the physics; it decides how much slack the colour gives before it
    // starts complaining, which is a preference and belongs to whoever is
    // looking at it.
    int strafeColour;       // WR_STRAFE_COLOUR_*
    int strafeBand;         // WR_STRAFE_BAND_*, what `strafeTolerance` measures
    float strafeTolerance;  // deg/s, or % of ideal, per strafeBand

    // The physics grade as a number, beside whatever the colour is doing. With
    // the 30-unit cap binding -- every surf configuration -- this is exactly the
    // fraction of the best possible speed-squared gain, so "82%" is a real
    // quantity and not a rating.
    bool showStrafePercent;

    // `^` turn faster, `v` turn slower, nothing when close enough. The trend
    // arrow on the big line uses the same two glyphs for a different thing, so
    // this one is drawn tight against the strafe number rather than at the end.
    bool showStrafeArrow;

    // THE SAME READING AS A SHAPE, because a number is the wrong instrument.
    //
    // Asked for as "reading the values at a glance can be difficult in difficult
    // surf" -- which is exactly right, and it is not a matter of font size. On a
    // hard ramp your eyes are on the ramp; a three-digit figure 192 pixels to
    // one side has to be found, focused on and read before it means anything,
    // and by then the moment it described is gone. A bar has none of those
    // steps: how far it leans and which way is legible in peripheral vision.
    //
    // Centre is the IDEAL rather than zero, because the actionable question on a
    // ramp is never "how fast am I turning" -- it is "faster or slower than I
    // should be". That also puts the full width on the error instead of half.
    //
    // It takes its scale from strafeBand and strafeTolerance, deliberately: the
    // bar, the colour and the arrow are three renderings of one comparison, and
    // a second tolerance setting is a way for them to disagree.
    bool showStrafeBar;
    float strafeBarWidth;   // pixels before hudScale; 0 tracks the HUD block
    float strafeBarHeight;
    float strafeBarRise;    // above the crosshair -- the flash owns 46 below it

    // GRADE YOUR OWN BOARDS, the same way a demo's are graded.
    //
    // Two readouts from one switch, because they answer the same question at
    // different moments: a flash under the crosshair at the instant you land,
    // which is the feedback, and a row on the corner block holding the last one,
    // which is what you read afterwards.
    //
    // Off by default and it will stay dark on maps whose geometry this reader
    // does not hold completely -- a live board needs the map's own plane normal,
    // and there is no honest way to grade one without it. See WrEnergyBoard.
    bool showBoard;
    float boardFlashSeconds;    // how long the crosshair flash lasts

    // WHAT A BOARD IS QUOTED IN.
    //
    // A clip is exactly dv = -(v.n)n, so once the normal is kept alongside the
    // board every one of these is the same measurement written out differently
    // -- not four estimates. See WrBoardLoss.
    int boardUnit;              // WR_BOARD_UNIT_*
    bool boardPercent;          // append how close to a perfect board it was

    // HOW MANY DECIMALS, because whole units were throwing away real digits.
    //
    // A board at 2500 u/s costs 10 to 15 units, so rounding the cost to whole
    // numbers is 3-5% of quantisation on the figure itself, and the approach
    // angle is worse: the loss moves 3.8 u/s per degree at 85 degrees, so a
    // whole-degree readout hides a couple of units of cost inside one printed
    // value. The percentage and the angle always take one place more than this,
    // because they are the quantities the cost is derived FROM.
    //
    // It is a setting and not a constant because how many of those digits are
    // meaningful depends on where the velocity came from -- see
    // WrEnergyBoardExact. Two is right when the entry was solved against the
    // map; a camera estimate does not earn the second one.
    int boardDecimals;          // 0..3, applied to the cost

    bool showOverlay;       // the corner block; ON by default
    int overlayCorner;      // 0 TL, 1 TR, 2 BL, 3 BR

    // How far a corner block is kept from the edge of the screen. Was a hard
    // 18 and unclamped, so a block could and did hang off the bottom.
    float overlayMargin;

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

    // And the window the STRAFE GAUGE measures over, which is a different and
    // much longer one on purpose. See WrEnergyEta: at 0.40 s a reading taken
    // from a camera-differenced velocity points the wrong way a quarter of the
    // time, and at 2 s it does so 8.5% of the time. Two seconds is the shortest
    // window where the number is worth putting on screen.
    float gaugeSeconds;
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

// Which edge of the crosshair readout the offset places.
//
// WR_HUD_LEFT puts the block's left edge at the offset, RIGHT puts its right
// edge there, and CENTRE straddles it -- so "directly above the crosshair" is
// CENTRE with a negative Y, which was not expressible before.
enum WrHudAlignX
{
    WR_HUD_LEFT = 0,
    WR_HUD_CENTRE_X,
    WR_HUD_RIGHT,
};

// The block grows and shrinks as the comparison row, the bar and the clock come
// and go. CENTRE keeps its middle on the offset, so it creeps in both directions
// as that happens; ABOVE pins its bottom edge and BELOW its top, so the edge
// nearest the crosshair stays where it was put.
enum WrHudAnchorY
{
    WR_HUD_CENTRE_Y = 0,
    WR_HUD_ABOVE,
    WR_HUD_BELOW,
};

enum WrHudMode
{
    WR_HUD_NET = 0,     // relative energy and the same figure as a speed
    WR_HUD_CARRIED,     // the share of the height you spent that is still speed
    WR_HUD_BUDGET,      // spent / banked / wasted
    WR_HUD_GAINED,      // gross gained and gross lost since the anchor
    WR_HUD_STRAFE,      // how close to the physical best your strafing is
    WR_HUD_TURN,        // how fast you are turning, against how fast you should
    WR_HUD_NET_STRAFE,  // the first one, with the one above on the second line
    WR_HUD_MODE_COUNT
};

enum WrStrafeColour
{
    WR_STRAFE_COLOUR_BAND = 0,  // a window you set, in whatever strafeBand says
    WR_STRAFE_COLOUR_PHYSICS,   // WrStrafeQuality, which is what shipped before
    WR_STRAFE_COLOUR_COUNT
};

enum WrStrafeBand
{
    WR_STRAFE_BAND_DEGREES = 0, // |turn - ideal|, in degrees per second
    WR_STRAFE_BAND_PERCENT,     // |1 - turn/ideal|, as a percentage
    WR_STRAFE_BAND_COUNT
};

// What a board's cost is written as. Every one of these comes out of the same
// dv = -(v.n)n; none of them is an approximation of another.
enum WrBoardUnit
{
    WR_BOARD_UNIT_SPEED = 0,    // 3D speed, which is what surf HUDs quote
    WR_BOARD_UNIT_HORIZONTAL,   // xy speed alone
    WR_BOARD_UNIT_ENERGY,       // as a height, comparable with the rest of the HUD
    WR_BOARD_UNIT_AXES,         // the x, y and z of it separately
    WR_BOARD_UNIT_COUNT
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
// Step the centre box's mode. +1 for the next, -1 for the previous; wraps both
// ways. Page Down and Page Up, from the hotkey thread.
void WrEnergyCycleHudMode(int step);

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

// How close your strafing is to the most air acceleration could physically add,
// in [-1, +1]. +1 is the ceiling, 0 is free flight, negative is energy being
// destroyed -- the same scale the demo lines are coloured on, so the two can
// never disagree.
//
// `tickInterval` is the tick the ceiling is computed at; use the compared run's
// when there is one, 0.015 otherwise. It is a parameter because the run store
// lives on the other side of a link boundary from this file -- see the note in
// the definition.
//
// `noReading` comes back true when there is nothing honest to show: too little
// history yet, or a rate too large to have come from a player, which is a map
// booster and not perfect play. Draw that as a gap, never as zero -- zero also
// means free flight, and drawing the two the same is what made the first version
// of the line colours unreadable.
float WrEnergyEta(float tickInterval, bool *noReading);

// How long a span the gauge's answer actually covered, which is less than the
// window for the first couple of seconds after a reset or a load.
float WrEnergyGaugeSpan(void);

float WrEnergyViewTurnRate(void);
float WrEnergyVelTurnRate(void);
float WrEnergySpeedRate(void);      // d|v|/dt, units per second per second
float WrEnergySinceGround(void);    // vs the last flat ground you jumped from
float WrEnergySinceStart(void);
float WrEnergyPeak(void);           // peak relative energy
// In the air, on a ramp, on the ground, or not known -- one of WR_PHASE_*.
//
// The one live reading that survived being derived from a camera. Efficiency
// did not: see wr_stress.h, 45% agreement and 26% pointing the wrong way.
// Contact changes the vertical acceleration by hundreds of units per second
// squared where air strafing changes it by nothing, and simulated against 250
// real runs through the real estimator this agrees 92.9% of the time, misses a
// ramp 0.3% of the time and invents one 6.8%.
//
// Erring towards inventing a surface rather than missing one is deliberate and
// is what the tolerance is chosen for.
//
// AND THE MAP NOW TAKES THE INVENTED ONES BACK: 98.3% on a map this reader holds
// all of, with the false surfaces down from 7.6% to 0.4%. See
// WrEnergySetGeometryTouch and the table on WrEnergyPhase.
// How fast your view is turning in YAW alone, degrees a second.
//
// Separate from the view turn rate beside it, which is the full angle between
// two forward vectors and therefore counts pitch -- and a surfer moves pitch
// constantly while riding a ramp, which would be charged to their strafing.
//
// Exact, in the same sense the view rate is: it comes from the camera basis the
// matrix oracle validated, not from anything differenced.
float WrEnergyYawRate(void);

// The same rate with its sign: positive turning one way, negative the other.
//
// Only the ramp ideal needs this, and it needs it badly -- dot(wishdir, normal)
// changes sign with the strafe direction, and the ideal turn rate on a 53-degree
// ramp is 55% of the flat one strafing one way and 17% strafing the other.
float WrEnergyYawRateSigned(void);

// The ramp under you, oriented out of the surface towards you, or false if the
// map has not answered or the reading has no sign worth trusting. See the
// definition -- the orientation comes from the physics, not from the file.
bool WrEnergySurfaceNormal(float out[3]);

int WrEnergyPhase(void);

// What the map said about the player's surroundings, pushed in once a frame
// before anything reads WrEnergyPhase.
//
// WHY IT IS PUSHED IN AND NOT LOOKED UP. This file cannot ask wr_bspload
// anything, for the same reason WrEnergyEta takes its tick interval as an
// argument: six of the nine harnesses do not link the geometry layer, and one
// of them is the harness that measures this. Pushing the answer in keeps
// wr_energy.cpp linkable with nothing attached, and leaves every harness with
// the kinematics-only behaviour unless a test deliberately says otherwise.
enum WrGeomTouch
{
    WR_GEOM_UNKNOWN = 0,    // no map, or one whose absences prove nothing
    WR_GEOM_NOTHING,        // asked, and there is no surface within reach
    WR_GEOM_TOUCHING,       // asked, and there is
};
// `normal`, when given, is the unit plane normal of the surface that was found.
// It is ignored unless `state` is WR_GEOM_TOUCHING, and it is what makes a live
// THE GAME'S OWN POSITION AND VELOCITY, when they have been found and proved.
//
// Pushed in rather than pulled out, for the same reason the geometry answer is:
// most of the test harnesses that link this file do not link a memory scanner,
// and they must go on getting exactly the camera-derived behaviour that every
// measurement in this header was taken against.
//
// Both or neither. A true origin paired with a camera-differenced velocity
// would be two instants about 20 ms apart, which is precisely the defect
// WrEnergySampleAt exists to have stopped -- so when only one of them is known,
// this is called with neither and the estimate is used for both.
//
// `eyeHeight` is cam.z - origin.z, measured rather than assumed, or negative
// when unknown. It is what makes a crouched player's feet land in the right
// place: the SETTING is 64 whether or not you are ducking, and Source ducks to
// 28, which is more than the whole 24-unit search radius of the map query.
void WrEnergySetTruePlayer(const Vec3 *origin, const Vec3 *velocity,
                           float eyeHeight);

// Is the live board being graded off the game's own velocity, or off the
// camera estimate? The two do not deserve the same number of decimal places.
bool WrEnergyTrueVelocityLive(void);

// board gradeable -- see WrEnergyBoard.
//
// `rampPlane`, when given, is four floats -- the nearest RIDEABLE surface's
// normal and its own plane distance, from WrBspLoadNearestEx. It is a separate
// argument rather than a better `normal` because the two answer different
// questions and the touch verdict may only be measured against the first one.
void WrEnergySetGeometryTouch(int state, const float *normal = 0,
                              const float *rampPlane = 0, float rampDist = -1.0f,
                              float nearDist = -1.0f);

// The last two answers, unprocessed, so a REFUSAL CAN NAME ITS CAUSE.
//
// WrEnergySurfaceNormal folds four different failures into one false: the map
// was never asked, it found nothing, it found something that is not a ramp, and
// the lift has no sign yet. The strafe row printed "no surface" for all of them,
// which on surf_kvas meant a player was told there was no surface while standing
// on one -- the map had simply never been asked, because four displacements out
// of 756 were not built. These two let the caller tell those apart and say which.
int  WrEnergyGeomTouch(void);
bool WrEnergyGeomNormalRaw(float out[3]);

// YOUR OWN LAST BOARD, GRADED THE SAME WAY A DEMO'S IS.
//
// Demo boards are found by looking for the air-to-contact transition in a whole
// recorded path and recovering the surface normal from the velocity change
// across it (see wr_path.cpp pass three). Live, neither half of that is
// available in the same form: there is no future to look at, and a normal
// recovered from a camera-differenced velocity at one instant is far worse than
// the same recovery on demo data.
//
// So the live board is not a port of the demo one. It is the same GRADE applied
// to better inputs:
//
//     the transition   the live phase readout, which the .bsp veto took to
//                      98.3% -- accurate enough to trust an edge of
//     the normal       read straight out of the map, not recovered from
//                      anything. Exact, where the demo path's is p50 1.19 deg
//     the velocity     the raw window velocity from BOARD_LOOKBACK seconds
//                      before the transition, because the live phase test reads
//                      a 0.10 s window and the velocity at the instant it fires
//                      is already partly post-clip
//
// This is only offered where all three hold, which means brush-complete maps
// only -- the same gate as the veto. WrPhaseBoard does the grading, so a live
// board and a demo board cannot be graded by different rules.
//
// False when there has been no board, or the last one is older than the caller
// cares about. `ageOut` is seconds since it happened.
//
// `maxAge` in seconds; anything <= 0 means "however old it is". This parameter
// is what the paragraph above always claimed and the code never did: the
// implementation only tested "has there ever been one", so the corner row went
// on showing a board from two ramps back for the rest of the level, and a stale
// number is indistinguishable from a fresh one at a glance.
bool WrEnergyBoard(WrBoardStats *out, float *ageOut, float maxAge = 0.0f);

// WHY THE LAST LANDING DID NOT PRODUCE ONE.
//
// A board passes thirteen tests and every one of them used to fail into the
// same silence. These name the one that fired, so a refusal can be read instead
// of guessed at, and count them, so "sometimes" becomes a number.
//
// The order is the order the tests run in, which is also roughly the order of
// how much they mean: the first three are about the map, the middle four about
// the moment, and the last three about the board itself being too small to say
// anything about.
enum
{
    WR_BOARD_WHY_NONE = 0,      // nothing has been refused; no landing yet
    WR_BOARD_WHY_MAP_OFF,       // the map layer has not answered at all
    WR_BOARD_WHY_NO_SURFACE,    // nothing within WR_BSP_TOUCH_RADIUS of the feet
    WR_BOARD_WHY_BAD_PLANE,     // what it found was not a unit normal
    WR_BOARD_WHY_UNKNOWN_PHASE, // not enough history to say air or contact
    WR_BOARD_WHY_NO_AIR,        // contact, but no sustained air before it
    WR_BOARD_WHY_NO_HISTORY,    // the ring could not reach back far enough
    WR_BOARD_WHY_NOT_RAMP,      // the surface is a wall or a floor, not a ramp
    WR_BOARD_WHY_DEGENERATE,    // WrPhaseBoard refused the arithmetic
    WR_BOARD_WHY_TOO_SLOW,      // under WR_BOARD_MIN_SPEED
    WR_BOARD_WHY_TOO_GLANCING,  // under WR_BOARD_MIN_INTO_PLANE
    WR_BOARD_WHY__COUNT
};

// A short phrase for each, for a HUD row. Never null.
const char *WrEnergyBoardWhyName(int why);

// The last refusal, and -- for WR_BOARD_WHY_NOT_RAMP, the one a player can act
// on -- the |n.z| that was actually seen.
int   WrEnergyBoardWhy(void);
float WrEnergyBoardWhyNz(void);

// How many times each cause has fired since the map loaded, so a report can say
// "eleven of them were the wall" instead of "sometimes".
int   WrEnergyBoardWhyCount(int why);

// Was the last board's arriving velocity SOLVED against the map's own plane, or
// estimated by reaching back a fixed distance? The two do not deserve the same
// number of decimal places and the panel says which it is showing.
bool WrEnergyBoardExact(void);

// Once a frame, AFTER WrEnergySetGeometryTouch -- it reads the phase, and the
// phase is not settled for the frame until the geometry answer is in.
void WrEnergyTickBoards(float dt);

// Live boards need the transition AND a normal, so they exist only on maps the
// geometry layer holds completely. Anything drawing a live board readout should
// say so rather than showing an empty box on maps that cannot have one.
bool WrEnergyBoardAvailable(void);

bool WrEnergyOnGround(void);
bool WrEnergyHaveRef(void);
float WrEnergyRefZ(void);
bool WrEnergyHaveGround(void);
float WrEnergyGroundZ(void);

// Recent history of the relative figure, oldest first.
int WrEnergyHistory(const float **out);

#endif // WR_ENERGY_H
