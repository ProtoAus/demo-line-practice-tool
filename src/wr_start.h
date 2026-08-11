// wr_start.h  --  where a map's runs begin, and noticing when you leave it.
//
// WHAT THIS IS NOT
//
// It is not a zone read. Momentum's start zones are mapper-tagged trigger
// brushes and WrLines cannot see one: it has no entity list, no netvars, no
// access to the game's timer, and no way to know a trigger fired. What it has is
// a world-to-screen matrix found by scanning memory, a camera solved out of it,
// and files on disk. That is the whole of it, and it is not going to change --
// wr_engine.cpp records what happened the last time this tool called into the
// engine.
//
// WHAT IT IS INSTEAD
//
// Every loaded run knows where its own run began (see startIndex in wr_path.h).
// A map with two hundred runs on it therefore carries two hundred independent
// observations of where the start is, recorded by two hundred different players.
// That is a better estimate of the place than one trigger read would be, and it
// comes with something a trigger read does not: a measured spread, so the panel
// can say how sure it is.
//
// WHY THE TRIGGER IS A PLANE AND NOT THE EDGE OF A BUBBLE
//
// points[startIndex] is the position at the instant the game's timer started,
// which is on the way OUT of the real zone rather than in the middle of it. Fit
// a circle to those points and you have a circle centred on the exit, not on the
// pad. Firing when you leave that circle would start the clock late by
// radius/speed -- half a second at 256 units and 500 u/s, which is enormous
// here.
//
// So the circle is only the ARMING region: it decides when you count as standing
// in the start. The moment that fires is crossing the plane through those points,
// outward, which is where their clocks actually started.
//
// WHAT IT CANNOT DO
//
//   - It does not know the zone's shape, size, or direction. It knows where some
//     clocks started and infers a point, a normal and a spread.
//   - It cannot find the start of a track no loaded run covers.
//   - It has no idea whether the GAME's timer is running. Practice mode, noclip
//     and save-loc loads all put our clock and the real one out of step, and
//     only ours is on screen. That was already true.
//   - There is no matching finish detector, and there should not be: a finish is
//     a line you cross once at speed, not a volume you wait in, so the same
//     machinery would be far less reliable while looking equally confident.

#ifndef WR_START_H
#define WR_START_H

#include "wr_common.h"

// Main, plus stages, plus bonuses. No real map comes near this.
#define WR_MAX_START_ZONES 24

struct WrStartZone
{
    unsigned char trackType, trackNum;

    // The MEDOID of the members' start positions, not the mean.
    //
    // One run whose start was recovered wrongly lands in the middle of the map,
    // and a mean would drag the answer thousands of units toward it. A medoid
    // needs no scratch array, is unmoved by an outlier, and -- the part that
    // actually matters -- is always a real recorded player origin, so anchoring
    // to it is directly comparable with a run's own t = 0.
    Vec3 centre;

    Vec3 outDir;            // mean heading at t = 0: the way out
    float radius;           // horizontal, the ARMING region
    float zLo, zHi;         // the floor band, so a ledge above is not "inside"

    // The floor itself, and whether there is one.
    //
    // Most starts are a flat pad, and when they are, every recovered start on the
    // leg sits within a few units of one horizontal plane. That is a thing the
    // members can be ASKED rather than assumed: `flat` is true when the p90
    // deviation from planeZ is under FLAT_PAD_UNITS, and it is false on a leg
    // whose starts are spread down a ramp or across a stairwell.
    //
    // Two things read it, and both are about the disc being the wrong shape.
    // points[startIndex] is where each clock STARTED, i.e. on the way out, so
    // the fitted circle is centred on the exit and part of the real pad falls
    // outside it -- stand there and nothing arms. On a flat leg the arming circle
    // is therefore allowed to grow, because the floor band is doing the work of
    // saying where you are and a wrongly armed state still cannot fire without
    // crossing the plane outward at speed.
    //
    // And planeZ, not centre.z, is the height to anchor the energy readout at:
    // centre is one member, chosen for being the medoid, and its own z is that
    // member's rather than the pad's.
    float planeZ;
    bool flat;

    float spread;           // p90 distance from the centre
    float alongSpread;      // p90 along outDir -- the trigger plane's own error

    int members;            // runs on this leg
    int trusted;            // ...whose start was actually recovered
    bool approx;            // built from points[0] because none of them was
    bool radiusCapped;      // the runs spread wider than the cap allows
};

enum WrStartState
{
    WR_START_AWAY = 0,      // not in any zone
    WR_START_INSIDE,        // in one, but moving or airborne
    WR_START_ARMED,         // in one, on the ground, slow: ready
    WR_START_LEFT           // crossed the plane; waiting to be able to re-arm
};

struct WrStartSettings
{
    bool enabled;
    bool autoAnchor;        // re-anchor the energy readout on the fire edge
    bool autoZeroClock;     // and zero the run clock
    bool showZone;          // draw the ring, the arrow and the plane
    float radiusScale;      // stretch the fitted radius, for odd geometry
    float leaveSpeed;       // how fast you must be going for a crossing to count
    float stillSpeed;       // and how slow to arm in the first place
};

extern WrStartSettings g_start;

void WrStartDefaults(void);
void WrStartReset(void);

// Once a frame, from inside WrTimerTick.
//
// The teleport flags are PASSED IN, not taken. WrEnergyTakeRestart and
// WrEnergyTakeTeleport are consume-once and have exactly one legal consumer --
// see wr_timer.cpp for what happened when two things believed they detected the
// same teleport. A second CONSUMER would be that defect again with the flag
// cleared out from under the first reader.
void WrStartTick(const Vec3 &cam, float dt, bool teleported);

// Consume-once: you crossed the start plane outward, having been armed.
bool WrStartTakeCrossed(const WrStartZone **which);

// The circle's radius as it is actually used: the fitted value with the
// "circle size" setting applied, and with the flat-pad allowance if the leg has
// one. Read this, not WrStartZone::radius, which is what the runs measured
// before either was applied.
float WrStartZoneRadius(const WrStartZone *z);

// Where to anchor the energy readout for this zone.
//
// centre with planeZ substituted on a flat leg, and centre itself otherwise.
// ONE function because there are two call sites -- the automatic anchor on the
// crossing edge and the button in the panel -- and two copies of a rule about
// which height to use is two chances to anchor at different heights for the same
// zone.
Vec3 WrStartZoneAnchor(const WrStartZone *z);

int WrStartZoneCount(void);
const WrStartZone *WrStartZoneAt(int i);
const WrStartZone *WrStartZoneHere(void);           // the one you are inside
const WrStartZone *WrStartZoneNearest(float *dist); // or the closest one

WrStartState WrStartStateNow(void);
const char *WrStartStateName(void);

// Why nothing is going to fire, in plain words. Empty when it would.
const char *WrStartWhyNot(void);

// Seconds since the last crossing, negative if there has not been one.
float WrStartSince(void);

#endif // WR_START_H
