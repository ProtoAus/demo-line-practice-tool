// wr_timer.cpp  --  see wr_timer.h.

#include "wr_timer.h"
#include "wr_energy.h"
#include "wr_path.h"
#include "wr_savelocs.h"
#include "wr_log.h"

#include <math.h>
#include <string.h>

// How far you have to leave the anchor before the clock starts. Small enough
// that it starts the moment you actually move, large enough that standing on the
// pad shuffling does not start it.
#define START_UNITS 32.0f

// A jump this large in one frame is a teleport, not movement -- the same test
// the matrix scan uses, for the same reason.
#define TELEPORT_UNITS 400.0f

static bool g_running = false;
static float g_elapsed = 0.0f;
static bool g_haveLast = false;
static Vec3 g_last;

void WrTimerReset(void)
{
    g_running = false;
    g_elapsed = 0.0f;
    g_haveLast = false;
}

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
    {
        g_haveLast = false;
        return;
    }

    Vec3 anchor;
    bool haveAnchor = WrEnergyAnchorPos(&anchor);

    // Consumed every frame, whether or not it is acted on, so it can never go
    // stale and fire a run later.
    bool restart = WrEnergyTakeRestart();

    // A teleport. Either it landed on a save-loc we have a time for, in which
    // case the clock goes back to what it said when that save-loc was made, or
    // it did not and the clock is left alone -- deliberately, because guessing
    // is worse than a number the user can see is stale.
    if (g_haveLast && WrDist(g_last, cam) > TELEPORT_UNITS)
    {
        float restored = 0.0f;
        if (WrSavelocTimeAt(cam, &restored))
        {
            // A save-loc is a more specific answer than "you are near the
            // start", so it wins over the restart below when both match.
            WrTimerSet(restored, "loaded a save-loc");
            restart = false;
        }
    }
    g_last = cam;
    g_haveLast = true;

    if (restart)
    {
        // A fail trigger, or the restart key. That is a new attempt rather than
        // a continuation, so the clock goes back to zero and re-arms -- it will
        // start again by itself when you leave the pad.
        g_running = false;
        g_elapsed = 0.0f;
    }

    if (!haveAnchor)
    {
        g_running = false;
        g_elapsed = 0.0f;
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

    g_elapsed += dt;
}

const char *WrTimerWhyNot(const WrRun *ref)
{
    if (!ref)
        return "no run enabled near you";
    if (!g_running)
        return "move away from the anchor to start the clock";
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

    float t = ref->points[ref->nearestIndex].t;
    if (ours) *ours = g_elapsed;
    if (theirs) *theirs = t;
    if (delta) *delta = g_elapsed - t;
    return true;
}
