// test_live.cpp  --  the recording that has to survive a failed run, and the
// save-loc load that does not move you.
//
// Two behaviours that only exist as an interaction between three files, and
// that both look like "the tool does nothing" when they break.
//
// THE RECORDING. WrLiveRecord wipes its buffer when the camera jumps, which is
// right for a teleport and catastrophic for a fail trigger: failing drops you
// back on the pad, that is a long way, and the attempt you had just made was
// erased on that frame. You then open the panel to see what went wrong and the
// graph is empty -- which reads as the graph clearing itself when you look at
// it. So a restart now HOLDS the buffer and leaving the start zone clears it.
// The thing to test is that the hold cannot get stuck, because a stuck hold is
// a recorder that never records again.
//
// THE CLOCK. Restoring the run clock from a save-loc lived entirely inside the
// teleport detector, which needs a 400-unit camera jump between two frames.
// Momentum restores the exact stored origin, so loading a save-loc you are
// standing beside moves you a few units or none at all and nothing fired. The
// match is now an exact-position one with a one-unit radius, on a RISING EDGE,
// because holding +mom_savestate_load parks you on the spot and a level trigger
// would re-fire every frame for as long as the key is down.
//
// Build:  tests\build.bat
// Run:    tests\test_live.exe

#include "wr_path.h"
#include "wr_savelocs.h"
#include "wr_timer.h"
#include "wr_energy.h"
#include "wr_start.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-64s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

// The engine layer. The camera is settable, because the start-zone machine reads
// it directly -- WrStartZoneNearest calls WrCameraOrigin -- and the hold's
// release is measured against the zone it was taken at, so a harness that could
// not move the camera could not drive it.
static Vec3 g_cam = { 0.0f, 0.0f, 0.0f };
static bool g_camOk = false;

// The run store's harness seam, the same one test_start uses.
extern void WrPathTestLoad(const WrRun *runs, int count);

bool WrCameraForward(Vec3 *out) { if (out) *out = WrVec(1, 0, 0); return true; }
bool WrCameraOrigin(Vec3 *out)
{
    if (!g_camOk)
        return false;
    if (out) *out = g_cam;
    return true;
}

// One frame of everything, in the order dllmain runs it.
static void Frame(const Vec3 &cam, float dt)
{
    g_cam = cam;
    g_camOk = true;
    WrEnergySample(cam, dt);
    WrTimerTick(cam, dt);
    Vec3 p, v;
    if (WrEnergySampleAt(&p, &v))
        WrLiveRecord(p, v, WrTimerElapsed());
}

static WrSavelocHit MakeLoc(float x, float y, float z, float seconds,
                            float speed)
{
    WrSavelocHit h;
    memset(&h, 0, sizeof(h));
    h.pos = WrVec(x, y, z);
    h.vel = WrVec(speed, 0.0f, 0.0f);
    h.haveVel = (speed != 0.0f);
    h.fromCps = true;
    h.seconds = seconds;
    h.ordinal = 0;
    return h;
}

int main(void)
{
    printf("\n=== wrlines: the held recording, and a load that did not move you ===\n");

    // -----------------------------------------------------------------------
    printf("\nthe live buffer is wiped by a jump, and held through one\n");
    {
        WrLiveClear();
        WrLiveSetEnabled(true);
        for (int i = 0; i < 20; i++)
            WrLiveRecord(WrVec((float)i * 10.0f, 0.0f, 0.0f),
                         WrVec(500.0f, 0.0f, 0.0f), (float)i * 0.1f);
        int n = 0;
        WrLivePoints(&n);
        Check(n == 20, "twenty moves recorded twenty points");

        // Unheld, a long jump still breaks the trail -- that is what the branch
        // is for, and it must keep working.
        WrLiveRecord(WrVec(9000.0f, 0.0f, 0.0f), WrVec(0.0f, 0.0f, 0.0f), 0.0f);
        WrLivePoints(&n);
        Check(n == 1, "an unheld 8-thousand-unit jump restarts the buffer");

        // 450 units: a teleport everywhere else in the tool, and ordinary
        // movement to this file until the threshold was brought into line.
        WrLiveClear();
        WrLiveRecord(WrVec(0.0f, 0.0f, 0.0f), WrVec(0, 0, 0), 0.0f);
        WrLiveRecord(WrVec(450.0f, 0.0f, 0.0f), WrVec(0, 0, 0), 0.1f);
        WrLivePoints(&n);
        Check(n == 1, "a 450-unit jump breaks the trail, as it does elsewhere");

        // Held: the same jump changes nothing at all.
        WrLiveClear();
        for (int i = 0; i < 12; i++)
            WrLiveRecord(WrVec((float)i * 10.0f, 0.0f, 0.0f),
                         WrVec(500.0f, 0.0f, 0.0f), (float)i * 0.1f);
        WrLivePoints(&n);
        int before = n;
        WrLiveHold(true);
        Check(WrLiveHeld(), "the hold reads as held");
        WrLiveRecord(WrVec(9000.0f, 0.0f, 0.0f), WrVec(0, 0, 0), 0.0f);
        for (int i = 0; i < 5; i++)
            WrLiveRecord(WrVec(9000.0f + (float)i * 10.0f, 0.0f, 0.0f),
                         WrVec(500.0f, 0.0f, 0.0f), (float)i * 0.1f);
        WrLivePoints(&n);
        Check(n == before, "held, neither the jump nor the moves after it change it");

        // And the way out. Clearing is what a new attempt does, and it must also
        // let go -- otherwise the Clear button leaves a recorder that is on,
        // empty, and silently refusing to record.
        WrLiveClear();
        Check(!WrLiveHeld(), "clearing lets go of the hold");
        WrLiveRecord(WrVec(0.0f, 0.0f, 0.0f), WrVec(0, 0, 0), 0.0f);
        WrLiveRecord(WrVec(20.0f, 0.0f, 0.0f), WrVec(0, 0, 0), 0.1f);
        WrLivePoints(&n);
        Check(n == 2, "and recording resumes");
    }

    // -----------------------------------------------------------------------
    printf("\nthe time axis never goes backwards across a hold\n");
    {
        // The reason this is a hold and not "keep appending": a live point's t
        // is the run clock, and the clock is zeroed on a restart and again at
        // the start line. Appending across one would put the graph's own axis
        // into reverse, and wr_profile binary-searches it.
        WrLiveClear();
        for (int i = 0; i < 10; i++)
            WrLiveRecord(WrVec((float)i * 10.0f, 0.0f, 0.0f),
                         WrVec(500.0f, 0.0f, 0.0f), 5.0f + (float)i * 0.1f);
        WrLiveHold(true);
        // The clock has gone back to zero; every one of these is refused.
        for (int i = 0; i < 10; i++)
            WrLiveRecord(WrVec(500.0f + (float)i * 10.0f, 0.0f, 0.0f),
                         WrVec(500.0f, 0.0f, 0.0f), (float)i * 0.1f);

        int n = 0;
        const WrPoint *pts = WrLivePoints(&n);
        bool monotonic = true;
        for (int i = 1; i < n; i++)
            if (pts[i].t < pts[i - 1].t)
                monotonic = false;
        Check(monotonic, "t is still non-decreasing after a held restart");
        WrLiveClear();
    }

    // -----------------------------------------------------------------------
    printf("\nthe hold is never taken without somewhere to leave\n");
    {
        // The release is "you left the start you were put back in", so a hold
        // taken where no start zone could be latched has no way out except a
        // crossing that may never come -- and a recorder switched on and
        // silently recording nothing is a worse failure than the one the hold
        // fixes. So the latch is taken FIRST and the hold only if it succeeded.
        //
        // The v0.4.3 way out was a stopwatch, `g_running && g_elapsed > 3.0f`,
        // and it was worse than useless: the anchor test starts the clock as
        // soon as the camera is 32 units away from the anchor, and a fail leaves
        // you hundreds of units from it, so the clock started on essentially the
        // next frame and this fired three seconds after every fail -- before the
        // player had finished falling, let alone opened the panel. That is
        // exactly the symptom that was reported again after v0.4.3 shipped.
        WrSavelocInstallForTest(NULL, 0);
        WrEnergyDefaults();
        WrStartDefaults();
        WrPathTestLoad(NULL, 0);        // no runs, so no fitted zones
        WrStartReset();
        WrEnergyReset();
        WrTimerReset();
        WrTimerStart();
        WrTimerZero();

        WrLiveClear();
        WrLiveSetEnabled(true);
        g_camOk = true;

        // Walk somewhere, then jump far enough to be a teleport landing back at
        // the anchor -- a fail.
        WrEnergyAnchorToFeet(WrVec(0.0f, 0.0f, 0.0f));
        for (int i = 0; i < 40; i++)
            Frame(WrVec(600.0f + (float)i * 12.0f, 0.0f, 64.0f), 0.016f);
        Frame(WrVec(20.0f, 0.0f, 64.0f), 0.016f);   // back at the anchor

        Check(!WrLiveHeld(), "with no zone to leave, the fail does not hold");

        // And, being unheld, it keeps working rather than freezing for ever.
        int n = 0;
        for (int i = 0; i < 40; i++)
            Frame(WrVec(20.0f + (float)i * 12.0f, 0.0f, 64.0f), 0.016f);
        WrLivePoints(&n);
        Check(n > 4, "and the recorder is still recording");

        WrLiveClear();
        WrTimerReset();
    }

    // -----------------------------------------------------------------------
    printf("\na fail holds your line, and leaving the start lets it go\n");
    {
        // The whole of the reported bug, end to end. Two runs make one start
        // zone; a save-loc with a time sits on the pad, which is an ordinary
        // thing to keep there and is what broke it.
        WrEnergyDefaults();
        WrStartDefaults();
        WrEnergyReset();
        WrTimerReset();

        static WrRun runs[2];
        memset(runs, 0, sizeof(runs));
        for (int r = 0; r < 2; r++)
        {
            runs[r].pointCount = 200;
            runs[r].points = (WrPoint *)calloc(200, sizeof(WrPoint));
            for (int i = 0; i < 200; i++)
            {
                runs[r].points[i].pos = WrVec((float)i * 25.0f,
                                              (float)r * 8.0f, 0.0f);
                runs[r].points[i].vel = WrVec(600.0f, 0.0f, 0.0f);
                runs[r].points[i].t = (float)i * 0.015f;
            }
            runs[r].startIndex = 0;
            runs[r].startTrusted = true;
            runs[r].enabled = true;
            strcpy_s(runs[r].map, sizeof(runs[r].map), "surf_test");
        }
        WrPathTestLoad(runs, 2);

        g_camOk = true;
        g_cam = WrVec(0.0f, 0.0f, 64.0f);
        WrStartReset();
        // Zones are fitted lazily, on the first tick after the store changes --
        // WrStartZoneCount does not build, it reports.
        Frame(WrVec(0.0f, 0.0f, 64.0f), 0.016f);
        Check(WrStartZoneCount() > 0, "the two runs fitted a start zone");

        WrSavelocHit pad[1];
        pad[0] = MakeLoc(0.0f, 0.0f, 0.0f, 5.47f, 0.0f);   // timed, on the pad
        WrSavelocInstallForTest(pad, 1);

        WrEnergyAnchorToFeet(WrVec(0.0f, 0.0f, 0.0f));

        // An attempt: away from the pad, recording.
        WrLiveClear();
        WrLiveSetEnabled(true);
        for (int i = 0; i < 60; i++)
            Frame(WrVec(700.0f + (float)i * 20.0f, 0.0f, 64.0f), 0.016f);
        int had = 0;
        WrLivePoints(&had);
        Check(had > 8, "an attempt was recorded");

        // Fail. Momentum drops you ABOVE the pad and you fall in.
        //
        // 40 units out from the anchor, and that number is the whole point of
        // this section. The game respawns you at the start TRIGGER; the anchor
        // is the point the chased run's recording begins at, which is tens to
        // hundreds of units away. So a fail lands you further than START_UNITS
        // (32) from the anchor and the clock starts running immediately -- while
        // still well inside the fitted zone's radius (128 at its narrowest), so
        // you have not left the start.
        //
        // That gap is exactly where the v0.4.3 release condition lived: "held,
        // and the clock has passed three seconds" was true about three seconds
        // after every fail, which is before the player has finished falling. Put
        // the landing under the anchor instead and the clock never starts, the
        // stopwatch never fires, and this section passes with the bug in place.
        Frame(WrVec(40.0f, 0.0f, 400.0f), 0.016f);
        Check(WrLiveHeld(), "the fail holds the line");
        for (int i = 0; i < 20; i++)
            Frame(WrVec(40.0f, 0.0f, 400.0f - (float)i * 16.0f), 0.016f);

        Check(WrTimerElapsed() < 1.0f,
              "and the pad's 5.47s save-loc did NOT put itself on the clock");

        int now = 0;
        WrLivePoints(&now);
        Check(now == had, "the attempt is untouched while held");

        // Ten seconds of standing there -- far past the stopwatch that used to
        // throw this away.
        for (int i = 0; i < 600; i++)
            Frame(WrVec(40.0f + (float)(i % 3), 0.0f, 64.0f), 0.016f);
        Check(WrTimerRunning() && WrTimerElapsed() > 3.0f,
              "the clock has been running for well over three seconds");
        WrLivePoints(&now);
        Check(WrLiveHeld() && now == had,
              "ten seconds later it is still there, which is the whole point");

        // Now leave the start. Anywhere past the fitted circle will do.
        for (int i = 0; i < 200; i++)
            Frame(WrVec(40.0f + (float)i * 20.0f, 0.0f, 64.0f), 0.016f);
        Check(!WrLiveHeld(), "leaving the start lets it go");

        WrLiveClear();
        WrTimerReset();
        WrPathTestLoad(NULL, 0);
        for (int r = 0; r < 2; r++)
            free(runs[r].points);
    }

    // -----------------------------------------------------------------------
    printf("\nthe exact matcher answers where the 24-unit one is too generous\n");
    {
        WrSavelocHit rows[3];
        rows[0] = MakeLoc(100.0f, 200.0f, 50.0f, 12.5f, 1800.0f);
        rows[1] = MakeLoc(140.0f, 200.0f, 50.0f, -1.0f, 0.0f);   // untimed
        rows[2] = MakeLoc(-900.0f, 0.0f, 0.0f, 3.25f, 0.0f);
        WrSavelocInstallForTest(rows, 3);

        WrSavelocHit hit;
        memset(&hit, 0xCD, sizeof(hit));
        Check(WrSavelocExactMatch(WrVec(100.0f, 200.0f, 114.0f), &hit),
              "dead on the origin matches, 64 units of eye height above it");
        Check(fabsf(hit.seconds - 12.5f) < 0.001f, "and it is the right one");
        Check(hit.fromCps, "fromCps is filled rather than left as stack garbage");
        Check(hit.ordinal == 0, "and so is the ordinal");

        Check(WrSavelocExactMatch(WrVec(100.7f, 200.0f, 50.0f), NULL),
              "0.7 units away still matches");
        Check(!WrSavelocExactMatch(WrVec(103.0f, 200.0f, 50.0f), NULL),
              "3 units away does not");
        Check(!WrSavelocExactMatch(WrVec(118.0f, 200.0f, 50.0f), NULL),
              "and neither does 18, which the ordinary matcher accepts");
        Check(WrSavelocMatch(WrVec(118.0f, 200.0f, 50.0f), NULL),
              "-- confirming the ordinary matcher does accept it");

        // The vertical band is deliberately NOT tightened: eyeHeight is a
        // setting, and ducking changes the real offset. A save-loc 90 units
        // below the camera is the same save-loc.
        Check(WrSavelocExactMatch(WrVec(100.0f, 200.0f, 140.0f), NULL),
              "90 units of height difference is still the same save-loc");
        Check(!WrSavelocExactMatch(WrVec(100.0f, 200.0f, 400.0f), NULL),
              "350 is not");
    }

    // -----------------------------------------------------------------------
    printf("\nthe clock restores on arrival, once, however long you hold\n");
    {
        WrSavelocHit rows[2];
        rows[0] = MakeLoc(0.0f, 0.0f, 0.0f, 7.75f, 0.0f);
        rows[1] = MakeLoc(4000.0f, 0.0f, 0.0f, -1.0f, 0.0f);     // untimed
        WrSavelocInstallForTest(rows, 2);

        WrEnergyDefaults();
        WrStartDefaults();
        g_start.enabled = false;        // no run store here, so no zones anyway
        WrEnergyReset();
        WrTimerReset();
        WrTimerStart();                 // by hand, so it runs with no anchor
        WrTimerZero();

        // A long way from anything, the clock just counts.
        for (int i = 0; i < 10; i++)
        {
            Vec3 cam = WrVec(-8000.0f + (float)i, 0.0f, 0.0f);
            WrEnergySample(cam, 0.016f);
            WrTimerTick(cam, 0.016f);
        }
        float away = WrTimerElapsed();
        Check(away > 0.10f && away < 0.20f, "away from every save-loc it counts");

        // Now the load: the camera is put exactly on the save-loc. This is far
        // enough to be a teleport as well, which is the case that already
        // worked -- so walk in from close by instead, under the 400-unit
        // threshold, which is the case that did not.
        WrEnergyReset();
        WrTimerZero();
        for (int i = 0; i < 5; i++)
        {
            Vec3 cam = WrVec(200.0f - (float)i * 40.0f, 0.0f, 64.0f);
            WrEnergySample(cam, 0.016f);
            WrTimerTick(cam, 0.016f);
        }
        Check(WrTimerElapsed() < 1.0f, "approaching it, nothing has fired yet");

        Vec3 on = WrVec(0.0f, 0.0f, 64.0f);
        WrEnergySample(on, 0.016f);
        WrTimerTick(on, 0.016f);
        // Within one frame of the recorded time, not exactly it: the restore
        // happens near the top of WrTimerTick and the same call then advances
        // the clock by this frame's dt, which is correct -- the frame did
        // happen -- and is 16 ms.
        float restored = WrTimerElapsed();
        Check(restored >= 7.75f && restored < 7.75f + 0.02f,
              "arriving on it sets the clock to its recorded time");
        Check(WrSavelocRecentKind() == WR_NOTE_RESTORED,
              "and says so, rather than doing it silently");

        // Held. Momentum freezes you on the spot for as long as the key is
        // down; a level trigger would re-set the clock every frame and it would
        // never advance past the restored value.
        for (int i = 0; i < 30; i++)
        {
            WrEnergySample(on, 0.016f);
            WrTimerTick(on, 0.016f);
        }
        Check(WrTimerElapsed() > 7.75f,
              "and holding the key does not pin it there -- the clock advances");

        // The untimed one. It must be NOTICED -- silence there is what made the
        // whole feature look broken, because it is indistinguishable from not
        // having seen the load at all.
        WrEnergyReset();
        Vec3 near2 = WrVec(4200.0f, 0.0f, 64.0f);
        WrEnergySample(near2, 0.016f);
        WrTimerTick(near2, 0.016f);
        Vec3 on2 = WrVec(4000.0f, 0.0f, 64.0f);
        float wasAt = WrTimerElapsed();
        WrEnergySample(on2, 0.016f);
        WrTimerTick(on2, 0.016f);
        Check(WrSavelocRecentKind() == WR_NOTE_NO_TIME,
              "an untimed save-loc reports that it has no time");
        Check(WrTimerElapsed() >= wasAt,
              "and the clock is left alone rather than zeroed");
    }

    // -----------------------------------------------------------------------
    printf("\na restart driven through the clock does not wipe the recording\n");
    {
        // The hold and its release both live in WrTimerTick, and driving them
        // by hand proves nothing about the order they run in. The first version
        // of this took the hold and then let it go in the SAME tick: the
        // anchor-based clock start fires the moment you are 32 units from the
        // anchor, a fail trigger drops you a couple of hundred from it, so the
        // buffer was emptied on the restart frame exactly as before.
        WrSavelocInstallForTest(NULL, 0);
        WrEnergyDefaults();
        WrStartDefaults();
        g_start.enabled = false;
        WrEnergyReset();
        WrTimerReset();

        Vec3 pad = WrVec(0.0f, 0.0f, 0.0f);
        WrEnergyAnchorToStartZone(pad);

        // Out on the run, recording.
        WrLiveClear();
        for (int i = 0; i < 40; i++)
        {
            Vec3 cam = WrVec(200.0f + (float)i * 40.0f, 0.0f, 0.0f);
            WrEnergySample(cam, 0.016f);
            WrTimerTick(cam, 0.016f);
            WrLiveRecord(cam, WrVec(1500.0f, 0.0f, 0.0f), WrTimerElapsed());
        }
        int n = 0;
        WrLivePoints(&n);
        Check(n > 20, "forty moves out from the pad recorded a path");
        int before = n;

        // Fail: back to the pad in one frame. Far enough to be a teleport, near
        // enough to the anchor to be a restart.
        WrLiveHold(true);
        Vec3 back = WrVec(150.0f, 0.0f, 0.0f);
        WrEnergySample(back, 0.016f);
        WrTimerTick(back, 0.016f);
        WrLiveRecord(back, WrVec(0, 0, 0), WrTimerElapsed());

        WrLivePoints(&n);
        Check(n == before, "the attempt survives the restart frame");
        Check(WrLiveHeld(), "and it is still held afterwards");
        WrLiveClear();
        WrTimerReset();
    }

    // -----------------------------------------------------------------------
    printf("\nan arrival is not counted twice, and a creation is not an arrival\n");
    {
        WrSavelocHit rows[1];
        rows[0] = MakeLoc(0.0f, 0.0f, 0.0f, 20.0f, 0.0f);
        WrSavelocInstallForTest(rows, 1);

        WrEnergyDefaults();
        WrStartDefaults();
        g_start.enabled = false;
        WrEnergyReset();
        WrTimerReset();
        WrTimerStart();
        WrTimerZero();

        // Far away, then a jump straight onto it: over 400 units, so this is
        // the teleport branch's arrival, not the exact matcher's.
        Vec3 far1 = WrVec(-5000.0f, 0.0f, 64.0f);
        for (int i = 0; i < 4; i++)
        {
            WrEnergySample(far1, 0.016f);
            WrTimerTick(far1, 0.016f);
        }
        Vec3 on = WrVec(0.0f, 0.0f, 64.0f);
        WrEnergySample(on, 0.016f);
        WrTimerTick(on, 0.016f);
        Check(WrTimerElapsed() >= 20.0f && WrTimerElapsed() < 20.05f,
              "the teleport branch restored it");

        // The camera is still parked there. If the exact matcher treats the
        // next frame as an arrival it re-restores, and the clock is pinned at
        // the stored time for as long as you stand there -- and it would seed
        // the energy from that save-loc on a player a fail trigger has just
        // stopped dead, which the teleport branch deliberately refuses to do.
        // Nudged by hundredths to exercise several distinct samples. Still well
        // inside the one-unit circle, so it is the same save-loc throughout.
        for (int i = 0; i < 10; i++)
        {
            Vec3 c = WrVec(0.02f * (float)i, 0.0f, 64.0f);
            WrEnergySample(c, 0.016f);
            WrTimerTick(c, 0.016f);
        }
        Check(WrTimerElapsed() > 20.1f,
              "the frame after a teleport is not a second arrival");

        // And a save-loc appearing under a camera that has not moved is a
        // creation, not a load. Same position, new table.
        float wasAt = WrTimerElapsed();
        WrSavelocHit rows2[2];
        rows2[0] = rows[0];
        rows2[1] = MakeLoc(0.0f, 0.0f, 0.0f, 20.0f, 0.0f);
        rows2[1].ordinal = 1;
        WrSavelocInstallForTest(rows2, 2);
        WrEnergySample(on, 0.016f);
        WrTimerTick(on, 0.016f);
        Check(WrTimerElapsed() >= wasAt,
              "making one where you stand does not rewind the clock");
        WrTimerReset();
    }

    // -----------------------------------------------------------------------
    printf("\na fail is not a load, and the quarantine ends when you move\n");
    {
        // The other half of the same bug. A save-loc kept on the start pad is
        // an ordinary thing, and every fail lands within the 24-unit match of
        // it -- so the teleport branch restored its time on every fail, put a
        // number on screen for two seconds, and (by clearing `restart`) skipped
        // the block that holds the recording. Both reported symptoms, one line.
        //
        // A restart is not one frame, either: the fail puts you above the pad
        // and you fall in, so the exact matcher fires several frames after the
        // teleport flag has been consumed. That is what the quarantine is for.
        WrSavelocHit rows[1];
        rows[0] = MakeLoc(0.0f, 0.0f, 0.0f, 5.47f, 0.0f);
        WrSavelocInstallForTest(rows, 1);

        WrEnergyDefaults();
        WrStartDefaults();
        g_start.enabled = false;
        WrEnergyReset();
        WrTimerReset();
        WrTimerStart();
        WrTimerZero();
        WrEnergyAnchorToFeet(WrVec(0.0f, 0.0f, 0.0f));

        // Out on the run for a while.
        for (int i = 0; i < 40; i++)
            Frame(WrVec(900.0f + (float)i * 30.0f, 0.0f, 64.0f), 0.016f);
        float ranFor = WrTimerElapsed();
        Check(ranFor > 0.3f, "the clock ran during the attempt");

        // Fail, landing DIRECTLY on the pad -- inside the 24-unit matcher's
        // 96-unit vertical band, so the teleport branch itself finds the
        // save-loc. This is the log line that reads
        //   [244.281] energy: teleported back to the anchor ... restart
        //   [244.281] timer: set to 5.47s (loaded a save-loc)
        // on one frame, and it is the precedence, not the quarantine, that has
        // to refuse it.
        // The clock alone cannot tell these two apart, and that is worth
        // stating: the restart block a few lines further down zeroes it either
        // way, so a restore that fires and is then overwritten leaves the clock
        // looking correct. What the player actually saw was the ANNOUNCEMENT --
        // WrSavelocNoteRestore, which the HUD borrows the clock row for two
        // seconds to show, which is the "a timer appears for a second when I
        // fail" report. So the note is what this checks. Poisoned first with a
        // known value, so "unchanged" is a real observation rather than a
        // leftover from the section above.
        WrSavelocNoteNoTime();
        Frame(WrVec(1.0f, 0.0f, 64.0f), 0.016f);
        Check(WrSavelocRecentKind() != WR_NOTE_RESTORED,
              "a fail landing ON the save-loc does not announce a restore");
        Check(WrTimerElapsed() < 1.0f, "and the clock is at zero, not at 5.47");

        // And again the way it actually happens: dropped ABOVE the pad, out of
        // the vertical band, falling in over the following frames. The teleport
        // flag is long consumed by the time the exact matcher can see anything,
        // which is what the quarantine is for -- measured at 125 ms in the log
        // this was diagnosed from.
        for (int i = 0; i < 40; i++)
            Frame(WrVec(1.0f + (float)i * 8.0f, 0.0f, 64.0f), 0.016f);
        // 0.3 units off, not 1.0: MATCH_EXACT_RADIUS is 1.0 and the test is
        // strict, so landing exactly on the radius misses -- which would make
        // this section pass without ever reaching the code it is about.
        WrSavelocNoteNoTime();
        Frame(WrVec(0.3f, 0.0f, 420.0f), 0.016f);
        for (int i = 0; i < 25; i++)
            Frame(WrVec(0.3f, 0.0f, 420.0f - (float)i * 18.0f), 0.016f);

        Check(WrSavelocRecentKind() != WR_NOTE_RESTORED,
              "and neither does one that drops you above it and lets you fall");
        Check(WrTimerElapsed() < 1.0f, "the clock is still at zero after that");

        // And once you have walked away and come back deliberately, it works.
        // 96 units is the quarantine's own distance; go well past it.
        for (int i = 0; i < 40; i++)
            Frame(WrVec(1.0f + (float)i * 12.0f, 0.0f, 64.0f), 0.016f);
        Check(WrTimerElapsed() > 0.4f, "the clock is running again");

        // Now a load that does not move you: step onto the exact spot.
        Frame(WrVec(300.0f, 0.0f, 64.0f), 0.016f);
        Frame(WrVec(0.3f, 0.0f, 64.0f), 0.016f);
        Check(WrTimerElapsed() >= 5.47f && WrTimerElapsed() < 5.6f,
              "a deliberate load after moving away still restores");

        WrLiveClear();
        WrTimerReset();
        WrSavelocInstallForTest(NULL, 0);
    }

    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
