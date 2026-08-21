// test_phase.cpp  --  air, ramp, ground, and what a board cost.
//
// WHAT GOES WRONG WITHOUT THIS
//
// The whole feature rests on one claim: that in free flight the vertical
// acceleration is exactly -sv_gravity, so anything else is a surface. That claim
// is true of the physics and was measured to hold to 0.1% across 2,294 stored
// runs -- but it is only useful if the code implements the test on the VERTICAL
// component and not on the magnitude of a - g.
//
// That distinction is the entire bug surface of this file. Source's air
// acceleration is purely horizontal, so a player air-strafing hard has a - g
// pointing sideways with a magnitude up to 2000 u/s^2 while a_z is still exactly
// -g. A magnitude test calls that a surface. Measured on real demos it put 36%
// of all samples into a phantom "the normal is horizontal" spike. So the first
// thing asserted below is that a hard air-strafe is still air.
//
// The second is the sign of the normal. The constraint force pushes AWAY from
// the surface, so the normal is +normalize(a - g); getting it backwards is a
// one-character mistake that puts 97.9% of recovered normals underground and
// still produces plausible-looking angles.
//
// The third is that the projection identity holds. Airborne clips use overbounce
// 1.0, which makes ClipVelocity a pure orthogonal projection, and every board
// statistic is a consequence of that. If lossPct and the approach angle ever
// stop agreeing, one of them is being computed from something else.
//
// THIS LINKS NOTHING
//
// wr_phase.h is static inline and includes only math.h, like wr_scale.h and
// wr_stress.h. No run store, no renderer, no ImGui.
//
// Build:  tests\build.bat
// Run:    tests\test_phase.exe

#include "wr_phase.h"
#include "wr_stress.h"   // the air-strafing physics; pure, like this one

#include <stdio.h>
#include <math.h>

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

static const float G = 800.0f;

// Clip a velocity against a plane the way Source does airborne: overbounce 1.0,
// i.e. remove the component along the normal and keep the rest.
static void Clip(const float v[3], const float n[3], float out[3])
{
    float d = v[0] * n[0] + v[1] * n[1] + v[2] * n[2];
    out[0] = v[0] - n[0] * d;
    out[1] = v[1] - n[1] * d;
    out[2] = v[2] - n[2] * d;
}

static void Norm(float v[3])
{
    float m = (float)sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= m; v[1] /= m; v[2] /= m;
}

static float AngleBetween(const float a[3], const float b[3])
{
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    if (d > 1.0f) d = 1.0f;
    if (d < -1.0f) d = -1.0f;
    return (float)(acos(d) * 57.2957795131);
}

int main(void)
{
    printf("\n=== wrlines ramp / air phase detection ===\n\n");

    // -----------------------------------------------------------------------
    printf("a ballistic arc is air, at any frame rate\n");
    {
        // Free fall from 1000 u/s up. Only gravity acts, so every tick must
        // classify as air whatever the interval -- the filters in this project
        // are specified in seconds precisely so a reading does not become a
        // different instrument at a different tick rate.
        bool allAir10 = true, allAir15 = true;
        for (int i = 0; i < 200; i++)
        {
            float h = 0.010f;
            float vz0 = 1000.0f - G * (h * i);
            float vz1 = 1000.0f - G * (h * (i + 1));
            if (WrPhaseIsContact(vz0, vz1, h, G)) allAir10 = false;

            h = 0.015f;
            vz0 = 1000.0f - G * (h * i);
            vz1 = 1000.0f - G * (h * (i + 1));
            if (WrPhaseIsContact(vz0, vz1, h, G)) allAir15 = false;
        }
        Check(allAir10, "200 ticks of free fall at 0.010 are all air");
        Check(allAir15, "200 ticks of free fall at 0.015 are all air");
    }

    // -----------------------------------------------------------------------
    printf("\nair-strafing at full tilt is still air -- the mistake this guards\n");
    {
        // The wishspeed cap is 30 u/s a tick, all of it horizontal. That is up
        // to 2000 u/s^2 of sideways acceleration, and a magnitude test on a - g
        // calls it a surface. The vertical component is untouched.
        const float h = 0.015f;
        float vz0 = 500.0f;
        float vz1 = vz0 - G * h;
        Check(!WrPhaseIsContact(vz0, vz1, h, G),
              "30 u/s of horizontal accel does not move the vertical test");

        // And prove the trap is real: the sideways change really is enormous.
        float vIn[3]  = { 2000.0f, 0.0f, vz0 };
        float vOut[3] = { 2000.0f, 30.0f, vz1 };
        float mag = (float)sqrt((vOut[0] - vIn[0]) * (vOut[0] - vIn[0]) +
                                (vOut[1] - vIn[1]) * (vOut[1] - vIn[1])) / h;
        Check(mag > 1500.0f,
              "the sideways acceleration a magnitude test would trip on is huge");
    }

    // -----------------------------------------------------------------------
    printf("\na 45 degree ramp is found, and its normal points up\n");
    {
        const float h = 0.015f;
        float n[3] = { 0.70710678f, 0.0f, 0.70710678f };   // 45 deg, upward

        // Arrive across the ramp with some speed into it.
        float vIn[3] = { -600.0f, 1800.0f, -900.0f };
        float clipped[3];
        Clip(vIn, n, clipped);

        // What the next sample sees also has the tick's gravity in it.
        float vOut[3] = { clipped[0], clipped[1], clipped[2] - G * h };

        Check(WrPhaseIsContact(vIn[2], vOut[2], h, G), "the boarding tick is contact");

        float got[3];
        Check(WrPhaseNormal(vIn, vOut, h, G, got), "a normal is recovered");
        Check(got[2] > 0.0f, "it points up, not into the ground");
        Check(AngleBetween(got, n) < 1.0f, "it is within 1 degree of the real plane");

        float ang = WrPhaseSurfaceAngle(got);
        Check(ang > 44.0f && ang < 46.0f, "the ramp reads 45 degrees from horizontal");
    }

    // -----------------------------------------------------------------------
    printf("\nwhere standable stops, which is not where you would guess\n");
    {
        // Source refuses to stand on a plane whose normal.z is under 0.7, and
        // that is 45.57 degrees -- so a 45 degree slope IS standable and a
        // "45 degree ramp" is a walkable hill, not a surf ramp. Real surf ramps
        // are steeper: fitted across 38,008 sustained-contact segments of this
        // machine's library the median is 54.1 degrees, normal.z 0.586, and
        // 94.8% of them are past the cut.
        float at45[3] = { 0.70710678f, 0.0f, 0.70710678f };
        float at54[3] = { 0.810f, 0.0f, 0.586f };
        Check(WrPhaseFromNormal(at45) == WR_PHASE_GROUND,
              "45 degrees is standable -- the cut is at 45.57");
        Check(WrPhaseFromNormal(at54) == WR_PHASE_RAMP,
              "54.1 degrees, the measured median surf ramp, is not");

        float ang54 = WrPhaseSurfaceAngle(at54);
        Check(ang54 > 53.8f && ang54 < 54.4f, "and it reads back as 54.1 degrees");
    }

    // -----------------------------------------------------------------------
    printf("\na floor is ground and a wall is a ramp\n");
    {
        float flat[3] = { 0.0f, 0.0f, 1.0f };
        float wall[3] = { 1.0f, 0.0f, 0.0f };
        Check(WrPhaseFromNormal(flat) == WR_PHASE_GROUND, "a flat floor is ground");
        Check(WrPhaseSurfaceAngle(flat) < 0.01f, "and reads 0 degrees");
        Check(WrPhaseFromNormal(wall) == WR_PHASE_RAMP, "a vertical wall is not standable");
        Check(WrPhaseSurfaceAngle(wall) > 89.9f, "and reads 90 degrees");

        // A ramp seen from underneath has a downward normal and is the same ramp.
        float under[3] = { 0.70710678f, 0.0f, -0.70710678f };
        float a = WrPhaseSurfaceAngle(under);
        Check(a > 44.0f && a < 46.0f, "an inverted normal reads the same tilt");
    }

    // -----------------------------------------------------------------------
    printf("\nthe plane fit recovers the ramp from a run of velocities\n");
    {
        float n[3] = { 0.5f, -0.3f, 0.81f };
        Norm(n);

        // Ten ticks of riding it: every velocity lies in the plane, pointing
        // in a different direction each time.
        float vels[3 * 10];
        for (int i = 0; i < 10; i++)
        {
            float raw[3] = { 1200.0f + 40.0f * i, -500.0f + 90.0f * i, -300.0f - 20.0f * i };
            Clip(raw, n, vels + 3 * i);
        }

        float got[3];
        Check(WrPhaseFitNormal(vels, 10, got), "a fit is produced");
        Check(AngleBetween(got, n) < 0.5f, "within half a degree of the real plane");
        Check(got[2] > 0.0f, "resolved to the upward sign");

        float one[3] = { 1.0f, 0.0f, 0.0f };
        Check(!WrPhaseFitNormal(one, 1, got), "one sample is not a plane");
        Check(!WrPhaseFitNormal(0, 10, got), "no samples is not a plane");
    }

    // -----------------------------------------------------------------------
    printf("\nthe projection identity holds: lossPct == 1 - sin(approach)\n");
    {
        float n[3] = { 0.6f, 0.0f, 0.8f };
        bool allGood = true;
        float worst = 0.0f;

        // Sweep how hard the ramp is hit, from a graze to a wall.
        for (int k = 1; k <= 40; k++)
        {
            float into = -30.0f * k;                 // component along the normal
            float along[3] = { -n[2], 0.0f, n[0] };  // in the plane, unit
            float speedAlong = 2500.0f;
            float vIn[3] = { along[0] * speedAlong + n[0] * into,
                             along[1] * speedAlong + n[1] * into,
                             along[2] * speedAlong + n[2] * into };
            float vOut[3];
            Clip(vIn, n, vOut);

            WrBoardStats b;
            if (!WrPhaseBoard(vIn, vOut, n, &b)) { allGood = false; break; }

            float pred = 1.0f - (float)sin(b.approachDeg / 57.2957795131);
            float err = (float)fabs(pred - b.lossPct);
            if (err > worst) worst = err;
            if (err > 1e-3f) allGood = false;

            // And the other consequence of an orthogonal projection.
            float ip = (float)sqrt(b.speedIn * b.speedIn - b.speedOut * b.speedOut);
            if (fabs(ip - b.intoPlane) > 0.5f) allGood = false;
        }
        printf("    worst identity error over 40 entries: %.6f\n", worst);
        Check(allGood, "loss, approach and into-plane agree across the sweep");
    }

    // -----------------------------------------------------------------------
    printf("\nand it still holds at the angles a good surfer actually rides\n");
    {
        // THE REGIME NOTHING TESTED. The sweep above starts at a 30 u/s entry
        // and works towards a wall; every assertion in this file lives between
        // 60 and 89 degrees. A competitive board is 88 to 89.9, and that is
        // exactly where the arithmetic was worst:
        //
        //   dot is under 0.02 there, and it was a float32 dot product of three
        //   terms with operands around 2500 -- an absolute error of order 1e-4,
        //   which is half a percent OF DOT and so a percent of the grade.
        //
        //   lossPct was 1.0f - sinA with sinA above 0.9997, which is a
        //   subtraction that keeps a handful of bits of a quantity the whole
        //   readout is about.
        //
        // The reference is computed in double AND in a form with no
        // cancellation in it: with phi the angle from the PLANE rather than
        // from the normal, 1 - sin(90 - phi) == 1 - cos(phi) == 2 sin^2(phi/2).
        const double nd[3] = { 0.6, 0.0, 0.8 };
        const float n2[3] = { (float)nd[0], (float)nd[1], (float)nd[2] };
        const double along[3] = { -nd[2], 0.0, nd[0] };
        const double s0 = 2500.0;

        double worstRel = 0.0, worstDeg = 0.0, worstOld = 0.0;
        bool ok = true;
        for (int k = 0; k <= 60; k++)
        {
            // 0.05 to 5 degrees off the surface: 89.95 down to 85 off the
            // normal.
            const double phi = (0.05 + k * (5.0 - 0.05) / 60.0) * 0.0174532925199433;
            const double sp = sin(phi), cp = cos(phi);

            float vIn[3];
            for (int a = 0; a < 3; a++)
                vIn[a] = (float)(s0 * (along[a] * cp - nd[a] * sp));

            float vOut[3];
            Clip(vIn, n2, vOut);

            WrBoardStats b;
            if (!WrPhaseBoard(vIn, vOut, n2, &b)) { ok = false; break; }

            const double sHalf = sin(phi * 0.5);
            const double lossTrue = 2.0 * sHalf * sHalf;    // 1 - cos(phi)
            const double degTrue = 90.0 - phi * 57.2957795130823;

            const double rel = fabs(b.lossPct - lossTrue) / lossTrue;
            if (rel > worstRel) worstRel = rel;
            const double dd = fabs(b.approachDeg - degTrue);
            if (dd > worstDeg) worstDeg = dd;

            // The arithmetic as it stood, on the same inputs, so the claim that
            // this was worth changing is a measurement in the output of the
            // test rather than an assertion in a comment. float32 throughout,
            // and lossPct as the subtraction 1 - sinA.
            const float s0f = sqrtf(vIn[0] * vIn[0] + vIn[1] * vIn[1] +
                                    vIn[2] * vIn[2]);
            float dotf = (vIn[0] * n2[0] + vIn[1] * n2[1] + vIn[2] * n2[2]) / s0f;
            if (dotf < 0.0f) dotf = -dotf;
            const float sinAf = sqrtf(1.0f - dotf * dotf);
            const double oldLoss = 1.0f - sinAf;
            const double relOld = fabs(oldLoss - lossTrue) / lossTrue;
            if (relOld > worstOld) worstOld = relOld;
        }

        printf("    85 to 89.95 deg: worst relative error in lossPct %.2e, "
               "worst angle error %.2e deg\n", worstRel, worstDeg);
        printf("    the same sweep through the old float32 arithmetic: %.2e\n",
               worstOld);
        Check(worstRel < worstOld,
              "and it is better than the arithmetic it replaced, measured on "
              "the same inputs rather than argued for");
        Check(ok, "every entry in the band grades");
        // 1e-4 relative is roughly float32's own resolution on the INPUTS,
        // which is the floor: vIn and the plane are both float32 and always
        // will be. What is being asserted is that nothing between them adds to
        // it. The line above prints what the previous arithmetic did on the
        // identical inputs, and it is 9.6e-2 -- so at the angles a good board
        // is actually made at, a tenth of the reported cost was coming from
        // the width of a float rather than from the ramp.
        Check(worstRel < 1e-4,
              "the cost is right to a hundredth of a percent of itself, where "
              "it used to lose a percent to the arithmetic alone");
        Check(worstDeg < 1e-3,
              "and the approach angle to a thousandth of a degree");
    }

    // -----------------------------------------------------------------------
    printf("\nthe horizontal cost survives the same cancellation\n");
    {
        // |v_h| before minus |v_h| after, where the two differ by well under a
        // unit out of thousands. Written as a difference of squares over a sum,
        // which is an identity and not an approximation -- see wr_phase.h.
        const float n3[3] = { 0.6f, 0.0f, 0.8f };
        bool ok = true;
        double worst = 0.0;
        for (int k = 1; k <= 40; k++)
        {
            const double into = -2.0 * k;               // a gentle board
            const double along[3] = { -0.8, 0.0, 0.6 };
            const double s = 2500.0;
            float vIn[3];
            for (int a = 0; a < 3; a++)
                vIn[a] = (float)(along[a] * s + n3[a] * into);
            float vOut[3];
            Clip(vIn, n3, vOut);

            WrBoardStats b;
            if (!WrPhaseBoard(vIn, vOut, n3, &b)) { ok = false; break; }

            // The straightforward form, in double, from the same inputs.
            const double hx = vIn[0], hy = vIn[1];
            const double ox = hx + (double)b.intoPlane * b.normal[0];
            const double oy = hy + (double)b.intoPlane * b.normal[1];
            const double ref = sqrt(hx * hx + hy * hy) - sqrt(ox * ox + oy * oy);
            const double got = WrBoardLossHorizontal(&b);
            const double rel = fabs(got - ref) / (fabs(ref) + 1e-9);
            if (rel > worst) worst = rel;
        }
        printf("    worst relative error against a double reference: %.2e\n",
               worst);
        Check(ok, "every entry grades");
        Check(worst < 1e-5, "and the horizontal figure matches it");
    }

    // -----------------------------------------------------------------------
    printf("\ngrading\n");
    {
        // A near-parallel entry is a good board; a head-on one is not.
        float n[3] = { 0.0f, 0.0f, 1.0f };
        float clean[3] = { 3000.0f, 0.0f, -10.0f };
        float slam[3]  = { 1500.0f, 0.0f, -1500.0f };
        float out[3];

        WrBoardStats a, b;
        Clip(clean, n, out);
        WrPhaseBoard(clean, out, n, &a);
        Clip(slam, n, out);
        WrPhaseBoard(slam, out, n, &b);

        Check(a.grade < b.grade, "a glancing entry grades better than a slam");
        Check(a.approachDeg > 89.0f, "the glancing entry is near parallel");
        Check(b.loss > 400.0f, "the slam costs hundreds of units");
        Check(WrPhaseGrade(0.0f, 90.0f, 1000.0f) == WR_GRADE_PERFECT,
              "a lossless parallel entry is perfect");
        Check(WrPhaseGrade(0.5f, 20.0f, 1000.0f) == WR_GRADE_TERRIBLE,
              "half your speed into a wall is terrible");

        // The speed relaxation only ever loosens, never tightens.
        unsigned char slow = WrPhaseGrade(0.006f, 86.0f, 1000.0f);
        unsigned char fast = WrPhaseGrade(0.006f, 86.0f, 3000.0f);
        Check(fast <= slow, "the same geometry grades no worse at higher speed");
        Check(WrPhaseGrade(0.006f, 86.0f, 100.0f) == slow,
              "below the reference speed nothing is scaled");
    }

    // -----------------------------------------------------------------------
    printf("\nteleports are rejected rather than graded\n");
    {
        // A fail trigger or a save-loc load moves you thousands of units in one
        // tick. Differencing across it produces an acceleration of tens of
        // thousands, which would otherwise be the most spectacular board in the
        // library. 65 of these turned up across 40 real runs.
        Check(WrPhaseIsTeleport(4000.0f, 2000.0f, 0.015f), "a 4000 unit step is a teleport");
        Check(!WrPhaseIsTeleport(30.0f, 2000.0f, 0.015f), "ordinary motion is not");
        Check(!WrPhaseIsTeleport(5.0f, 0.0f, 0.015f), "a small step while stopped is not");
        Check(WrPhaseIsTeleport(10.0f, 100.0f, 0.0f), "a zero-length tick is unusable");
    }

    // -----------------------------------------------------------------------
    printf("\nsustained surfing is not reported as a board\n");
    {
        // Riding a ramp, the only thing clipped each tick is the into-plane
        // component gravity accrued since the last one. That is a handful of
        // units a second, and it must not be graded -- reproducing the reference
        // plugin's rule against demo data called 84% of these perfect.
        float n[3] = { 0.6f, 0.0f, 0.8f };
        float raw[3] = { 1800.0f, 400.0f, -600.0f };
        float rid[3];
        Clip(raw, n, rid);

        float vIn[3] = { rid[0], rid[1], rid[2] };
        float vOut[3] = { rid[0], rid[1], rid[2] - G * 0.015f };
        float clipped[3];
        Clip(vOut, n, clipped);

        WrBoardStats b;
        WrPhaseBoard(vIn, clipped, n, &b);
        Check(!WrPhaseBoardWorthReporting(&b),
              "one tick of gravity into the ramp is below the board threshold");

        // Whereas an actual arrival is well over it.
        float slam[3] = { 1500.0f, 0.0f, -1200.0f };
        Clip(slam, n, clipped);
        WrPhaseBoard(slam, clipped, n, &b);
        Check(WrPhaseBoardWorthReporting(&b), "a real arrival is over it");

        float slow[3] = { 10.0f, 0.0f, -40.0f };
        Clip(slow, n, clipped);
        WrPhaseBoard(slow, clipped, n, &b);
        Check(!WrPhaseBoardWorthReporting(&b), "walking pace is never a board");
    }

    // -----------------------------------------------------------------------
    printf("\ndegenerate input is refused rather than guessed\n");
    {
        float v[3] = { 100.0f, 0.0f, 0.0f };
        float got[3];
        Check(!WrPhaseNormal(v, v, 0.0f, G, got), "a zero interval has no normal");

        // Two samples that differ only by exactly one tick of gravity are free
        // flight: there is no constraint force, so there is nothing to point at.
        float a[3] = { 100.0f, 0.0f, 0.0f };
        float b[3] = { 100.0f, 0.0f, -G * 0.015f };
        Check(!WrPhaseNormal(a, b, 0.015f, G, got), "free flight has no normal");

        WrBoardStats s;
        float zero[3] = { 0.0f, 0.0f, 0.0f };
        float n[3] = { 0.0f, 0.0f, 1.0f };
        Check(!WrPhaseBoard(zero, zero, n, &s), "a stationary player is not a board");
        Check(!WrPhaseBoard(a, b, n, 0), "a null output is refused");
    }

    // -----------------------------------------------------------------------
    printf("\nthe perfect strafe angle, and where the other formula breaks\n");
    {
        // Surf: airaccel 150, maxspeed 250, 0.015 tick. accelspeed is 562.5, so
        // the 30 u/s wishspeed cap binds and the gain is 30.
        Check(WrAirGainPerTick(0.015f, 150.0f, 250.0f) == 30.0f,
              "at surf settings the wishspeed cap binds, so a tick adds 30");

        // Low air acceleration: 3 * 250 * 0.015 = 11.25, under the cap, so the
        // acceleration binds instead and the gain is smaller.
        float low = WrAirGainPerTick(0.015f, 3.0f, 250.0f);
        Check(low > 11.2f && low < 11.3f,
              "at airaccel 3 the acceleration binds instead, at 11.25");

        // The ideal turn slows as you speed up -- the same 30 units a tick buys
        // a smaller angle. This is the property the readout is for.
        float a1000 = WrPerfectStrafeDegrees(1000.0f, 0.015f, 150.0f, 250.0f);
        float a3000 = WrPerfectStrafeDegrees(3000.0f, 0.015f, 150.0f, 250.0f);
        Check(a1000 > a3000, "the ideal angle shrinks as speed rises");
        Check(a3000 * 3.0f > a1000 * 0.98f && a3000 * 3.0f < a1000 * 1.02f,
              "and shrinks as 1/speed, since atan is linear this small");

        // atan(30/1000) = 1.718 degrees.
        Check(a1000 > 1.70f && a1000 < 1.74f, "atan(30/1000) is 1.72 degrees");

        // THE DISAGREEMENT WORTH PINNING. The strafe-analyzer project computes
        // min(tick * 30 * airaccel, 30), which puts the wishspeed cap where
        // sv_maxspeed belongs. At surf settings both saturate at 30 and nobody
        // notices. At CS:GO KZ's airaccelerate 12 on a 64 tick they part
        // company: Source gives min(12 * 250 / 64, 30) = 30, and theirs gives
        // 12 * 30 / 64 = 5.6 -- and that is exactly the configuration somebody
        // would reach for this to check.
        float kz = WrAirGainPerTick(1.0f / 64.0f, 12.0f, 250.0f);
        float theirs = 12.0f * 30.0f / 64.0f;
        Check(kz == 30.0f, "at KZ settings Source still caps the gain at 30");
        Check(theirs < 6.0f, "the other formula gives 5.6 for the same case");

        // Degenerate input is clamped rather than dividing by zero.
        Check(WrPerfectStrafeDegrees(0.0f, 0.015f, 150.0f, 250.0f) > 0.0f,
              "a standing player gets a finite angle, not a divide by zero");
        Check(WrAirGainPerTick(0.015f, 0.0f, 250.0f) == 0.0f,
              "no air acceleration means no gain");
    }

    printf("\n");
    if (g_failures)
    {
        printf("=== %d FAILED ===\n\n", g_failures);
        return 1;
    }
    printf("=== all phase checks passed ===\n\n");
    return 0;
}
