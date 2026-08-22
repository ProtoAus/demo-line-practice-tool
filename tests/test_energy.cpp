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
static Vec3 g_testForward = { 1.0f, 0.0f, 0.0f };

bool WrCameraForward(Vec3 *out)
{
    if (out) *out = g_testForward;
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
    printf("\nthe practice readout defaults are immediate and actionable\n");
    {
        WrEnergyDefaults();
        Check(g_energy.showHud, "the crosshair readout starts on");
        Check(g_energy.hudMode == WR_HUD_TURN,
              "and starts on turn rate versus ideal");
        Check(!g_energy.compareToRun,
              "nearby-run comparison starts off");
        Check(!g_energy.showBar, "the comparison bar starts off");
        Check(!g_energy.showReferenceStrafeBar,
              "the nearby run's strafe bar starts off");
        Check(fabsf(g_energy.strafeBarSensitivity - 1.0f) < 1e-6f,
              "the strafe bar starts at literal 1x sensitivity");
        Check(fabsf(g_energy.velWindowSeconds - 0.020f) < 1e-6f &&
              fabsf(g_energy.velTau - 0.030f) < 1e-6f &&
              fabsf(g_energy.speedTau - 0.050f) < 1e-6f &&
              fabsf(g_energy.smoothSeconds - 0.12f) < 1e-6f,
              "the camera fallback starts on the Snappy preset");
    }

    printf("\na proved player pair needs no velocity smoothing\n");
    {
        WrEnergyDefaults();
        WrEnergyReset();
        const Vec3 origin = WrVec(100.0f, 200.0f, 300.0f);
        const Vec3 velocity = WrVec(1200.0f, -400.0f, 250.0f);
        const Vec3 camera = WrVec(origin.x, origin.y,
                                  origin.z + g_energy.eyeHeight);
        WrEnergySetTruePlayer(&origin, &velocity, 64.0f);
        WrEnergySample(camera, 1.0f / 200.0f);

        Vec3 feet, sampledVelocity;
        const float expectedEnergy = WrEnergyOf(camera, velocity);
        Check(WrEnergyValid(), "one exact sample is immediately valid");
        Check(fabsf(WrEnergySpeed() - WrLength(velocity)) < 1e-4f,
              "speed is the game's value on that same frame");
        Check(fabsf(WrEnergyNow() - expectedEnergy) < 1e-4f,
              "energy is built from the exact position/velocity pair");
        WrEnergyAnchorToFeet(origin);
        const float expectedRelative = expectedEnergy - camera.z;
        Check(fabsf(WrEnergyRelative() - expectedRelative) < 1e-4f,
              "the displayed energy is neither smoothed nor bucketed");
        Check(WrEnergySampleAt(&feet, &sampledVelocity) &&
              WrDist(feet, origin) < 1e-4f &&
              WrDist(sampledVelocity, velocity) < 1e-4f,
              "the live recorder receives that exact pair too");

        // The default crosshair mode is turn rate versus ideal. Its view angle
        // still has to be differenced, but on the exact-player path the result
        // must not ease through the old 120 ms display EMA.
        const float oneDegree = 0.01745329252f;
        g_testForward = WrVec(cosf(oneDegree), sinf(oneDegree), 0.0f);
        WrEnergySetTruePlayer(&origin, &velocity, 64.0f);
        WrEnergySample(camera, 0.010f);
        Check(fabsf(WrEnergyYawRate() - 100.0f) < 0.1f,
              "turn rate also updates without the display EMA");
        g_testForward = WrVec(1.0f, 0.0f, 0.0f);
    }

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

        const float accel150 = WrAirAccelerationPerTickEx(
            0.015f, 150.0f, 250.0f, 1.0f);
        const float deadAccel150 = WrAirAccelerationPerTickEx(
            0.015f, 150.0f, 250.0f, 0.25f);
        Check(fabsf(accel150 - 562.5f) < 1e-5f &&
              fabsf(deadAccel150 - 140.625f) < 1e-5f,
              "the live stress display exposes 562.5 -> 140.6 per tick");
        Check(fabsf(deadAccel150 / WR_AIR_WISHSPEED - 4.6875f) < 1e-5f,
              "and reports 4.69x headroom above the 30-unit cap");

        const float duckWish = WrAirWishSpeed(250.0f, true);
        Check(fabsf(duckWish - 85.0f) < 1e-5f,
              "Momentum crouch scales the uncapped wishspeed to 34 percent");
        Check(WrAirSurfaceFriction(-1.0f) == 1.0f &&
              WrAirSurfaceFriction(0.0f) == 1.0f,
              "falling and the apex keep normal air friction");
        Check(WrAirSurfaceFriction(0.01f) == 0.25f &&
              WrAirSurfaceFriction(140.0f) == 0.25f &&
              WrAirSurfaceFriction(140.01f) == 1.0f,
              "deadstrafe is exactly the 0 < vz <= 140 window");

        const float normalIdeal = WrPerfectStrafeDegreesEx(
            2000.0f, 0.015f, 150.0f, 250.0f, 1.0f);
        const float crouchDeadIdeal = WrPerfectStrafeDegreesEx(
            2000.0f, 0.015f, 150.0f, duckWish, 0.25f);
        Check(fabsf(normalIdeal - crouchDeadIdeal) < 1e-6f,
              "at surf settings crouch plus deadstrafe still reaches the cap");
        Check(WrPerfectStrafeDegreesEx(2000.0f, 1.0f / 64.0f, 12.0f,
                                       duckWish, 0.25f) < normalIdeal,
              "at low airaccelerate the same state lowers the true ideal turn");

        WrEnergyDefaults();
        WrEnergyReset();
        const Vec3 org = WrVec(0.0f, 0.0f, 0.0f);
        const Vec3 vel = WrVec(1000.0f, 0.0f, 100.0f);
        WrEnergySetTruePlayer(&org, &vel, 28.0f);
        Check(WrEnergyCrouched(),
              "the measured 28-unit view offset identifies a crouched surfer");
        WrEnergySetTruePlayer(&org, &vel, 64.0f);
        Check(!WrEnergyCrouched(),
              "the measured 64-unit view offset identifies standing");
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

    printf("\na stationary camera no longer pauses exact numbers\n");
    {
        WrEnergyDefaults();
        WrEnergyReset();

        const float dt = 1.0f / 200.0f;
        const Vec3 origin = WrVec(200.0f, 300.0f, 1900.0f);
        const Vec3 camera = WrVec(origin.x, origin.y,
                                  origin.z + g_energy.eyeHeight);
        Vec3 velocity = WrVec(2400.0f, 0.0f, 0.0f);
        WrEnergySetTruePlayer(&origin, &velocity, 64.0f);
        WrEnergySample(camera, dt);
        WrEnergyAnchorToFeet(origin);

        // Repeat the same origin long enough to trip the stale-matrix hint. The
        // hint must no longer return from the sampler or freeze its outputs.
        for (int i = 0; i < 20; i++)
        {
            WrEnergySetTruePlayer(&origin, &velocity, 64.0f);
            WrEnergySample(camera, dt);
        }
        Check(WrEnergyCameraStill(),
              "repeated position remains available only as a matrix hint");

        velocity = WrVec(1801.0f, 0.0f, 0.0f);
        WrEnergySetTruePlayer(&origin, &velocity, 64.0f);
        WrEnergySample(camera, dt);
        const float expected = WrEnergyOf(camera, velocity) - camera.z;
        Check(fabsf(WrEnergyRelative() - expected) < 1e-3f,
              "a changed exact value appears immediately at the same position");
    }

    printf("\na moving camera is never described as still\n");
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
            if (WrEnergyCameraStill()) everHeld = true;
        }
        Check(!everHeld, "bit-identical is the test, so a live camera stays live");
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

    printf("\nthe strafe quality curve survives a literal AirAccelerate\n");
    {
        // CGameMovement::AirAccelerate, written out from its description with
        // nothing simplified -- in particular wishspd is capped at 30 and used
        // ONLY for addspeed, while accelspeed is computed from the UNCAPPED
        // wishspeed. Getting those two the wrong way round is the single most
        // common error in reimplementations of this function, so the test copy
        // has to keep them apart on its own rather than call ours.
        //
        // The names are the surf community's terms of art and are kept so this
        // can be checked line by line against any description of the function.
        // It is a reimplementation of the arithmetic, not a copy of anybody's
        // source -- the same standard wr_stress.h applies to strafe-analyzer.
        struct Sim
        {
            static float Accel(float vel[3], const float wishdir[3],
                               float wishspeed, float accel, float tick,
                               float friction)
            {
                float wishspd = wishspeed;
                if (wishspd > 30.0f)                 // GetAirSpeedCap()
                    wishspd = 30.0f;
                float currentspeed = vel[0] * wishdir[0] + vel[1] * wishdir[1] +
                                     vel[2] * wishdir[2];
                float addspeed = wishspd - currentspeed;
                if (addspeed <= 0.0f)
                    return 0.0f;
                float accelspeed = accel * wishspeed * tick * friction;
                if (accelspeed > addspeed)
                    accelspeed = addspeed;
                for (int i = 0; i < 3; i++)
                    vel[i] += accelspeed * wishdir[i];
                return accelspeed;
            }
        };

        const float tick = 0.015f, accel = 150.0f, maxSpeed = 250.0f;
        const float V = 1000.0f;
        const float idealPerSec =
            WrPerfectStrafeDegrees(V, tick, accel, maxSpeed) / tick;

        Check(WrAirCapBinds(tick, accel, maxSpeed, 1.0f),
              "at surf settings the wishspeed cap is what binds");

        float worstGain = 0.0f, worstQual = 0.0f;
        for (int k = 0; k <= 30; k++)
        {
            const float c = (float)k;               // dot(velocity, wishdir)
            const float sinT = c / V;
            const float cosT = sqrtf(1.0f - sinT * sinT);
            const float wishdir[3] = { sinT, cosT, 0.0f };
            float vel[3] = { V, 0.0f, 0.0f };

            Sim::Accel(vel, wishdir, maxSpeed, accel, tick, 1.0f);

            // ONE: the gain collapses to 900 - c^2.
            const float d2 = vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]
                           - V * V;
            const float eGain = fabsf(d2 - (900.0f - c * c));
            if (eGain > worstGain) worstGain = eGain;

            // THREE: the turn the simulation actually produced, put through the
            // shipped quality function, against the gain fraction it actually
            // got. Nothing here tells WrStrafeQuality what c was.
            const float turned = atan2f(vel[1], vel[0]) * 57.2957795131f / tick;
            const float q = WrStrafeQuality(turned, idealPerSec);
            const float eQual = fabsf(q - d2 / 900.0f);
            if (eQual > worstQual) worstQual = eQual;
        }

        printf("     worst gain error %.4f units^2, worst quality error %.5f\n",
               worstGain, worstQual);
        Check(worstGain < 0.5f,
              "the per-tick gain really is 900 - c^2 when the cap binds");
        Check(worstQual < 0.01f,
              "and 1 - (1-r)^2 recovers the gain fraction from the turn alone");

        // The far side: turning twice as fast is exactly as bad as not turning.
        Check(fabsf(WrStrafeQuality(0.0f, idealPerSec)) < 1e-6f,
              "not turning at all scores zero");
        Check(fabsf(WrStrafeQuality(2.0f * idealPerSec, idealPerSec)) < 1e-6f,
              "and so does turning twice as fast");
        Check(WrStrafeQuality(idealPerSec, idealPerSec) > 0.999f,
              "the ideal rate scores one");
        float ratio = 0.0f;
        Check(WrStrafeRatioFromAirDelta(0.0f, 2000.0f, 30.0f, 0.0f,
                                        &ratio) && fabsf(ratio - 1.0f) < 1e-6f,
              "a perpendicular demo acceleration reconstructs the ideal bar");
        Check(WrStrafeRatioFromAirDelta(15.0f, 2000.0f, 15.0f, 0.0f,
                                        &ratio) && fabsf(ratio - 0.5f) < 1e-6f,
              "acceleration ahead of velocity reconstructs under-turning");
        Check(WrStrafeRatioFromAirDelta(-15.0f, 2000.0f, 15.0f, 0.0f,
                                        &ratio) && fabsf(ratio - 1.5f) < 1e-6f,
              "acceleration behind it reconstructs over-turning");
        Check(WrStrafeQuality(0.8f * idealPerSec, idealPerSec) > 0.95f,
              "and the old +-20% band is still the top few percent");

        // The regime with no answer, rather than a bad answer.
        Check(!WrAirCapBinds(1.0f / 64.0f, 12.0f, 250.0f, 0.25f),
              "CS:GO KZ in a deadstrafe period is the other regime");
    }

    printf("\nthe generalised quality curve is the old one where it should be\n");
    {
        // WrStrafeQuality is now WrStrafeQualityEx with cScale = 30, and the
        // whole point of normalising before squaring rather than after is that
        // this is exact rather than close. Bit-for-bit, across the range.
        int same = 0, total = 0;
        for (int i = 0; i <= 400; i++)
        {
            const float r = (float)i * 0.005f;      // 0 .. 2
            const float ideal = 137.0f;
            const float got = r * ideal;
            const float a = WrStrafeQuality(got, ideal);
            const float b = WrStrafeQualityEx(got, ideal, WR_AIR_WISHSPEED);
            total++;
            if (memcmp(&a, &b, sizeof(float)) == 0)
                same++;
        }
        printf("     %d of %d identical to the bit\n", same, total);
        Check(same == total,
              "cScale = 30 reproduces the flat formula exactly, not nearly");

        // And a bigger scale is strictly harsher, which is the claim the ramp
        // case rests on.
        const float q30 = WrStrafeQualityEx(0.7f * 100.0f, 100.0f, 30.0f);
        const float q46 = WrStrafeQualityEx(0.7f * 100.0f, 100.0f, 46.0f);
        Check(q46 < q30, "a plane that turns you for you grades a slip harder");
    }

    printf("\nthe ramp ideal is Source's own clip, not the flat one\n");
    {
        // ONE TICK OF SURF, written out: AirAccelerate, then gravity, then
        // PM_ClipVelocity against the ramp -- in that order, which is the order
        // TryPlayerMove runs them. Then the horizontal rotation that produced,
        // against WrPerfectStrafeDegreesOnPlane's closed form.
        //
        // The velocity starts IN the plane, because that is what riding a ramp
        // means; a surfer's v.n is zero to within the tick.
        const float tick = 0.015f;
        const float gravity = 800.0f;
        const float nrm[3] = { 0.8f, 0.0f, 0.6f };      // nz 0.6, a ~53 deg ramp
        const float speed = 1000.0f;

        float worst = 0.0f;
        for (int side = 0; side < 2; side++)
        {
            const float s = side ? -1.0f : 1.0f;

            // v along +y, which is perpendicular to the normal's horizontal
            // part, so v.n = 0 and wn = +-0.8: riding across the fall line.
            float vel[3] = { 0.0f, speed, 0.0f };
            const float wsh[3] = { s, 0.0f, 0.0f };

            const float gain = WrAirGainPerTick(tick, WR_AIR_ACCEL_DEFAULT,
                                                WR_MAXSPEED_DEFAULT);
            float v[3] = { vel[0] + gain * wsh[0],
                           vel[1] + gain * wsh[1],
                           vel[2] + gain * wsh[2] };
            v[2] -= gravity * tick;

            // PM_ClipVelocity, overbounce 1: an orthogonal projection.
            const float d = v[0] * nrm[0] + v[1] * nrm[1] + v[2] * nrm[2];
            v[0] -= d * nrm[0];
            v[1] -= d * nrm[1];
            v[2] -= d * nrm[2];

            const float simulated = fabsf(atan2f(v[0], v[1]));   // rad this tick
            const float wn = wsh[0] * nrm[0] + wsh[1] * nrm[1] + wsh[2] * nrm[2];
            const float closed =
                WrPerfectStrafeDegreesOnPlane(speed, tick, WR_AIR_ACCEL_DEFAULT,
                                              WR_MAXSPEED_DEFAULT, gravity,
                                              nrm, wn) / 57.2957795131f;

            const float err = fabsf(simulated - closed);
            if (err > worst) worst = err;
            printf("     wn %+.1f  simulated %.9f  closed form %.9f  (%.1f%% of air)\n",
                   wn, simulated, closed, 100.0f * closed / (gain / speed));
        }

        printf("     worst disagreement %.2e rad\n", worst);
        Check(worst < 1e-5f,
              "the closed form reproduces a literal clipped tick");

        // The flat function is this one on a plane you are not touching.
        const float flat = WrPerfectStrafeDegrees(speed, tick,
                                                  WR_AIR_ACCEL_DEFAULT,
                                                  WR_MAXSPEED_DEFAULT);
        const float viaPlane =
            WrPerfectStrafeDegreesOnPlane(speed, tick, WR_AIR_ACCEL_DEFAULT,
                                          WR_MAXSPEED_DEFAULT, gravity, 0, 0.0f);
        Check(fabsf(flat - viaPlane) < 1e-6f,
              "and with no plane at all it IS the flat function");

        // The size of the thing. If these ever came out close to each other the
        // ramp case would not have been worth the trouble.
        const float nUp[3] = { 0.8f, 0.0f, 0.6f };
        const float into = WrPerfectStrafeDegreesOnPlane(speed, tick,
                                                         WR_AIR_ACCEL_DEFAULT,
                                                         WR_MAXSPEED_DEFAULT,
                                                         gravity, nUp, -0.8f);
        const float away = WrPerfectStrafeDegreesOnPlane(speed, tick,
                                                         WR_AIR_ACCEL_DEFAULT,
                                                         WR_MAXSPEED_DEFAULT,
                                                         gravity, nUp, 0.8f);
        Check(into < flat * 0.35f && away < flat * 0.75f,
              "a ramp's ideal is a fraction of the flat one, both ways round");
        Check(into < away * 0.5f,
              "and which way you strafe changes it by more than a factor of two");
    }

    printf("\na board can be quoted in any unit and stay the same board\n");
    {
        // Arriving at 1200 u/s into the same 53-degree ramp, 20 degrees off
        // parallel, so there is a real board to take apart.
        const float nrm[3] = { 0.8f, 0.0f, 0.6f };
        const float sinA = 0.34202014f;                 // sin(20 deg)
        const float cosA = 0.93969262f;
        const float s0 = 1200.0f;

        // v = s0 * (parallel * cos + (-n) * sin): 20 degrees INTO the plane.
        const float par[3] = { 0.0f, 1.0f, 0.0f };      // in-plane, v.n = 0
        float vIn[3];
        for (int k = 0; k < 3; k++)
            vIn[k] = s0 * (par[k] * cosA - nrm[k] * sinA);

        float vOut[3] = { vIn[0], vIn[1], vIn[2] };

        WrBoardStats b;
        Check(WrPhaseBoard(vIn, vOut, nrm, &b), "the board is measured");

        printf("     %.1f u/s in, %.0f deg approach, %.1f%% of perfect\n",
               b.speedIn, b.approachDeg, WrBoardPerfectPct(&b));

        Check(fabsf(b.approachDeg - 70.0f) < 0.05f,
              "20 degrees into the plane is 70 off the normal");
        Check(fabsf(WrBoardPerfectPct(&b) - 100.0f * cosA) < 0.05f,
              "and it keeps cos(20 deg) of its speed");

        // The normal is stored pointing back at whoever arrived, whichever way
        // the file had it. Both signs in, one sign out.
        float flipped[3] = { -nrm[0], -nrm[1], -nrm[2] };
        WrBoardStats b2;
        Check(WrPhaseBoard(vIn, vOut, flipped, &b2), "and again with n flipped");
        Check(fabsf(b.normal[0] - b2.normal[0]) < 1e-6f &&
              fabsf(b.normal[2] - b2.normal[2]) < 1e-6f,
              "the stored normal is oriented, so the axes cannot come out mirrored");
        Check(fabsf(b.loss - b2.loss) < 1e-3f, "and the grade is unchanged by it");

        // Every unit is the same clip read differently, so they have to agree
        // with the vector the clip actually produced.
        float clipped[3];
        const float d = vIn[0] * nrm[0] + vIn[1] * nrm[1] + vIn[2] * nrm[2];
        for (int k = 0; k < 3; k++)
            clipped[k] = vIn[k] - d * nrm[k];

        float delta[3];
        WrBoardDeltaAxes(&b, delta);
        for (int k = 0; k < 3; k++)
            Check(fabsf((vIn[k] + delta[k]) - clipped[k]) < 1e-2f,
                  "the per-axis change reconstructs the clipped velocity");

        const float hIn = sqrtf(vIn[0] * vIn[0] + vIn[1] * vIn[1]);
        const float hOut = sqrtf(clipped[0] * clipped[0] +
                                 clipped[1] * clipped[1]);
        Check(fabsf(WrBoardLossHorizontal(&b) - (hIn - hOut)) < 1e-2f,
              "the horizontal figure is the horizontal speed it really lost");

        const float e = (s0 * s0 - (clipped[0] * clipped[0] +
                                    clipped[1] * clipped[1] +
                                    clipped[2] * clipped[2])) / (2.0f * 800.0f);
        Check(fabsf(WrBoardLossEnergy(&b, 800.0f) - e) < 1e-2f,
              "and the energy figure is the kinetic term it really lost");

        // A board that costs nothing is 100%, exactly.
        float par2[3] = { 0.0f, s0, 0.0f };
        WrBoardStats perfect;
        Check(WrPhaseBoard(par2, par2, nrm, &perfect), "a parallel arrival");
        Check(fabsf(WrBoardPerfectPct(&perfect) - 100.0f) < 1e-3f,
              "costs nothing and says so");
        Check(WrBoardLossEnergy(&perfect, 800.0f) < 1e-3f,
              "in energy as well");
    }

    printf("\nyour own board is graded, and it does not read as free\n");
    {
        // The live board detector, driven end to end without a game: free fall
        // into a 53-degree ramp, with the map's answer pushed in the way
        // dllmain does it.
        //
        // The numeric assertions are on the INVARIANT rather than on a
        // hand-computed grade, because the moment of detection depends on how
        // the 0.10 s phase window fills and pinning that would make this a test
        // of the frame rate. What must hold is that the loss is the projection
        // of the speed onto the plane -- which is the whole fix, and the exact
        // thing that was reading zero.
        const float n[3] = { 0.8f, 0.0f, 0.6f };     // nz 0.6, a ramp not a floor
        const float dt = 1.0f / 200.0f;
        const float g = 800.0f;

        // Free fall, then an exact orthogonal clip and a slide down the plane.
        const Vec3 p0 = WrVec(0.0f, 0.0f, 4000.0f);
        const Vec3 v0 = WrVec(500.0f, 0.0f, -600.0f);
        const float tHit = 0.5f;

        // v at the moment of contact, and what the clip leaves of it.
        const Vec3 vHit = WrVec(v0.x, v0.y, v0.z - g * tHit);
        const float d = vHit.x * n[0] + vHit.y * n[1] + vHit.z * n[2];
        const Vec3 vC = WrVec(vHit.x - d * n[0], vHit.y - d * n[1],
                              vHit.z - d * n[2]);
        // Gravity with its into-plane part removed: the acceleration of a slide.
        const float gn = -g * n[2];
        const Vec3 aC = WrVec(-gn * n[0], -gn * n[1], -g - gn * n[2]);
        const Vec3 pHit = WrVec(p0.x + v0.x * tHit,
                                p0.y + v0.y * tHit,
                                p0.z + v0.z * tHit - 0.5f * g * tHit * tHit);

        // Run it twice: once with the map answering, once with it silent.
        for (int withMap = 1; withMap >= 0; withMap--)
        {
            WrEnergyDefaults();
            WrEnergyReset();

            bool sawEarly = false;
            for (int i = 0; i < 300; i++)
            {
                const float t = i * dt;
                Vec3 p;
                if (t < tHit)
                    p = WrVec(p0.x + v0.x * t, p0.y + v0.y * t,
                              p0.z + v0.z * t - 0.5f * g * t * t);
                else
                {
                    const float s = t - tHit;
                    p = WrVec(pHit.x + vC.x * s + 0.5f * aC.x * s * s,
                              pHit.y + vC.y * s + 0.5f * aC.y * s * s,
                              pHit.z + vC.z * s + 0.5f * aC.z * s * s);
                }

                WrEnergySample(p, dt);
                if (withMap)
                    WrEnergySetGeometryTouch(t < tHit ? WR_GEOM_NOTHING
                                                      : WR_GEOM_TOUCHING,
                                             t < tHit ? 0 : n);
                else
                    WrEnergySetGeometryTouch(WR_GEOM_UNKNOWN);
                WrEnergyTickBoards(dt);

                // Nothing may be reported while still in free flight.
                if (t < tHit - 0.05f && WrEnergyBoard(0, 0))
                    sawEarly = true;
            }

            WrBoardStats b;
            float age = 0.0f;
            const bool got = WrEnergyBoard(&b, &age);

            if (withMap)
            {
                Check(WrEnergyBoardAvailable(),
                      "with the map answering, live boards are offered");
                Check(!sawEarly, "and nothing was called a board during the fall");
                Check(got, "the landing was found and graded");
                if (got)
                {
                    const float sinA =
                        (float)sin(b.approachDeg / 57.2957795131);
                    const float pred = b.speedIn * (1.0f - sinA);
                    printf("     %s  -%.1f u/s at %.1f deg  (projection says "
                           "%.1f)\n", WrPhaseGradeName(b.grade), b.loss,
                           b.approachDeg, pred);
                    Check(b.loss > 5.0f,
                          "it cost something, which is the defect that started "
                          "this");
                    Check(fabsf(b.loss - pred) < 0.5f,
                          "and the cost is the projection onto the plane");
                    Check(b.approachDeg > 60.0f && b.approachDeg < 89.0f,
                          "the approach angle is the one the trajectory had");
                    Check(b.rampDeg > 45.0f && b.rampDeg < 62.0f,
                          "and the ramp is read at its real tilt");
                }
            }
            else
            {
                Check(!WrEnergyBoardAvailable(),
                      "with no map, live boards are not offered");
                Check(!got, "and none is invented from the kinematics alone");
            }
        }
    }

    printf("\nthe arriving velocity is SOLVED against the plane, not guessed\n");
    {
        // WHAT THE TEST ABOVE DOES NOT CHECK, AND WHY THAT MATTERED.
        //
        // It asserts `loss` is the projection of whatever vIn the detector
        // happened to fetch. That is true by construction inside WrPhaseBoard,
        // so it would pass with the velocity taken from a completely wrong
        // instant -- and the velocity WAS taken from a wrong instant: a fixed
        // 0.10 s before a detector whose own lag is not fixed, with no
        // correction for the gravity that acted in between.
        //
        // This one knows the right answer. The trajectory is analytic, so the
        // velocity at contact is exact, and so is the loss it must produce.
        const float n[3] = { 0.8f, 0.0f, 0.6f };
        const float g = 800.0f;
        const Vec3 p0 = WrVec(0.0f, 0.0f, 4000.0f);
        const Vec3 v0 = WrVec(500.0f, 0.0f, -600.0f);
        const float tHit = 0.5f;

        const Vec3 vHit = WrVec(v0.x, v0.y, v0.z - g * tHit);
        const Vec3 pHit = WrVec(p0.x + v0.x * tHit, p0.y + v0.y * tHit,
                                p0.z + v0.z * tHit - 0.5f * g * tHit * tHit);
        const float plane[4] = { n[0], n[1], n[2],
                                 n[0] * pHit.x + n[1] * pHit.y + n[2] * pHit.z };

        const float sHit = WrLength(vHit);
        const float dotHit =
            fabsf(vHit.x * n[0] + vHit.y * n[1] + vHit.z * n[2]) / sHit;
        const float lossTrue = sHit * (1.0f - sqrtf(1.0f - dotHit * dotHit));
        const float degTrue = (float)(acos(dotHit) * 57.2957795131);

        const float d = vHit.x * n[0] + vHit.y * n[1] + vHit.z * n[2];
        const Vec3 vC = WrVec(vHit.x - d * n[0], vHit.y - d * n[1],
                              vHit.z - d * n[2]);
        const float gn = -g * n[2];
        const Vec3 aC = WrVec(-gn * n[0], -gn * n[1], -g - gn * n[2]);

        printf("     the trajectory really loses %.3f u/s at %.3f deg\n",
               lossTrue, degTrue);

        // Three frame rates, because the old walk returned the first sample
        // PAST the mark rather than the sample AT it -- so its answer moved
        // with dt, and a board read differently at 60 fps and at 300.
        const float fps[3] = { 60.0f, 144.0f, 300.0f };
        float solved[3], guessed[3];

        for (int mode = 0; mode < 2; mode++)      // 0 = solved, 1 = old way
        {
            for (int k = 0; k < 3; k++)
            {
                const float dt = 1.0f / fps[k];
                WrEnergyDefaults();
                WrEnergyReset();
                // Camera and feet at the same point, so the fixture is about
                // the board arithmetic and not about the eye offset.
                g_energy.eyeHeight = 0.0f;
                g_energy.gravity = g;

                for (int i = 0; i < (int)(1.5f * fps[k]); i++)
                {
                    const float t = i * dt;
                    Vec3 p;
                    if (t < tHit)
                        p = WrVec(p0.x + v0.x * t, p0.y + v0.y * t,
                                  p0.z + v0.z * t - 0.5f * g * t * t);
                    else
                    {
                        const float s = t - tHit;
                        p = WrVec(pHit.x + vC.x * s + 0.5f * aC.x * s * s,
                                  pHit.y + vC.y * s + 0.5f * aC.y * s * s,
                                  pHit.z + vC.z * s + 0.5f * aC.z * s * s);
                    }
                    WrEnergySample(p, dt);
                    const bool touching = (t >= tHit);
                    WrEnergySetGeometryTouch(touching ? WR_GEOM_TOUCHING
                                                      : WR_GEOM_NOTHING,
                                             touching ? n : 0,
                                             (touching && mode == 0) ? plane : 0,
                                             touching ? 0.0f : -1.0f);
                    WrEnergyTickBoards(dt);
                }

                WrBoardStats b;
                float age = 0.0f;
                const float v = WrEnergyBoard(&b, &age) ? b.loss : -1.0f;
                if (mode == 0) solved[k] = v; else guessed[k] = v;

                if (mode == 0)
                    printf("     %3.0f fps  solved %.3f  (err %+.3f)   "
                           "%s\n", fps[k], v, v - lossTrue,
                           WrEnergyBoardExact() ? "entry solved"
                                                : "entry NOT solved");
            }
        }
        printf("     the fixed lookback, same runs: %.3f  %.3f  %.3f\n",
               guessed[0], guessed[1], guessed[2]);

        float worst = 0.0f, spread = 0.0f;
        for (int k = 0; k < 3; k++)
        {
            const float e = fabsf(solved[k] - lossTrue);
            if (e > worst) worst = e;
            for (int j = 0; j < 3; j++)
            {
                const float s = fabsf(solved[k] - solved[j]);
                if (s > spread) spread = s;
            }
        }
        Check(solved[0] > 0.0f && solved[1] > 0.0f && solved[2] > 0.0f,
              "a board is found at every frame rate");
        Check(worst < 0.5f,
              "and its cost is the trajectory's real one, not the projection "
              "of a velocity from the wrong instant");
        Check(spread < 0.25f,
              "the same landing reads the same at 60, 144 and 300 fps");

        // The point of keeping the old path in the fixture: it says how much
        // the fix was worth, rather than asserting that it was worth something.
        float oldWorst = 0.0f;
        for (int k = 0; k < 3; k++)
        {
            const float e = fabsf(guessed[k] - lossTrue);
            if (e > oldWorst) oldWorst = e;
        }
        printf("     worst error   solved %.3f   fixed lookback %.3f u/s\n",
               worst, oldWorst);
        Check(worst < oldWorst,
              "and it is closer than reaching back a fixed distance was");
    }

    printf("\nboards are graded on the game's own velocity when it is offered\n");
    {
        // The same landing, with the pair a proved memory read would supply.
        // What this checks is the PLUMBING -- that a pushed-in velocity reaches
        // the board and displaces the camera estimate -- not the scan, which
        // needs a game and lives in wr_player.cpp.
        const float n[3] = { 0.8f, 0.0f, 0.6f };
        const float g = 800.0f;
        const float dt = 1.0f / 200.0f;
        const Vec3 p0 = WrVec(0.0f, 0.0f, 4000.0f);
        const Vec3 v0 = WrVec(500.0f, 0.0f, -600.0f);
        const float tHit = 0.5f;
        const Vec3 vHit = WrVec(v0.x, v0.y, v0.z - g * tHit);
        const Vec3 pHit = WrVec(p0.x + v0.x * tHit, p0.y + v0.y * tHit,
                                p0.z + v0.z * tHit - 0.5f * g * tHit * tHit);
        const float plane[4] = { n[0], n[1], n[2],
                                 n[0] * pHit.x + n[1] * pHit.y + n[2] * pHit.z };
        const float sHit = WrLength(vHit);
        const float dotHit =
            fabsf(vHit.x * n[0] + vHit.y * n[1] + vHit.z * n[2]) / sHit;
        const float lossTrue = sHit * (1.0f - sqrtf(1.0f - dotHit * dotHit));

        const float d = vHit.x * n[0] + vHit.y * n[1] + vHit.z * n[2];
        const Vec3 vC = WrVec(vHit.x - d * n[0], vHit.y - d * n[1],
                              vHit.z - d * n[2]);
        const float gn = -g * n[2];
        const Vec3 aC = WrVec(-gn * n[0], -gn * n[1], -g - gn * n[2]);

        WrEnergyDefaults();
        WrEnergyReset();
        g_energy.eyeHeight = 64.0f;     // deliberately WRONG for this fixture
        g_energy.gravity = g;

        bool everTrue = false;
        for (int i = 0; i < 300; i++)
        {
            const float t = i * dt;
            Vec3 p, v;
            if (t < tHit)
            {
                p = WrVec(p0.x + v0.x * t, p0.y + v0.y * t,
                          p0.z + v0.z * t - 0.5f * g * t * t);
                v = WrVec(v0.x, v0.y, v0.z - g * t);
            }
            else
            {
                const float s = t - tHit;
                p = WrVec(pHit.x + vC.x * s + 0.5f * aC.x * s * s,
                          pHit.y + vC.y * s + 0.5f * aC.y * s * s,
                          pHit.z + vC.z * s + 0.5f * aC.z * s * s);
                v = WrVec(vC.x + aC.x * s, vC.y + aC.y * s, vC.z + aC.z * s);
            }

            // The camera sits 64 above the origin, as it would in a game. The
            // eye height setting is right here; the point is that the pushed
            // pair is used INSTEAD of it, so the ring holds exact feet.
            const Vec3 cam = WrVec(p.x, p.y, p.z + 64.0f);
            WrEnergySetTruePlayer(&p, &v, 64.0f);
            if (WrEnergyTrueVelocityLive())
                everTrue = true;
            WrEnergySample(cam, dt);
            const bool touching = (t >= tHit);
            WrEnergySetGeometryTouch(touching ? WR_GEOM_TOUCHING
                                              : WR_GEOM_NOTHING,
                                     touching ? n : 0, touching ? plane : 0,
                                     touching ? 0.0f : -1.0f);
            WrEnergyTickBoards(dt);
        }

        WrBoardStats b;
        float age = 0.0f;
        const bool got = WrEnergyBoard(&b, &age);
        Check(everTrue, "the pushed pair is accepted");
        Check(got, "the landing is still found");
        if (got)
        {
            printf("     -%.4f u/s against a true %.4f; entry "
                   "(%.2f %.2f %.2f), wanted (%.2f %.2f %.2f), %s\n",
                   b.loss, lossTrue,
                   b.velIn[0], b.velIn[1], b.velIn[2],
                   vHit.x, vHit.y, vHit.z,
                   WrEnergyBoardExact() ? "solved" : "looked back");
            Check(fabsf(b.loss - lossTrue) < 0.05f,
                  "and the cost comes out to a hundredth of a unit");
        }

        // Nonsense is refused rather than trusted. A scan that has latched onto
        // the wrong address must not be able to put a garbage board on screen.
        const Vec3 mad = WrVec(1e9f, 0.0f, 0.0f);
        const Vec3 ok = WrVec(0.0f, 0.0f, 0.0f);
        WrEnergySetTruePlayer(&ok, &mad, 64.0f);
        Check(!WrEnergyTrueVelocityLive(),
              "a velocity past every sanity bound is not believed");
        WrEnergySetTruePlayer(&ok, 0, 64.0f);
        Check(!WrEnergyTrueVelocityLive(),
              "and an origin without a velocity is not used half-way");
    }

    printf("\na refusal names the test that refused it\n");
    {
        WrEnergyDefaults();
        WrEnergyReset();
        Check(WrEnergyBoardWhy() == WR_BOARD_WHY_NONE,
              "nothing has been refused before anything has happened");

        for (int i = 0; i < WR_BOARD_WHY__COUNT; i++)
            Check(WrEnergyBoardWhyName(i) != 0 &&
                  WrEnergyBoardWhyName(i)[0] != '\0',
                  i == 0 ? "every cause has a phrase" : "");
        Check(strcmp(WrEnergyBoardWhyName(-1), "?") == 0 &&
              strcmp(WrEnergyBoardWhyName(WR_BOARD_WHY__COUNT), "?") == 0,
              "and an index off either end is a question mark, not a crash");

        // A wall, hit out of sustained air, on a map that is answering. Every
        // gate passes except the ramp band -- which used to end in silence.
        //
        // The fall has to actually STOP at the contact, not merely be declared
        // over: the phase readout is vertical acceleration against gravity and
        // the map may only veto a contact, never invent one. A trajectory that
        // goes on falling at exactly -g is airborne no matter what the geometry
        // says, which is the whole design of the one-sided veto.
        const float wall[3] = { 1.0f, 0.0f, 0.0f };
        const float dt = 1.0f / 200.0f;
        const float tHitW = 0.5f;
        const float zHitW = 4000.0f - 0.5f * 800.0f * tHitW * tHitW;
        g_energy.gravity = 800.0f;
        g_energy.eyeHeight = 0.0f;
        for (int i = 0; i < 200; i++)
        {
            const float t = i * dt;
            const bool touching = (t >= tHitW);
            const Vec3 p = touching
                ? WrVec(1000.0f * t, 0.0f, zHitW)
                : WrVec(1000.0f * t, 0.0f, 4000.0f - 0.5f * 800.0f * t * t);
            WrEnergySample(p, dt);
            WrEnergySetGeometryTouch(touching ? WR_GEOM_TOUCHING
                                              : WR_GEOM_NOTHING,
                                     touching ? wall : 0);
            WrEnergyTickBoards(dt);
        }
        Check(!WrEnergyBoard(0, 0), "a wall is not graded as a board");
        Check(WrEnergyBoardWhy() == WR_BOARD_WHY_NOT_RAMP,
              "and the refusal says so instead of going quiet");
        Check(WrEnergyBoardWhyCount(WR_BOARD_WHY_NOT_RAMP) >= 1,
              "the tally counted it");
        Check(WrEnergyBoardWhyCount(WR_BOARD_WHY_NOT_RAMP) < 20,
              "once per landing, not once per frame -- which is what makes the "
              "number readable");
        Check(fabsf(WrEnergyBoardWhyNz() - 0.0f) < 1e-4f,
              "and it carries the n.z it actually saw, so a wall and a floor "
              "can be told apart");
    }

    printf("\na ramp across the room is not the ramp you landed on\n");
    {
        // Preferring the ramp candidate fixes the side-entry defect and can
        // introduce a smaller one in the other direction: a FLOOR landing with
        // a rideable plane somewhere in range would be graded against a surface
        // nobody touched -- a board invented where the old code stayed quiet.
        // Both directions are pinned here, because a fix that only ever adds
        // boards is not obviously a fix.
        const float floorN[3] = { 0.0f, 0.0f, 1.0f };
        const float rampN[3] = { 0.8f, 0.0f, 0.6f };
        const float dt = 1.0f / 200.0f;
        const float tHit = 0.5f;
        const float zHit = 4000.0f - 0.5f * 800.0f * tHit * tHit;
        const float rampPlane[4] = { rampN[0], rampN[1], rampN[2],
                                     rampN[0] * 0.0f + rampN[2] * zHit };

        for (int far_ = 0; far_ < 2; far_++)
        {
            WrEnergyDefaults();
            WrEnergyReset();
            g_energy.eyeHeight = 0.0f;
            g_energy.gravity = 800.0f;

            for (int i = 0; i < 200; i++)
            {
                const float t = i * dt;
                const bool touching = (t >= tHit);
                const Vec3 p = touching
                    ? WrVec(1000.0f * t, 0.0f, zHit)
                    : WrVec(1000.0f * t, 0.0f,
                            4000.0f - 0.5f * 800.0f * t * t);
                WrEnergySample(p, dt);
                // A floor one unit away, and a ramp either just beside it or
                // most of the search radius off.
                WrEnergySetGeometryTouch(touching ? WR_GEOM_TOUCHING
                                                  : WR_GEOM_NOTHING,
                                         touching ? floorN : 0,
                                         touching ? rampPlane : 0,
                                         touching ? (far_ ? 22.0f : 3.0f) : -1.0f,
                                         touching ? 1.0f : -1.0f);
                WrEnergyTickBoards(dt);
            }

            const bool got = WrEnergyBoard(0, 0);
            if (far_)
            {
                Check(!got,
                      "a ramp 22 units away, with a floor at 1, is not what "
                      "was landed on");
                Check(WrEnergyBoardWhy() == WR_BOARD_WHY_NOT_RAMP,
                      "and the refusal is the floor's, named");
            }
            else
            {
                Check(got,
                      "a ramp 3 units away, at a junction the hull straddles, "
                      "still grades");
            }
        }
    }

    printf("\nthe last board does not outlive its own ramp\n");
    {
        // WrEnergyBoard's header always promised an age cap and the code never
        // had one, so the corner row went on describing a ramp two ramps back
        // for the rest of the level.
        const float n[3] = { 0.8f, 0.0f, 0.6f };
        const float dt = 1.0f / 200.0f;
        WrEnergyDefaults();
        WrEnergyReset();
        g_energy.eyeHeight = 0.0f;
        g_energy.gravity = 800.0f;

        const Vec3 p0 = WrVec(0.0f, 0.0f, 4000.0f);
        const Vec3 v0 = WrVec(500.0f, 0.0f, -600.0f);
        const float tHit = 0.5f;
        const Vec3 vHit = WrVec(v0.x, v0.y, v0.z - 800.0f * tHit);
        const Vec3 pHit = WrVec(p0.x + v0.x * tHit, p0.y + v0.y * tHit,
                                p0.z + v0.z * tHit - 0.5f * 800.0f * tHit * tHit);
        const float d = vHit.x * n[0] + vHit.y * n[1] + vHit.z * n[2];
        const Vec3 vC = WrVec(vHit.x - d * n[0], vHit.y - d * n[1],
                              vHit.z - d * n[2]);
        const float gn = -800.0f * n[2];
        const Vec3 aC = WrVec(-gn * n[0], -gn * n[1], -800.0f - gn * n[2]);

        for (int i = 0; i < 200; i++)
        {
            const float t = i * dt;
            Vec3 p;
            if (t < tHit)
                p = WrVec(p0.x + v0.x * t, p0.y + v0.y * t,
                          p0.z + v0.z * t - 0.5f * 800.0f * t * t);
            else
            {
                const float s = t - tHit;
                p = WrVec(pHit.x + vC.x * s + 0.5f * aC.x * s * s,
                          pHit.y + vC.y * s + 0.5f * aC.y * s * s,
                          pHit.z + vC.z * s + 0.5f * aC.z * s * s);
            }
            WrEnergySample(p, dt);
            const bool touching = (t >= tHit);
            WrEnergySetGeometryTouch(touching ? WR_GEOM_TOUCHING
                                              : WR_GEOM_NOTHING,
                                     touching ? n : 0);
            WrEnergyTickBoards(dt);
        }

        float age = 0.0f;
        Check(WrEnergyBoard(0, &age), "the board is there");
        Check(WrEnergyBoard(0, 0, 20.0f), "and a 20 s caller still sees it");

        // Age it without moving: the phase readout goes quiet, the clock does
        // not.
        for (int i = 0; i < 5000; i++)
            WrEnergyTickBoards(dt);
        Check(WrEnergyBoard(0, &age) && age > 24.0f,
              "twenty-five seconds later it is still on record");
        Check(!WrEnergyBoard(0, 0, 20.0f),
              "but a caller that asked for twenty is no longer shown it");
        Check(WrEnergyBoard(0, 0, 0.0f),
              "and zero still means \"however old it is\", for callers that "
              "want the record rather than the readout");
    }

    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
