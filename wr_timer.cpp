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

// The save-loc we are currently sitting exactly on top of, and whether we were
// sitting on one last frame. See the block in WrTimerTick: standing on a
// save-loc is a state, arriving on one is the event, and the load key holds you
// in the state for as long as you hold it down.
static Vec3 g_lastExact = {0.0f, 0.0f, 0.0f};
static bool g_wasExact = false;

// The save-loc table's version, so a table that changed under a stationary
// camera is not mistaken for the camera having arrived somewhere.
static unsigned int g_lastSavelocGen = 0;

void WrTimerReset(void)
{
    g_running = false;
    g_manual = false;
    g_elapsed = 0.0f;
    g_wasExact = false;
    g_lastExact = WrVec(0.0f, 0.0f, 0.0f);
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

    // A save-loc load that did NOT move you far enough to look like a teleport.
    //
    // The restore used to live entirely inside the teleport branch below, so it
    // only ever fired on a camera jump of more than 400 units. Load a save-loc
    // you are standing beside -- which is what practising a section IS -- and
    // the camera moves a few units or none at all, nothing is detected, and the
    // clock simply keeps counting. That is the whole of "saving a loc time does
    // not work": not a missing time, a missing event.
    //
    // Edge-triggered, because standing on the spot is a state and arriving on it
    // is the event: holding +mom_savestate_load parks you there for as long as
    // the key is down, and wr_energy.cpp is meanwhile holding the readout frozen
    // for exactly the same reason. Latched on WHICH save-loc rather than on "was
    // exact", so stepping from one to another an inch away still reads as an
    // arrival.
    //
    // Only when the teleport branch did not fire, so the one-lookup rule below
    // still holds: a given frame is answered by one search, never two.
    // Whether we are on one is worked out on EVERY frame, including teleport
    // frames and frames where the table has just changed underneath us. Only
    // whether to ACT on it is conditional -- see the two suppressions below.
    // Skipping the test entirely on those frames was the bug: the latch went
    // false, and the very next frame looked like a fresh arrival.
    // The table's version is tracked EVERY frame, not only on the frames we are
    // standing on a save-loc. Reading it inside the block below meant a change
    // that happened while you were somewhere else was still "new" whenever you
    // next arrived, and suppressed the arrival that mattered.
    unsigned int gen = WrSavelocGeneration();
    bool tableChanged = (gen != g_lastSavelocGen);
    g_lastSavelocGen = gen;

    bool exactNow = false;
    WrSavelocHit exact;
    if (WrSavelocExactMatch(cam, &exact) && exact.fromCps)
    {
        exactNow = true;

        // A teleport is the OTHER branch's business, and it has already
        // answered this frame. Momentum leaves you parked on the restored
        // origin, so without this the next frame reads as an arrival and fires
        // again -- re-restoring the clock, and seeding the energy from a
        // save-loc's velocity on a player a fail trigger has just stopped dead,
        // which is precisely what the refusal below exists to prevent.
        //
        // And a table that changed THIS frame. Making a save-loc where you
        // stand puts a new entry under a camera that has not moved, which is an
        // arrival by position and a creation in fact -- it would rewind the
        // clock by however long the background read took and overwrite "saved
        // at 12.5s" with "restored to 12.5s".
        bool suppress = teleported || tableChanged;

        if (suppress)
        {
            // Latch it anyway, so the frame after is not an edge either.
            g_lastExact = exact.pos;
        }
        else if (!g_wasExact || WrDist(exact.pos, g_lastExact) > 0.5f)
        {
            g_lastExact = exact.pos;
            if (exact.seconds >= 0.0f)
            {
                WrTimerSet(exact.seconds, "loaded a save-loc, without moving");
                WrSavelocNoteRestore(exact.seconds);
            }
            else
            {
                // Noticed, and there is nothing to restore. Said out loud,
                // because silence here looks exactly like not noticing at all,
                // and not noticing is what this whole block exists to fix.
                WrSavelocNoteNoTime();
                WrLogf("timer: landed exactly on a save-loc at (%.0f %.0f %.0f) "
                       "with no time of ours", exact.pos.x, exact.pos.y,
                       exact.pos.z);
            }

            // Same seed the teleport branch does, for the same reason: the
            // velocity is the GAME's record and exists for effectively every
            // save-loc, so a near load gets its readout back instantly instead
            // of waiting for the camera to be differenced. No restart to refuse
            // here -- a restart is raised by the teleport detector, and this
            // branch only runs when that did not fire.
            if (exact.haveVel)
                WrEnergySeed(cam, exact.vel, "a save-loc loaded in place");
        }
    }
    g_wasExact = exactNow;

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

        // And the attempt you just failed is worth keeping until the next one
        // begins. Left alone, the recorder wipes it on this very frame -- the
        // trip back to the pad is a long way -- so by the time you have opened
        // the panel to see what went wrong there is nothing to see, which reads
        // as the graph clearing itself every time you look at it.
        //
        // `restart` rather than raw `teleported`: a mid-map save-loc load should
        // still break the trail where it was and carry on, which is what it has
        // always done.
        //
        // Only where a start zone is actually known, because leaving one is the
        // ONLY thing that lets the hold go. With no runs loaded there are no
        // zones (they are fitted from the run store), nothing could ever release
        // it, and recording would stop for the session.
        if (g_start.enabled && WrStartZoneCount() > 0)
            WrLiveHold(true);
    }

    // The hold's last way out.
    //
    // It is normally let go of by leaving the start zone, or by the clock
    // re-starting from the anchor further down -- both of which are "the next
    // attempt has begun". Neither is guaranteed: the start machine has strict
    // re-arming rules and does not fire on every leg, and a hand-started clock
    // never takes the anchor path because it never stops running. That leaves a
    // recorder that is switched on and quietly recording nothing, which is a
    // worse failure than the one the hold fixes.
    //
    // So: three seconds of clock since the restart zeroed it is a new attempt
    // by any reading. Long enough that it cannot pre-empt either proper
    // release, short enough that nothing is lost.
    if (WrLiveHeld() && g_running && g_elapsed > 3.0f)
        WrLiveClear();

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

        // The new attempt starts here, so the old one stops being the answer.
        // Clearing rather than merely un-holding, so what the graph shows is one
        // attempt from the start line and not a session's worth of them -- and
        // because the clock was just zeroed a line above, keeping the old points
        // would put the time axis into reverse.
        WrLiveClear();
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

            // NOT a place to release the live hold, and it looked like one.
            //
            // This fires as soon as the camera is START_UNITS (32) from the
            // anchor, and the anchor is the chased run's start point -- which
            // is a hundred or three from the pad a fail trigger drops you on.
            // So on essentially every restart the distance is already over 32
            // on the very frame the hold is taken, and clearing here undid it
            // in the same WrTimerTick. The buffer was emptied exactly as before
            // and the graph was still blank when you went to look at it.
            //
            // The hold is let go of by leaving the start zone, which is what the
            // user asked for, and by the clock-based backstop above.
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
