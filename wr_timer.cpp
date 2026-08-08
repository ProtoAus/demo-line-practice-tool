// wr_timer.cpp  --  see wr_timer.h.

#include "wr_timer.h"
#include "wr_energy.h"
#include "wr_path.h"
#include "wr_savelocs.h"
#include "wr_start.h"
#include "wr_log.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

// How far you have to leave the anchor before the clock starts. Small enough
// that it starts the moment you actually move, large enough that standing on the
// pad shuffling does not start it.
#define START_UNITS 32.0f

// There is no teleport test here any more. There used to be a second copy of
// one, with its own last-position and its own 400-unit threshold, and the two
// disagreed on exactly the frame that mattered: a load slow enough to produce a
// frame over half a second made THIS one throw its last position away, so it
// skipped the teleport test entirely on the following frame, while the energy
// sampler kept its position and saw the teleport. A slow load is not an edge
// case, it is what loading looks like. WrLines has one camera history, so it has
// one teleport detector, and it lives in wr_energy.cpp.

static bool g_running = false;
static float g_elapsed = 0.0f;

// Started by hand rather than by leaving the anchor. Without this the clock is
// dead whenever no run is enabled nearby -- which also silently disabled the
// whole save-loc timing feature, since it only stamps while the clock runs.
static bool g_manual = false;

void WrTimerReset(void)
{
    g_running = false;
    g_manual = false;
    g_elapsed = 0.0f;
}

void WrTimerStart(void)
{
    g_manual = true;
    g_running = true;
    WrLogf("timer: started by hand");
}

void WrTimerStop(void)
{
    g_manual = false;
    g_running = false;
}

void WrTimerZero(void)
{
    g_elapsed = 0.0f;
}

bool WrTimerManual(void) { return g_manual; }

void WrTimerSet(float seconds, const char *why)
{
    g_elapsed = seconds;
    g_running = true;
    WrLogf("timer: set to %.2fs (%s)", seconds, why ? why : "");
}

bool WrTimerRunning(void) { return g_running; }
float WrTimerElapsed(void) { return g_elapsed; }

void WrTimerTick(const Vec3 &cam, float dt)
{
    if (!(dt > 0.0f) || dt > 0.5f)
        return;

    Vec3 anchor;
    bool haveAnchor = WrEnergyAnchorPos(&anchor);

    // Both consumed every frame, whether or not they are acted on, so neither
    // can go stale and fire a run later.
    bool restart = WrEnergyTakeRestart();
    Vec3 landed;
    bool teleported = WrEnergyTakeTeleport(&landed);

    // A teleport. One lookup answers two questions, and it has to be one lookup:
    // the clock restores from a time only WE ever recorded, which exists for
    // about four save-locs in a hundred, while the energy restores from a
    // velocity the GAME recorded, which exists for effectively all of them. Two
    // searches with those two different filters could match two different
    // save-locs and restore a clock from one and a speed from the other.
    if (teleported)
    {
        WrSavelocHit hit;
        if (WrSavelocMatch(landed, &hit))
        {
            if (hit.seconds >= 0.0f)
            {
                // A save-loc is a more specific answer than "you are near the
                // start", so it wins over the restart below when both match.
                WrTimerSet(hit.seconds, "loaded a save-loc");
                WrSavelocNoteRestore(hit.seconds);
                restart = false;
            }

            // The velocity does not depend on us ever having timed this
            // save-loc, so this runs on save-locs made years before WrLines
            // existed. `landed` is the camera, which is what the energy figure
            // is measured from -- not the origin in the file, which is the feet.
            //
            // Refused when this is a restart. A fail trigger drops you back at
            // the start, and if you keep a save-loc on the start pad -- which is
            // an ordinary thing to do -- the landing matches it. Seeding then
            // would claim the speed that save-loc was made at, on a player the
            // game has just stopped dead. The cost is that an untimed save-loc
            // within 384 units of the anchor loses its instant readout, and that
            // is a fair trade: those are spawn-adjacent, and a fifth of all
            // save-locs are made standing still anyway, where there is nothing
            // to seed.
            if (hit.haveVel && !restart)
                WrEnergySeed(landed, hit.vel, "a save-loc");
        }
    }

    if (restart)
    {
        // A fail trigger, or the restart key. That is a new attempt rather than
        // a continuation, so the clock goes back to zero and re-arms -- it will
        // start again by itself when you leave the pad.
        g_running = g_manual;   // a hand-started clock stays started
        g_elapsed = 0.0f;
    }

    // The start-zone machine, fed the flags rather than allowed to take them.
    //
    // Both of the take functions above are consume-once and this is their one
    // consumer; a second reader would clear a flag out from under the first,
    // which is the same defect as having had two teleport detectors. See the
    // note at the top of this file.
    WrStartTick(cam, dt, teleported);

    const WrStartZone *crossed = NULL;
    if (WrStartTakeCrossed(&crossed) && crossed)
    {
        // Placed AFTER the save-loc block on purpose: loading a save-loc is a
        // more specific statement about where you are in a run than crossing
        // the start line, and if both happen in one frame the save-loc is the
        // one that was actually asked for.
        if (g_start.autoAnchor && WrEnergyAnchorSource() != WR_ANCHOR_MANUAL)
            WrEnergyAnchorToStartZone(crossed->centre);
        if (g_start.autoZeroClock)
        {
            char why[64];
            _snprintf_s(why, sizeof(why), _TRUNCATE, "left the %s start",
                        WrTrackNameOf(crossed->trackType, crossed->trackNum));
            WrTimerSet(0.0f, why);
        }
        return;
    }

    if (!haveAnchor)
    {
        // No anchor is no longer the same as no clock. It used to zero and stop
        // the timer on every frame here, which made the whole save-loc timing
        // feature inert for anyone not chasing a loaded run -- and unrecoverable,
        // because the zeroing happened again the next frame. A hand-started
        // clock now simply keeps running.
        if (!g_manual)
            return;
        if (WrEnergyHeld())
            return;
        g_elapsed += dt;
        return;
    }

    if (!g_running)
    {
        // The anchor is a player-origin height for a run start and a camera
        // height for a manual one, so compare horizontally only -- otherwise a
        // run-start anchor never triggers, being 64 units below the camera.
        float dx = cam.x - anchor.x, dy = cam.y - anchor.y;
        if (sqrtf(dx * dx + dy * dy) > START_UNITS)
        {
            g_running = true;
            g_elapsed = 0.0f;
        }
        return;
    }

    // A paused demo must not accumulate time, or the you-versus-them delta is
    // wrong by however long you sat looking at it. A player standing perfectly
    // still mid-run reads the same way and their clock stops too, which is
    // wrong -- but standing still mid-surf is not a thing that happens, and the
    // alternative is the demo case being wrong every single time.
    if (WrEnergyHeld())
        return;

    g_elapsed += dt;
}

const char *WrTimerWhyNot(const WrRun *ref)
{
    if (!ref)
        return "no run enabled near you";
    if (!g_running)
        return "move away from the anchor to start the clock";
    if (g_manual)
        return "the clock was started by hand, so it does not line up with theirs";
    if (!ref->timingTrusted)
        return "this run's recovered timing is not reliable";
    return "";
}

bool WrTimerDelta(const WrRun *ref, float *ours, float *theirs, float *delta)
{
    if (WrTimerWhyNot(ref)[0])
        return false;
    if (ref->nearestIndex < 0 || ref->nearestIndex >= ref->pointCount)
        return false;

    // Their clock from THEIR run's start, because ours is zeroed at that same
    // place -- the anchor is points[startIndex]. Reading the raw stored `t` was
    // measuring from the first recorded sample instead, which put a constant
    // offset of the whole pre-roll into every delta: a median of 0.72 s, and
    // 1.74 s at worst. WrRunTimeAt is the one place that arithmetic lives.
    float t = WrRunTimeAt(ref, ref->nearestIndex);
    if (ours) *ours = g_elapsed;
    if (theirs) *theirs = t;
    if (delta) *delta = g_elapsed - t;
    return true;
}
