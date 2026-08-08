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
#include <string.h>
#include <math.h>

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-64s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

// The engine layer, which none of this needs.
bool WrCameraForward(Vec3 *out) { if (out) *out = WrVec(1, 0, 0); return true; }
bool WrCameraOrigin(Vec3 *out) { (void)out; return false; }

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
    printf("\nthe hold cannot strand the recorder\n");
    {
        // A hand-started clock never stops, so it never takes the anchor path
        // that normally lets the hold go; and a leg with no fitted start zone
        // never fires the crossing either. A recorder that is switched on and
        // silently recording nothing is a worse failure than the one the hold
        // exists to fix, so there is a last way out.
        WrSavelocInstallForTest(NULL, 0);
        WrEnergyDefaults();
        WrStartDefaults();
        g_start.enabled = false;
        WrEnergyReset();
        WrTimerReset();
        WrTimerStart();
        WrTimerZero();

        WrLiveClear();
        for (int i = 0; i < 8; i++)
            WrLiveRecord(WrVec((float)i * 10.0f, 0.0f, 0.0f),
                         WrVec(500.0f, 0.0f, 0.0f), (float)i * 0.1f);
        WrLiveHold(true);

        // The camera has to actually move: a bit-identical one reads as a paused
        // demo and the clock deliberately stops, which would make this test
        // pass or fail for the wrong reason.
        int frame = 0;
        for (; frame < 60; frame++)      // ~1 second of clock
        {
            Vec3 cam = WrVec(-30000.0f + (float)frame * 4.0f, 0.0f, 0.0f);
            WrEnergySample(cam, 0.016f);
            WrTimerTick(cam, 0.016f);
        }
        Check(WrLiveHeld(), "a second in, it is still held");
        for (; frame < 300; frame++)     // past three seconds
        {
            Vec3 cam = WrVec(-30000.0f + (float)frame * 4.0f, 0.0f, 0.0f);
            WrEnergySample(cam, 0.016f);
            WrTimerTick(cam, 0.016f);
        }
        Check(!WrLiveHeld(), "three seconds of clock later, it has let go");
        WrLiveClear();
        WrTimerReset();
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
        // Nudged by hundredths so the camera is not bit-identical -- a still
        // camera reads as a paused demo and the clock deliberately stops, which
        // would make this pass for the wrong reason. Still well inside the
        // one-unit circle, so it is the same save-loc throughout.
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

    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
