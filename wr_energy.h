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
// the reference is the height of the last ground you were settled on -- on a
// surf map that is the start pad, because you never touch ground again -- and
// what gets displayed is
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
// The eye offset cancels in Erel: z and z_ref are both camera heights. It does
// NOT cancel in the you-versus-run gap, where their z is the player origin --
// hence eyeHeight below.
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
    float eyeHeight;        // camera above feet; only affects the run comparison
};

extern WrEnergySettings g_energy;

void WrEnergyDefaults(void);

// Feed the camera position once per frame. dt is the frame time in seconds.
void WrEnergySample(const Vec3 &pos, float dt);

// Forget the run so far. Called on map change and from the Reset button.
void WrEnergyReset(void);

// Re-arm the reference height at the current position, for when you want to
// measure from here rather than from the last ground.
void WrEnergyRearm(void);

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
