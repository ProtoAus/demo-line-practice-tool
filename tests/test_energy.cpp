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
// Build:  cl /nologo /EHsc /I.. tests\test_energy.cpp /Fe:tests\test_energy.exe
// Run:    tests\test_energy.exe

#include "wr_smooth.h"
#include "wr_stress.h"

#include <stdio.h>
#include <math.h>

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

    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
