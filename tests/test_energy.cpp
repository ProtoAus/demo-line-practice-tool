// test_energy.cpp  --  drive the energy filters with a scripted trajectory.
//
// The reported defect was "the number flickers around at ramp ends and when
// dropping from a platform". It was not noise: the reference height re-armed
// itself whenever a ground heuristic fired, and that heuristic fires at the apex
// of every arc. The zero point was moving.
//
// The decisive property is physics: in free flight E = z + v^2/2g is CONSERVED.
// So against a fixed anchor, a whole jump must read as a flat line. This drives
// the real estimator chain with a real ballistic arc plus view bob, and asserts
// exactly that -- then runs the old re-arming policy over the same trajectory to
// show what it did.
//
// The last section drives the REAL sampler rather than a copy of the chain,
// because the second reported defect did not live in the filters at all: it was
// the order of two statements in WrEnergySample. See "a fail trigger" below.
//
// Build:  tests\build.bat        (or see the command in that file)
// Run:    tests\test_energy.exe

#include "wr_smooth.h"
#include "wr_stress.h"
#include "wr_budget.h"
#include "wr_energy.h"
#include "wr_engine.h"

#include <stdio.h>
#include <math.h>

// wr_energy.cpp reaches outside itself for exactly one thing -- the camera
// forward vector, for the view turn rate -- so it is stubbed here rather than
// dragging the whole memory scanner into a unit test. Everything else it needs
// (WrLength, WrDist, WrSaneVec, WrLogf) is in wr_log.cpp, which links cleanly:
// WrLogf is a no-op until WrLogInit runs, so the test writes no log file.
bool WrCameraForward(Vec3 *out)
{
    if (out) *out = WrVec(1.0f, 0.0f, 0.0f);
    return true;
}

// Mirrors SETTLE_TAUS in wr_energy.cpp -- how many time constants the output
// filter is given to converge after a discontinuity before the gain/loss
// accumulator starts banking again.
#define SETTLE_TAUS 3.0f

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

static const float G = 800.0f;

// Deterministic view bob, so the numbers in a comment stay the numbers.
static unsigned int g_seed = 7;
static float Noise(void)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return ((float)((g_seed >> 16) & 0x7FFF) / 32767.0f) * 2.0f - 1.0f;  // -1..1
}

struct Chain
{
    WrVelWindow win;
    WrEma vx, vy, vz, energy;
    float refZ;
    bool haveRef;
};

static void ChainInit(Chain *c, float refZ)
{
    WrVelReset(&c->win);
    WrEmaReset(&c->vx); WrEmaReset(&c->vy); WrEmaReset(&c->vz);
    WrEmaReset(&c->energy);
    c->refZ = refZ;
    c->haveRef = true;
}

// One frame of the real chain: window -> per-axis EMA -> E -> EMA. Returns the
// relative energy, or a huge sentinel while the window is still filling.
static float ChainStep(Chain *c, float x, float y, float z, float dt,
                       float *outVz)
{
    WrVelPush(&c->win, x, y, z, dt);
    float rx = 0, ry = 0, rz = 0, mx = 0, my = 0, mz = 0;
    if (!WrVelEstimate(&c->win, 0.040f, &rx, &ry, &rz, &mx, &my, &mz))
        return 1e9f;

    // The smoothed velocity is for display only. The energy uses the window
    // velocity paired with the window's midpoint height, so both refer to the
    // same instant -- see WrVelEstimate.
    float vx = WrEmaStep(&c->vx, rx, dt, 0.060f);
    float vy = WrEmaStep(&c->vy, ry, dt, 0.060f);
    float vz = WrEmaStep(&c->vz, rz, dt, 0.060f);
    if (outVz) *outVz = vz;

    float e = mz + (rx * rx + ry * ry + rz * rz) / (2.0f * G);
    float smooth = WrEmaStep(&c->energy, e, dt, 0.30f);
    return smooth - c->refZ;
}

int main(void)
{
    printf("\na ballistic arc against a fixed anchor is a flat line\n");
    {
        // Straight off a platform: 900 u/s forward, 268 u/s up (a Source jump),
        // falling for 1.6 s. Two units of view bob at 15 Hz on top.
        const float dt = 1.0f / 200.0f;
        const float z0 = 1000.0f;
        Chain c;
        ChainInit(&c, z0);

        float lo = 1e9f, hi = -1e9f;
        // Three time constants of the 0.30 s output filter, so it has charged to
        // 95% before anything is judged. Below this you measure the filter, not
        // the trajectory -- the first attempt used 0.35 s and read an 88-unit
        // spread that was almost entirely the EMA still catching up.
        float tSettle = 0.90f;
        for (int i = 0; i < 320; i++)
        {
            float t = i * dt;
            float x = 900.0f * t;
            float z = z0 + 268.0f * t - 0.5f * G * t * t;
            float bob = 2.0f * sinf(t * 15.0f * 6.28318f) + 0.35f * Noise();
            float rel = ChainStep(&c, x, 0.0f, z + bob, dt, 0);
            if (rel > 1e8f || t < tSettle)
                continue;
            if (rel < lo) lo = rel;
            if (rel > hi) hi = rel;
        }
        printf("     relative energy over the whole arc: %.1f .. %.1f (spread %.1f)\n",
               lo, hi, hi - lo);
        // Conserved energy for this launch is (900^2 + 268^2) / 1600 = 551 above
        // the platform -- the horizontal component counts too, which is the
        // whole reason this metric beats a speedometer.
        Check(hi - lo < 15.0f, "stays within 15 units across the entire flight");
        Check(lo > 500.0f && hi < 600.0f, "and sits at the launch energy, ~551");
    }

    printf("\nwhat the old re-arming reference did to the same arc\n");
    {
        // The old rule: whenever |smoothed vz| < 30 held for 3 frames inside a
        // 6-unit band, refZ was set to the current z. Replayed here over the
        // identical trajectory.
        const float dt = 1.0f / 200.0f;
        const float z0 = 1000.0f;
        Chain c;
        ChainInit(&c, z0);
        g_seed = 7;

        int settled = 0;
        float settledZ = 0.0f, refZ = z0;
        float worstStep = 0.0f, prev = 1e9f;
        int rearms = 0;

        for (int i = 0; i < 320; i++)
        {
            float t = i * dt;
            float x = 900.0f * t;
            float z = z0 + 268.0f * t - 0.5f * G * t * t;
            float bob = 2.0f * sinf(t * 15.0f * 6.28318f) + 0.35f * Noise();
            float vz = 0.0f;
            float absRel = ChainStep(&c, x, 0.0f, z + bob, dt, &vz);
            if (absRel > 1e8f)
                continue;

            float zc = z + bob;
            if (fabsf(vz) < 30.0f)
            {
                if (settled == 0) settledZ = zc;
                if (fabsf(zc - settledZ) < 6.0f) settled++;
                else { settled = 1; settledZ = zc; }
            }
            else settled = 0;

            if (settled >= 3)
            {
                if (fabsf(zc - refZ) > 1.0f) rearms++;
                refZ = zc;                      // the bug, verbatim
            }

            float rel = (absRel + c.refZ) - refZ;
            if (prev < 1e8f && fabsf(rel - prev) > worstStep)
                worstStep = fabsf(rel - prev);
            prev = rel;
        }
        printf("     reference re-armed on %d frames mid-flight, worst single-frame "
               "step %.1f units\n", rearms, worstStep);
        Check(rearms > 0, "the old rule really does re-arm during free flight");
    }

    printf("\nstanding still reads a steady zero\n");
    {
        const float dt = 1.0f / 200.0f;
        Chain c;
        ChainInit(&c, 1000.0f);
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < 600; i++)
        {
            float t = i * dt;
            float bob = 1.2f * sinf(t * 12.0f * 6.28318f) + 0.4f * Noise();
            float rel = ChainStep(&c, 0.0f, 0.0f, 1000.0f + bob, dt, 0);
            if (rel > 1e8f || t < 0.4f) continue;
            if (rel < lo) lo = rel;
            if (rel > hi) hi = rel;
        }
        printf("     %.2f .. %.2f\n", lo, hi);
        Check(fabsf(lo) < 6.0f && fabsf(hi) < 6.0f,
              "never leaves a 6-unit band while motionless");
    }

    printf("\nthe filters do not change with the frame rate\n");
    {
        const float rates[3] = { 60.0f, 200.0f, 500.0f };
        float finals[3];
        for (int r = 0; r < 3; r++)
        {
            float dt = 1.0f / rates[r];
            Chain c;
            ChainInit(&c, 1000.0f);
            g_seed = 7;
            float rel = 0.0f;
            for (float t = 0.0f; t < 1.2f; t += dt)
            {
                float x = 1500.0f * t;
                float v = ChainStep(&c, x, 0.0f, 1000.0f, dt, 0);
                if (v < 1e8f) rel = v;
            }
            finals[r] = rel;
            printf("     %5.0f fps -> %.1f\n", rates[r], rel);
        }
        float spread = 0.0f;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                if (fabsf(finals[i] - finals[j]) > spread)
                    spread = fabsf(finals[i] - finals[j]);
        // 1500 u/s flat is 1500^2/1600 = 1406 units of energy.
        Check(spread < 6.0f, "60 fps and 500 fps agree to within 6 units");
    }

    printf("\nthe arrow does not strobe on a noisy trend\n");
    {
        WrArrow a;
        WrArrowReset(&a);
        const float dt = 1.0f / 200.0f;
        int flips = 0, last = 0;
        g_seed = 99;
        for (int i = 0; i < 2000; i++)
        {
            // A trend hovering exactly on the threshold, the worst case.
            float trend = 12.0f + 6.0f * Noise();
            int s = WrArrowStep(&a, trend, 12.0f, dt, 0.20f);
            if (s != last) { flips++; last = s; }
        }
        printf("     %d changes in 10 s sitting on the threshold\n", flips);
        Check(flips <= 6, "at most a handful of changes, not one per frame");
    }

    printf("\nthe air-strafing ceiling is where the physics says it is\n");
    {
        // ws^2 / (2 g tick) = 900 / (2*800*0.015). Measured against twelve
        // record-class runs, the 99th percentile of dE/dt lands at +38.18..+38.88.
        float p = WrAirPowerCeiling(800.0f, 0.015f);
        printf("     P_max = %.2f energy units/s (measured p99 across real runs: "
               "38.18-38.88)\n", p);
        Check(fabsf(p - 37.5f) < 0.01f, "37.5 units/s at g=800 on a 66 tick");
        Check(WrAirPowerCeiling(800.0f, 0.0075f) > p,
              "a faster tick raises it, as 1/tick");

        Check(fabsf(WrEfficiency(37.5f, p) - 1.0f) < 0.001f,
              "gaining at the ceiling is eta = 1");
        Check(WrEfficiency(0.0f, p) == 0.0f, "free flight is exactly 0");
        Check(WrEfficiency(-40.0f, p) < -0.9f, "losing hard is strongly negative");
        Check(WrEfficiency(9039.0f, p) == 0.0f,
              "a booster (measured +9039 u/s in a real run) is rejected, not "
              "drawn as perfect");

        signed char b = WrEtaToByte(0.531f);   // the surf_demise WR median
        Check(fabsf(WrEtaFromByte(b) - 0.531f) < 0.01f,
              "the stored byte round-trips to within 0.01");

        // The rejection is one-sided. Losing at ten times the ceiling is a ramp
        // entry or a wall, not a trigger, and it used to be collapsed to the
        // same 0.0 as free flight -- 18.8% of all samples and 94.6% of all
        // energy lost in the library.
        Check(WrEfficiency(-400.0f, p) == -1.0f,
              "losing at ten times the ceiling reads as fully losing");
        Check(!WrEtaIsNoData(-400.0f, p), "and is not treated as missing data");
        Check(WrEtaIsNoData(9039.0f, p), "while a booster still is");

        // An EVEN bucket count over a symmetric range has no bucket at zero, so
        // free flight drew as a faintly green grey.
        Check(WrEtaFromBucket(WrEtaBucket(0.0f, 17), 17) == 0.0f,
              "an odd bucket count round-trips eta 0 to exactly 0");
        Check(WrEtaFromBucket(WrEtaBucket(0.0f, 16), 16) != 0.0f,
              "an even one does not, which is what it used to be");

        // The deadstrafe period: sv_airaccelerate decides whether it matters.
        float full150 = WrAirPowerCeilingEx(800.0f, 0.015f, 150.0f, 250.0f, 1.0f);
        float dead150 = WrAirPowerCeilingEx(800.0f, 0.015f, 150.0f, 250.0f, 0.25f);
        float full12 = WrAirPowerCeilingEx(800.0f, 0.015625f, 12.0f, 250.0f, 1.0f);
        float dead12 = WrAirPowerCeilingEx(800.0f, 0.015625f, 12.0f, 250.0f, 0.25f);
        printf("     quarter surface friction: airaccel 150 %.2f -> %.2f, "
               "airaccel 12 %.2f -> %.2f\n", full150, dead150, full12, dead12);
        Check(fabsf(full150 - dead150) < 0.01f,
              "at airaccel 150 the deadstrafe period does not lower the ceiling");
        Check(dead12 < full12 * 0.7f,
              "at airaccel 12 it lowers it by a third, as the KZ community "
              "reports");
    }

    printf("\nturn rate alone would fire on perfect play, which is why it is not\n"
           "   the metric\n");
    {
        // Air accel can turn a velocity by at most ws/|v| radians per tick.
        float cap2000 = WrTurnCeilingDeg(2000.0f, 0.015f);
        float cap3650 = WrTurnCeilingDeg(3650.0f, 0.015f);
        printf("     air accel alone can turn %.0f u/s at %.0f deg/s, "
               "%.0f u/s at %.0f deg/s\n", 2000.0f, cap2000, 3650.0f, cap3650);
        Check(cap2000 > 55.0f && cap2000 < 60.0f, "57 deg/s at 2000 u/s");
        Check(cap3650 < cap2000, "and less the faster you go");
        // A ramp turns velocity faster than this routinely -- measured in 10% of
        // the surf_demise world record's samples and 24% of a surf_666 run.
        Check(WrTurnRateDeg(2000.0f, 0.0f, 1900.0f, 600.0f, 0.015f) > cap2000,
              "a turn a real ramp produces exceeds it, so exceeding it is not "
              "in itself a mistake");
    }

    printf("\nthe gain accumulator reads zero when nothing is happening\n");
    {
        // The null test, and it is the one with teeth. A straight line at
        // constant height and constant speed has exactly constant energy, so
        // anything the accumulator reports is noise it failed to reject.
        const float speeds[2] = { 2000.0f, 3200.0f };
        const float rates[3] = { 60.0f, 200.0f, 500.0f };
        const float hs[3] = { 25.0f, 50.0f, WR_SWING_HYSTERESIS };
        const float NOISE = 2.0f;       // units of camera position wobble
        float worstSwing = 0.0f, worstNaive = 0.0f, bestNaive = 1e9f;
        float worstAt[3] = { 0.0f, 0.0f, 0.0f };

        printf("     %19s %10s %10s %10s %11s\n", "", "h=25", "h=50",
               "h=shipped", "rectifier");
        for (int s = 0; s < 2; s++)
        {
            for (int r = 0; r < 3; r++)
            {
                float dt = 1.0f / rates[r];
                Chain c;
                ChainInit(&c, 0.0f);
                WrSwing sw[3];
                for (int k = 0; k < 3; k++)
                    WrSwingReset(&sw[k], hs[k]);
                g_seed = 4242;

                float naive = 0.0f, prev = 0.0f;
                bool havePrev = false;
                for (float t = 0.0f; t < 60.0f; t += dt)
                {
                    float x = speeds[s] * t;
                    float e = ChainStep(&c, x + NOISE * Noise(),
                                        NOISE * Noise(), 1000.0f + NOISE * Noise(),
                                        dt, 0);
                    if (e > 1e8f || t < 1.0f)
                        continue;
                    for (int k = 0; k < 3; k++)
                        WrSwingStep(&sw[k], e);
                    if (havePrev && e > prev)
                        naive += e - prev;      // the trap, for comparison
                    prev = e;
                    havePrev = true;
                }
                float g[3];
                for (int k = 0; k < 3; k++)
                {
                    g[k] = 0.0f;
                    WrSwingTotals(&sw[k], &g[k], NULL);
                    if (g[k] > worstAt[k]) worstAt[k] = g[k];
                }
                printf("     %5.0f u/s @ %3.0f fps %10.1f %10.1f %10.1f %11.1f\n",
                       speeds[s], rates[r], g[0], g[1], g[2], naive);
                if (g[2] > worstSwing) worstSwing = g[2];
                if (naive > worstNaive) worstNaive = naive;
                if (naive < bestNaive) bestNaive = naive;
            }
        }
        printf("     worst case: h=25 %.0f, h=50 %.0f, shipped %.0f, rectifier %.0f\n",
               worstAt[0], worstAt[1], worstAt[2], worstNaive);
        Check(worstSwing < 60.0f,
              "the swing accumulator stays near zero on a null trajectory");
        Check(worstNaive > 1000.0f,
              "while rectifying every sample invents thousands of units");
        // The point of this one: the obvious test does not catch the obvious
        // bug. A rectified EMA's noise floor is T*sigma/(tau*sqrt(2pi)), which
        // has no dt in it, so 60 fps and 500 fps agree about a number that is
        // entirely noise.
        Check(worstNaive < bestNaive * 3.0f,
              "and a frame-rate test would have called that noise stable");
    }

    printf("\nthe accumulator against a signal it is supposed to see\n");
    {
        // Eight swings of +500 then -250, driven straight in: 4000 up, 2000
        // down, net +2000. Both legs are comfortably past the threshold, so all
        // of it must be banked.
        WrSwing sw;
        WrSwingReset(&sw, WR_SWING_HYSTERESIS);
        float e = 0.0f;
        WrSwingStep(&sw, e);
        for (int i = 0; i < 8; i++)
        {
            for (int k = 0; k < 25; k++) { e += 20.0f; WrSwingStep(&sw, e); }
            for (int k = 0; k < 25; k++) { e -= 10.0f; WrSwingStep(&sw, e); }
        }
        float g = 0.0f, l = 0.0f;
        WrSwingTotals(&sw, &g, &l);
        printf("     gained %.0f  lost %.0f  net %.0f  (true +4000 / -2000 / +2000)\n",
               g, l, g - l);
        Check(fabsf(g - 4000.0f) < 60.0f, "banks the rises");
        Check(fabsf(l - 2000.0f) < 60.0f, "and the falls");
        Check(fabsf((g - l) - e) < 0.01f,
              "gained minus lost is the net change, exactly");
    }

    printf("\na teleport is seeded, not stepped\n");
    {
        // Run the identical trajectory twice: once seeding the discontinuity,
        // once stepping straight through it, so the difference is visible
        // rather than asserted.
        float lSeed = 0.0f, lStep = 0.0f;
        for (int mode = 0; mode < 2; mode++)
        {
            WrSwing sw;
            WrSwingReset(&sw, WR_SWING_HYSTERESIS);
            for (int i = 0; i < 50; i++)
                WrSwingStep(&sw, (float)i * 10.0f);     // climb to 490

            // A save-loc load 1100 units down the map.
            if (mode == 0) WrSwingSeed(&sw, -1100.0f);
            else           WrSwingStep(&sw, -1100.0f);
            for (int i = 1; i < 50; i++)
                WrSwingStep(&sw, -1100.0f + (float)i * 10.0f);

            float g = 0.0f, l = 0.0f;
            WrSwingTotals(&sw, &g, &l);
            (mode == 0 ? lSeed : lStep) = l;
        }
        printf("     lost: %.0f seeding the load, %.0f stepping through it\n",
               lSeed, lStep);
        Check(lSeed < 50.0f, "seeding banks nothing for the teleport itself");
        Check(lStep > 1000.0f,
              "where stepping through it would charge the player the whole drop");
    }

    // -----------------------------------------------------------------------
    // From here on the REAL sampler runs, not a copy of it.
    // -----------------------------------------------------------------------

    printf("\na fail trigger does not freeze the readout\n");
    {
        // Reported as "every time you hit a fail trigger it saves your value and
        // stops updating, and hitting the restart key doesn't reset it".
        //
        // It was not the filters. WrEnergySample recorded your last position at
        // the BOTTOM of the function, past an early return taken while the
        // velocity window refills -- so the frame after a teleport still held
        // the pre-teleport position, detected the same teleport again, and
        // emptied the window again. For the rest of the map.
        WrEnergyDefaults();
        WrEnergyReset();

        const float dt = 1.0f / 200.0f;
        const float padZ = 2000.0f;

        for (int i = 0; i < 100; i++)
            WrEnergySample(WrVec(0.0f, 0.0f, padZ), dt);
        // Anchored at the feet, as WrRenderWorld does from a run's first point.
        WrEnergyAnchorToFeet(WrVec(0.0f, 0.0f, padZ - 64.0f));

        // Down the map: 2000 u/s of drop against only 300 u/s across, so the
        // height lost is not bought back by speed and the figure goes properly
        // negative -- which is what makes a frozen value obvious.
        for (int i = 0; i < 400; i++)
        {
            float t = i * dt;
            WrEnergySample(WrVec(300.0f * t, 0.0f, padZ - 2000.0f * t), dt);
        }
        float atFail = WrEnergyRelative();

        // The trigger fires: back on the pad in a single frame.
        for (int i = 0; i < 400; i++)
            WrEnergySample(WrVec(0.0f, 0.0f, padZ), dt);
        float afterFail = WrEnergyRelative();

        printf("     %.0f at the moment of the fail, %.0f two seconds after the "
               "respawn\n", atFail, afterFail);
        Check(fabsf(atFail) > 500.0f,
              "the value at the fail really is far from zero");
        Check(fabsf(afterFail) < 20.0f,
              "and back on the pad it reads zero again, rather than sticking");
        Check(WrEnergyTakeRestart(), "the respawn was reported as a restart");
        Check(!WrEnergyTakeRestart(), "and reading that clears it");
    }

    printf("\na paused camera holds the readout instead of draining it\n");
    {
        // Reported while watching demos: "when you pause, the numbers start to
        // freak out and rise or fall". Feeding a stopped camera in reads as the
        // player having instantaneously stopped dead, so the entire kinetic
        // term drains out over the output filter's time constant.
        WrEnergyDefaults();
        WrEnergyReset();

        const float dt = 1.0f / 200.0f;
        const float z0 = 2000.0f, speed = 2400.0f;
        for (int i = 0; i < 100; i++)
            WrEnergySample(WrVec(0.0f, 0.0f, z0), dt);
        WrEnergyAnchorToFeet(WrVec(0.0f, 0.0f, z0 - 64.0f));

        // Fly flat and fast until the reading is settled. `frozen` must be the
        // LAST position actually fed, not the next one along, or the pause does
        // not begin until a frame later.
        float t = 0.0f;
        Vec3 frozen = WrVec(0.0f, 0.0f, z0);
        for (int i = 0; i < 400; i++, t += dt)
        {
            frozen = WrVec(speed * t, 0.0f, z0);
            WrEnergySample(frozen, dt);
        }

        float before = WrEnergyRelative();
        float gBefore = WrEnergyGained(), lBefore = WrEnergyLost();

        // Pause: the identical position, for three seconds of frames.
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < 600; i++)
        {
            WrEnergySample(frozen, dt);
            float r = WrEnergyRelative();
            if (r < lo) lo = r;
            if (r > hi) hi = r;
        }
        printf("     before %.0f, over a 3 s pause %.0f..%.0f, held=%s\n",
               before, lo, hi, WrEnergyHeld() ? "yes" : "no");
        Check(WrEnergyHeld(), "the hold engages");
        Check(fabsf(hi - before) < 1.0f && fabsf(lo - before) < 1.0f,
              "and the figure does not move at all across the pause");
        Check(WrEnergyGained() == gBefore && WrEnergyLost() == lBefore,
              "the accumulators bank nothing for time nobody played");

        // Resume from where it stopped, at the same speed.
        float worstStep = 0.0f;
        float prev = before;
        for (int i = 0; i < 200; i++, t += dt)
        {
            WrEnergySample(WrVec(speed * t, 0.0f, z0), dt);
            float r = WrEnergyRelative();
            float step = fabsf(r - prev);
            if (step > worstStep) worstStep = step;
            prev = r;
        }
        printf("     worst single-frame step on resume %.1f, settled at %.0f\n",
               worstStep, prev);
        Check(!WrEnergyHeld(), "and releases as soon as the camera moves");
        // The ring's own clock does not advance during the hold, so the first
        // difference after it spans real positions over a real interval with
        // the pause simply excised. If that were not true this would step by
        // the whole kinetic term.
        Check(worstStep < 20.0f, "with no step, because the pause is excised");
        Check(fabsf(prev - before) < 20.0f, "and the same value it paused at");
    }

    printf("\na moving camera is never held, however slowly it moves\n");
    {
        WrEnergyDefaults();
        WrEnergyReset();
        const float dt = 1.0f / 200.0f;
        g_seed = 5150;
        bool everHeld = false;
        for (int i = 0; i < 1200; i++)
        {
            // Barely moving, but moving: sub-unit drift plus bob, which is what
            // a live camera does when the player is standing still.
            float t = i * dt;
            WrEnergySample(WrVec(0.4f * t, 0.0f,
                                 1000.0f + 1.2f * sinf(t * 12.0f * 6.28318f) +
                                 0.3f * Noise()), dt);
            if (WrEnergyHeld()) everHeld = true;
        }
        Check(!everHeld, "bit-identical is the test, so a live camera never holds");
    }

    printf("\na save-loc load into motion does not spike\n");
    {
        // The reported defect: "loading a saved location shows some crazy value
        // and takes 0.25-0.5s to drop to the correct one".
        //
        // The existing teleport test lands the player STANDING STILL, which is
        // why this survived it. The real case lands you mid-flight at speed, and
        // the frame after the window is emptied the estimator used to report a
        // velocity differenced over a single frame -- 5 ms against a 40 ms
        // window -- straight into filters that had just been reset, so it was
        // returned unfiltered.
        WrEnergyDefaults();
        WrEnergyReset();

        const float dt = 1.0f / 200.0f;
        const float padZ = 4000.0f;
        for (int i = 0; i < 100; i++)
            WrEnergySample(WrVec(0.0f, 0.0f, padZ), dt);
        WrEnergyAnchorToFeet(WrVec(0.0f, 0.0f, padZ - 64.0f));
        for (int i = 0; i < 200; i++)
            WrEnergySample(WrVec(0.0f, 0.0f, padZ), dt);

        // The load: 9000 units across the map, 2500 down, arriving at 2200 u/s.
        // Run it twice -- a clean arrival, and one where the view is still
        // settling for the first few frames, which is what decides how long the
        // filter takes to converge.
        const float baseX = 9000.0f, locZ = padZ - 2500.0f, speed = 2200.0f;
        float truth = (locZ - padZ) + (speed * speed) / (2.0f * 800.0f);
        const float jitters[2] = { 0.0f, 8.0f };

        for (int j = 0; j < 2; j++)
        {
            WrEnergyReset();
            for (int i = 0; i < 100; i++)
                WrEnergySample(WrVec(0.0f, 0.0f, padZ), dt);
            WrEnergyAnchorToFeet(WrVec(0.0f, 0.0f, padZ - 64.0f));
            for (int i = 0; i < 200; i++)
                WrEnergySample(WrVec(0.0f, 0.0f, padZ), dt);
            g_seed = 31337;

            float worst = -1e9f, settle = -1.0f;
            for (int i = 0; i < 500; i++)
            {
                float t = i * dt;
                float jit = (i < 3) ? jitters[j] * Noise() : 0.0f;
                WrEnergySample(WrVec(baseX + speed * t + jit, 0.0f,
                                     locZ + jit * 0.5f), dt);
                float rel = WrEnergyRelative();
                if (rel > worst) worst = rel;
                if (settle < 0.0f && fabsf(rel - truth) < 60.0f) settle = t;
            }
            float g = WrEnergyGained(), l = WrEnergyLost();
            printf("     %s arrival: true %.0f, worst shown %.0f, settled %.0f ms, "
                   "gained %.0f lost %.0f\n", j ? "jittery" : "clean  ",
                   truth, worst, settle * 1000.0f, g, l);

            // The defect was an overshoot of thousands. Nothing about a save-loc
            // load should ever read as MORE energy than the player actually has.
            Check(worst < truth + 200.0f, j
                  ? "a jittery arrival still never overshoots"
                  : "a clean arrival never overshoots");
            // A clean arrival is right as soon as there is a window to measure
            // over -- one window, 40 ms, and no filter lag at all, because the
            // filter seeds from a value that is already correct.
            //
            // A jittery one cannot be: its first window genuinely does measure a
            // wrong velocity, so the figure converges at the output filter's own
            // time constant. Three taus is 0.9 s and that is the filter working,
            // not failing. Real save-loc loads set the position exactly; the 8
            // units of wobble here is an invented worst case.
            Check(settle >= 0.0f &&
                  settle < (j ? SETTLE_TAUS * 0.30f + 0.1f : 0.10f), j
                  ? "and converges at the filter's time constant, no slower"
                  : "and is right within one window, with no filter lag");
            Check(g < 100.0f && l < 100.0f, j
                  ? "and banks nothing, even settling from a bad first window"
                  : "and banks nothing as gain or loss");
        }
    }

    printf("\nthe three budget numbers add up, on every frame\n");
    {
        // If this ever fails, the readout is lying: spent - banked IS the
        // negated headline figure, by construction rather than by coincidence.
        WrEnergyDefaults();
        WrEnergyReset();

        const float dt = 1.0f / 200.0f;
        const float padZ = 3000.0f;
        for (int i = 0; i < 100; i++)
            WrEnergySample(WrVec(0.0f, 0.0f, padZ), dt);
        WrEnergyAnchorToFeet(WrVec(0.0f, 0.0f, padZ - 64.0f));

        float worst = 0.0f, lowCarried = 1e9f, highCarried = -1e9f;
        for (int i = 0; i < 500; i++)
        {
            float t = i * dt;
            // A clean vertical drop off the pad, from rest: height traded for
            // speed and nothing else, so it must spend and bank the same amount.
            float z = padZ - 0.5f * 800.0f * t * t;
            WrEnergySample(WrVec(0.0f, 0.0f, z), dt);
            if (t < 0.9f)
                continue;
            WrEnergyBudget b;
            if (!WrEnergyBudgetNow(&b))
                continue;
            float err = fabsf((b.spent - b.banked) + WrEnergyRelative());
            if (err > worst) worst = err;
            if (b.carriedValid)
            {
                if (b.carried < lowCarried) lowCarried = b.carried;
                if (b.carried > highCarried) highCarried = b.carried;
            }
        }
        printf("     worst disagreement %.4f units; carried ran %.0f%%..%.0f%% "
               "through a clean drop from rest\n", worst, lowCarried, highCarried);
        Check(worst < 1.0f, "spent - banked is the negated net figure, exactly");
        Check(lowCarried > 95.0f && highCarried < 105.0f,
              "and a clean fall keeps everything it spends");
    }

    printf("\nstarting with speed reads over 100%%, and that is not a bug\n");
    {
        // The same drop, entered carrying 600 u/s. That 225 units of energy was
        // never taken from the anchor's height, so the ratio is legitimately
        // above 1 -- the same reason the fastest surf_utopia run finishes at
        // 293%. Anything that clamped it would be hiding real energy.
        WrEnergyDefaults();
        WrEnergyReset();
        const float dt = 1.0f / 200.0f;
        const float padZ = 3000.0f;
        for (int i = 0; i < 100; i++)
            WrEnergySample(WrVec(600.0f * i * dt, 0.0f, padZ), dt);
        WrEnergyAnchorToFeet(WrVec(600.0f * 100 * dt, 0.0f, padZ - 64.0f));

        float base = 600.0f * 100 * dt;
        WrEnergyBudget b;
        b.carried = 0.0f;
        for (int i = 0; i < 500; i++)
        {
            float t = i * dt;
            WrEnergySample(WrVec(base + 600.0f * t, 0.0f,
                                 padZ - 0.5f * 800.0f * t * t), dt);
            WrEnergyBudgetNow(&b);
        }
        printf("     carried %.0f%% after spending %.0f units of height\n",
               b.carried, b.spent);
        Check(b.carriedValid && b.carried > 105.0f,
              "over 100% when you brought speed with you");
    }

    printf("\nclimbing above the anchor is not clamped\n");
    {
        WrEnergyDefaults();
        WrEnergyReset();
        const float dt = 1.0f / 200.0f;
        for (int i = 0; i < 100; i++)
            WrEnergySample(WrVec(0.0f, 0.0f, 1000.0f), dt);
        WrEnergyAnchorToFeet(WrVec(0.0f, 0.0f, 1000.0f - 64.0f));

        // Straight up 800 units. Measured median backtrack of the height spent,
        // from its running maximum, is 1465 units on surf_demise and 31160 on
        // surf_vacant, so this is not an edge case.
        for (int i = 0; i < 400; i++)
            WrEnergySample(WrVec(0.0f, 0.0f, 1000.0f + (float)i * 2.0f), dt);
        WrEnergyBudget b;
        bool ok = WrEnergyBudgetNow(&b);
        printf("     spent %.0f, banked %.0f, wasted %.0f\n",
               b.spent, b.banked, b.wasted);
        Check(ok && b.spent < -100.0f, "spent goes negative above the anchor");
        Check(!b.carriedValid, "and the ratio is withheld rather than inverted");
    }

    printf("\na teleport away from the anchor is a save-loc, not a restart\n");
    {
        WrEnergyDefaults();
        WrEnergyReset();

        const float dt = 1.0f / 200.0f;
        const float padZ = 2000.0f;
        const float locZ = 900.0f;

        for (int i = 0; i < 100; i++)
            WrEnergySample(WrVec(0.0f, 0.0f, padZ), dt);
        WrEnergyAnchorToFeet(WrVec(0.0f, 0.0f, padZ - 64.0f));

        // Loading a save-loc 8000 units across the map and 1100 units down.
        for (int i = 0; i < 400; i++)
            WrEnergySample(WrVec(8000.0f, 0.0f, locZ), dt);

        float rel = WrEnergyRelative();
        printf("     %.0f, against %.0f of height lost\n", rel, locZ - padZ);
        Check(!WrEnergyTakeRestart(),
              "no restart, so a save-loc load keeps its clock");
        Check(fabsf(rel - (locZ - padZ)) < 20.0f,
              "and the readout follows to where you now are");
    }

    // -----------------------------------------------------------------------
    //
    // The live recorder used to be handed THIS frame's feet and the SMOOTHED
    // velocity readout. Those describe two instants about 80 ms apart -- the
    // window velocity refers to its own midpoint, and the EMA trails that
    // again -- so every energy computed from a live point was wrong by whatever
    // the trajectory did in between. Energy is quadratic in speed, so on a ramp
    // that is worth hundreds of units.
    //
    // Free flight is the test that catches it, because there the answer is
    // known exactly: E is conserved. A mismatched pair cannot hold it.
    printf("\nthe pair handed to the live recorder is of one instant\n");
    {
        WrEnergyDefaults();
        g_energy.gravity = G;
        WrEnergyReset();

        // Measured as DRIFT, not as spread. A single raw pair is noisy -- it is
        // a 40 ms difference, which is why the readout is filtered before it is
        // shown -- and no pairing fixes that. What a mismatched pair adds is
        // BIAS that moves with the trajectory, so the test is whether the mean
        // energy early in the arc matches the mean late in it. In free flight
        // those must be equal.
        const float dt = 1.0f / 200.0f;
        const float z0 = 3000.0f;
        double earlySum = 0.0, lateSum = 0.0;
        int earlyN = 0, lateN = 0;
        double oldEarly = 0.0, oldLate = 0.0;
        int oldEarlyN = 0, oldLateN = 0;
        int samples = 0;

        for (int i = 0; i < 400; i++)
        {
            float t = i * dt;
            float x = 1600.0f * t;
            float z = z0 + 400.0f * t - 0.5f * G * t * t;
            float bob = 2.0f * sinf(t * 15.0f * 6.28318f) + 0.35f * Noise();
            Vec3 cam = WrVec(x, 0.0f, z + bob);
            WrEnergySample(cam, dt);

            Vec3 p, v;
            if (!WrEnergySampleAt(&p, &v))
                continue;
            if (t < 0.25f)          // let the window and the filters fill
                continue;
            samples++;

            float e = WrEnergyOf(p, v);

            // What the recorder used to be handed: where the camera is NOW,
            // and the smoothed velocity readout.
            Vec3 oldFeet = cam;
            oldFeet.z -= g_energy.eyeHeight;
            Vec3 oldVel;
            float eOld = WrEnergyVelocity(&oldVel)
                       ? WrEnergyOf(oldFeet, oldVel) : 0.0f;

            if (t < 0.9f)       { earlySum += e; earlyN++;
                                  oldEarly += eOld; oldEarlyN++; }
            else if (t > 1.6f)  { lateSum += e;  lateN++;
                                  oldLate += eOld;  oldLateN++; }
        }

        float drift = (float)fabs(lateSum / lateN - earlySum / earlyN);
        float driftOld = (float)fabs(oldLate / oldLateN - oldEarly / oldEarlyN);
        printf("     %d pairs; drift across the arc %.1f units, was %.1f\n",
               samples, drift, driftOld);
        Check(samples > 200, "the recorder is offered a pair on nearly every frame");
        Check(drift < 20.0f,
              "free flight holds its energy, which a mismatched pair cannot");
        Check(drift < driftOld * 0.5f,
              "and the old pairing drifted at least twice as far");

        // And the feet are the feet: a run stores the player origin, so a live
        // point 64 units high would sit above a demo line of the same path.
        Vec3 p, v;
        if (WrEnergySampleAt(&p, &v))
            Check(p.z < z0 + 400.0f && p.z > 0.0f, "the position is a real height");
    }

    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
