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

    // Anchor the readout to the start of the run being compared against, so its
    // clock and yours start in the same place. Off means only manual anchors.
    bool anchorToRunStart;
};

// Where the anchor came from, for the UI to say plainly.
enum WrAnchorSource
{
    WR_ANCHOR_NONE = 0,
    WR_ANCHOR_RUN_START,
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

WrAnchorSource WrEnergyAnchorSource(void);
bool WrEnergyAnchorPos(Vec3 *out);   // false when there is no anchor

// Convert a position and velocity into an absolute energy height.
float WrEnergyOf(const Vec3 &pos, const Vec3 &vel);

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
