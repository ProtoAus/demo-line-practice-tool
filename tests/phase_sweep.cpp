// phase_sweep.cpp  --  the measurements behind wr_phase.h, re-derivable.
//
// Every number in wr_phase.h's header and in WrEnergyPhase's came from this
// program run over a real .wrpath library. It is committed so they can be
// checked rather than believed, and so that changing the classifier is a thing
// with a measurable consequence rather than a matter of taste.
//
// It is NOT part of the harness run. It needs thousands of real runs on disk,
// which are other people's demos and are not in this repository -- the same
// reason tests\parity.ps1 is a separate script. test_phase.exe is the part that
// runs everywhere, on synthetic trajectories where the answer is known exactly.
//
//   tests\phase_sweep.exe [wrlines_data\paths] [--live]
//
// Two questions, one program.
//
// THE CORPUS SWEEP asks what the classifier finds on stored runs, where the
// velocity is a central difference of exact recorded positions and reads gravity
// back to 0.1%. This is the demo-line case and it is close to exact.
//
// THE LIVE SIMULATION asks whether the same test survives being fed a velocity
// differenced from CAMERA positions. It resamples each run at 200 Hz, adds view
// bob, and pushes it through the real wr_smooth.h estimator with the real
// settings out of wr_energy.cpp -- so what is being measured is the shipped
// filter chain and not an idealisation of it. This is the method wr_stress.h
// used to decide that live efficiency colouring could not be shipped.

#include "wr_phase.h"
#include "wr_smooth.h"
#include "wr_stress.h"      // the strafe quality curve this checks
#include "wr_bsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>

// sv_gravity. Not a setting here: the whole point is to measure against the
// value the runs were recorded at, and every surf map on the leaderboard is 800.
static const float G = 800.0f;

// The live filter chain, from wr_energy.cpp's defaults. Kept as named constants
// so a drift between this and the shipped values is visible in a diff.
static const float VEL_WINDOW = 0.040f;     // VEL_WINDOW_SECONDS
static const float VEL_TAU = 0.060f;        // VEL_TAU
static const float FPS = 200.0f;

struct Pt { float x, y, z, vx, vy, vz, t; };

// The .wrpath layout, from wr_path.cpp: a 0x100 header then 28-byte points of
// x y z vx vy vz t. Read directly rather than through the loader so this program
// links nothing but the two pure headers it is measuring.
static Pt *Load(const char *path, int *outN)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char head[0x100];
    if (fread(head, 1, 0x100, f) != 0x100) { fclose(f); return 0; }
    if (memcmp(head, "WRPATH\0\0", 8) != 0) { fclose(f); return 0; }
    unsigned int n = *(unsigned int *)(head + 0x10);
    if (n < 64 || n > 5000000) { fclose(f); return 0; }
    Pt *p = (Pt *)malloc(sizeof(Pt) * n);
    if (!p) { fclose(f); return 0; }
    if (fread(p, sizeof(Pt), n, f) != n) { free(p); fclose(f); return 0; }
    fclose(f);
    *outN = (int)n;
    return p;
}

static int CmpF(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

// ---------------------------------------------------------------------------
// The corpus sweep
// ---------------------------------------------------------------------------

static long long g_air, g_contact, g_tele;
static float *g_ang; static int g_angN, g_angCap;
static int g_grades[WR_GRADE_COUNT];
static int g_boards, g_runs;

// HOW LONG A LIVE BOARD READOUT MAY KEEP THE LAST ONE.
//
// The corner row held its board for ever, so a number from two ramps back read
// exactly like the one under your feet. Any cap is arbitrary unless it is
// measured against what a surf line actually does, and what it has to clear is
// the gap between consecutive boards on one run: shorter than that and the row
// goes blank between ramps for no reason; much longer and it is describing
// somewhere else. WR_BOARD_ROW_LIFE in wr_render.cpp is set from the p95 here.
static float *g_bGap; static int g_bGapN, g_bGapCap;

// Board loss, three ways, because a player reported every board reading "-0"
// while its grade and its angle both varied. Those cannot both be right: a
// board approached at 57 degrees off the normal MUST lose 1 - sin(57) = 16% of
// its speed, and 16% of anything is not zero.
//
//   measured   |v[i]| - |v[i+1]|, what the tool shows today
//   wide       |v[i-1]| - |v[i+2]|, well outside the smear but three ticks of
//              ramp acceleration are now inside the measurement instead
//   clean      |v[i-1]| - |v[i+1]|, the pair the algebra says is exact
//   implied    s0 * (1 - sin(approach)), what the recovered normal predicts
//
// WHY THERE IS A SMEAR AT ALL, AND WHY IT LANDS ON EXACTLY ONE PAIR
//
// A .wrpath velocity is a CENTRAL difference (wr_dp.cpp), v[k] = (p[k+1] -
// p[k-1]) / 2h. WrPhaseIsContact compares ticks i and i+1, so a board's clip
// sits BETWEEN them. Write u for the velocity before it and w for after:
//
//     v[i-1] = (p[i]   - p[i-2]) / 2h = (u + u) / 2 = u      clean, pre-clip
//     v[i]   = (p[i+1] - p[i-1]) / 2h = (u + w) / 2          SMEARED
//     v[i+1] = (p[i+2] - p[i]  ) / 2h = (w + w) / 2 = w      clean, post-clip
//
// v[i] is the one sample in the entire run that straddles the event, and it is
// the one the board reads as its "before". So the reported drop is |(u+w)/2| -
// |w| when the real drop is |u| - |w| -- and (u+w)/2 already contains half the
// clip, which is most of the way to w. Hence "-0".
static float *g_lossM, *g_lossW, *g_lossC, *g_lossI;
static int g_lossN, g_lossCap;
static int g_lossZero, g_lossProjZero;   // how many would print as "-0"
static int g_gradesClean[WR_GRADE_COUNT];
static float *g_angGap; static int g_angGapN, g_angGapCap;
static float *g_trendA, *g_trendB; static int g_trendN, g_trendCap;

// The speed change over a tick with NO clip in it, either side of the board:
// before  |v[i]|   - |v[i-1]|   still in free flight
// after   |v[i+2]| - |v[i+1]|   already riding
// A board's loss is superimposed on whatever this is, and a difference of two
// speeds measures their sum. This is the size of the thing being added.
static void PushTrend(float before, float after)
{
    if (g_trendN == g_trendCap)
    {
        g_trendCap = g_trendCap ? g_trendCap * 2 : 1024;
        g_trendA = (float *)realloc(g_trendA, sizeof(float) * g_trendCap);
        g_trendB = (float *)realloc(g_trendB, sizeof(float) * g_trendCap);
    }
    g_trendA[g_trendN] = before;
    g_trendB[g_trendN] = after;
    g_trendN++;
}

// How far the clean pair moves the approach angle. If the smear only cost speed
// this is near zero; if it also flattered the angle, the grade was wrong too.
static void PushBoardGap(float v)
{
    if (g_bGapN == g_bGapCap)
    {
        g_bGapCap = g_bGapCap ? g_bGapCap * 2 : 1024;
        g_bGap = (float *)realloc(g_bGap, sizeof(float) * g_bGapCap);
    }
    g_bGap[g_bGapN++] = v;
}

static void PushAngleGap(float v)
{
    if (g_angGapN == g_angGapCap)
    {
        g_angGapCap = g_angGapCap ? g_angGapCap * 2 : 1024;
        g_angGap = (float *)realloc(g_angGap, sizeof(float) * g_angGapCap);
    }
    g_angGap[g_angGapN++] = v;
}

static void PushLoss(float m, float w, float c, float im)
{
    if (g_lossN == g_lossCap)
    {
        g_lossCap = g_lossCap ? g_lossCap * 2 : 1024;
        g_lossM = (float *)realloc(g_lossM, sizeof(float) * g_lossCap);
        g_lossW = (float *)realloc(g_lossW, sizeof(float) * g_lossCap);
        g_lossC = (float *)realloc(g_lossC, sizeof(float) * g_lossCap);
        g_lossI = (float *)realloc(g_lossI, sizeof(float) * g_lossCap);
    }
    g_lossM[g_lossN] = m;
    g_lossW[g_lossN] = w;
    g_lossC[g_lossN] = c;
    g_lossI[g_lossN] = im;
    g_lossN++;
    if (m < 0.5f) g_lossZero++;
    if (im < 0.5f) g_lossProjZero++;
}

// The same board read off the clean pair: v[i-1] against v[i+1], which are 2h
// apart, so gravity gets 2h too.
static bool CleanBoard(Pt *p, int i, int n, WrBoardStats *out)
{
    if (i - 1 < 0 || i + 1 >= n)
        return false;
    Pt &a = p[i - 1], &b = p[i + 1];
    float h2 = b.t - a.t;
    float vIn[3]  = { a.vx, a.vy, a.vz };
    float vOut[3] = { b.vx, b.vy, b.vz };
    float nrm[3];
    if (!WrPhaseNormal(vIn, vOut, h2, G, nrm))
        return false;
    return WrPhaseBoard(vIn, vOut, nrm, out);
}

// ---------------------------------------------------------------------------
// The strafe quality curve, against runs that never heard of it
// ---------------------------------------------------------------------------
//
// wr_stress.h derives quality = 1 - (1 - r)^2 from AirAccelerate, and
// tests\test_energy.exe checks that against a literal transcription of the
// engine function. Both of those are the same claim checked twice. This is the
// outside check: real airborne ticks from real demos, binned by the turn ratio
// they actually had, reporting the speed they actually gained.
//
// HORIZONTAL ONLY, and that is what makes it clean. AirMove zeroes wishdir's z,
// so air acceleration cannot touch vz -- and gravity cannot touch vx or vy. In
// free flight the horizontal speed therefore changes for exactly one reason, and
// d|v_h|^2 over a tick IS the air-strafe gain with nothing to subtract off.
//
// TURN OF THE VELOCITY, not of the view. The derivation's w is the rate the
// velocity direction rotates; the live readout uses the YAW rate instead,
// because that is what a camera can measure, and the two are equal in steady
// state. A .wrpath stores no view angles, so this validates the formula and not
// that substitution -- worth saying rather than implying the whole chain is
// checked.
// TWO TABLES, AND THE SECOND ONE IS THE MEASUREMENT.
//
// Per tick, r is a ratio of two angles that are both about 1.7 degrees at surf
// speed, recovered from central differences. It is a noisy estimate, and the
// curve being measured is CONCAVE -- so by Jensen noise in r pulls every bin's
// mean gain down, hardest at the peak where the curvature is, and binning by a
// noisy r also drags the extremes toward the middle of the r distribution. Both
// effects flatten the curve and move its apex toward the bulk of the samples.
//
// So the same thing is measured again over a window: mean r across Q_WIN ticks
// of unbroken air against the total speed gained over that same window. Noise in
// the mean falls as 1/sqrt(Q_WIN) and the two tables printed side by side are
// the evidence for which reading to believe.
#define QBINS 20                 // r from 0 to 2 in steps of 0.1
#define Q_MIN_SPEED 200.0f       // below this the per-tick angle is all noise
#define Q_WIN 16                 // ticks, about a quarter second
static double g_qSum[QBINS];
static long long g_qN[QBINS];
static double g_qwSum[QBINS];
static long long g_qwN[QBINS];
static const float Q_ACCEL = WR_AIR_ACCEL_DEFAULT;
static const float Q_MAXSPEED = WR_MAXSPEED_DEFAULT;

// The turn ratio of one tick pair, or -1 when it cannot be had.
static float StrafeRatio(Pt *p, int i, float *gainOut)
{
    Pt &a = p[i], &b = p[i + 1];
    const float h = b.t - a.t;
    if (!(h > 1e-4f))
        return -1.0f;

    const float s0 = sqrtf(a.vx * a.vx + a.vy * a.vy);
    const float s1 = sqrtf(b.vx * b.vx + b.vy * b.vy);
    if (s0 < Q_MIN_SPEED || s1 < Q_MIN_SPEED)
        return -1.0f;

    float dot = (a.vx * b.vx + a.vy * b.vy) / (s0 * s1);
    if (dot > 1.0f) dot = 1.0f;
    if (dot < -1.0f) dot = -1.0f;
    const float turned = (float)(acos(dot) * 57.2957795131) / h;

    const float ideal = WrPerfectStrafeDegrees(s0, h, Q_ACCEL, Q_MAXSPEED) / h;
    if (!(ideal > 1e-3f))
        return -1.0f;

    if (gainOut) *gainOut = s1 * s1 - s0 * s0;
    return turned / ideal;
}

// How much clear air either side of the pair. Contamination matters more here
// than sample count: a ramp turns the velocity far faster than air strafing can,
// so a misclassified contact tick lands in the HIGH-r bins specifically and
// brings a ClipVelocity loss with it. Widening this is the test for that.
#ifndef Q_AIR_GUARD
#define Q_AIR_GUARD 6
#endif

static void PushStrafe(Pt *p, int i, signed char *st, int n)
{
    if (i - Q_AIR_GUARD < 0 || i + 1 + Q_AIR_GUARD >= n)
        return;
    // Air on both sides of the pair being differenced. A contact anywhere in the
    // window puts ClipVelocity into the horizontal change and this stops being a
    // measurement of strafing.
    for (int k = i - Q_AIR_GUARD; k <= i + Q_AIR_GUARD; k++)
        if (st[k] != 0)
            return;

    float gain = 0.0f;
    const float r = StrafeRatio(p, i, &gain);
    if (r < 0.0f)
        return;

    int bin = (int)(r * (QBINS / 2.0f));
    if (bin < 0 || bin >= QBINS)
        return;

    g_qSum[bin] += (double)gain / 900.0;
    g_qN[bin]++;
}

// The same question over Q_WIN ticks at once: the mean ratio held across the
// window against the speed actually gained across it.
static void PushStrafeWindow(Pt *p, int i, signed char *st, int n)
{
    if (i - Q_AIR_GUARD < 0 || i + Q_WIN + Q_AIR_GUARD >= n)
        return;
    for (int k = i - Q_AIR_GUARD; k <= i + Q_WIN + Q_AIR_GUARD; k++)
        if (st[k] != 0)
            return;

    double sumR = 0.0;
    for (int k = 0; k < Q_WIN; k++)
    {
        const float r = StrafeRatio(p, i + k, 0);
        if (r < 0.0f)
            return;
        sumR += r;
    }
    const double meanR = sumR / Q_WIN;

    const float s0 = sqrtf(p[i].vx * p[i].vx + p[i].vy * p[i].vy);
    const float s1 = sqrtf(p[i + Q_WIN].vx * p[i + Q_WIN].vx +
                           p[i + Q_WIN].vy * p[i + Q_WIN].vy);

    int bin = (int)(meanR * (QBINS / 2.0));
    if (bin < 0 || bin >= QBINS)
        return;

    g_qwSum[bin] += (double)(s1 * s1 - s0 * s0) / (900.0 * Q_WIN);
    g_qwN[bin]++;
}

static void PushAngle(float v)
{
    if (g_angN == g_angCap)
    {
        g_angCap = g_angCap ? g_angCap * 2 : 1024;
        g_ang = (float *)realloc(g_ang, sizeof(float) * g_angCap);
    }
    g_ang[g_angN++] = v;
}

static void SweepRun(Pt *p, int n)
{
    if (n < 300) return;
    g_runs++;

    signed char *st = (signed char *)malloc(n);
    if (!st) return;
    for (int i = 0; i + 1 < n; i++)
    {
        Pt &a = p[i], &b = p[i + 1];
        float h = b.t - a.t;
        float s0 = sqrtf(a.vx * a.vx + a.vy * a.vy + a.vz * a.vz);
        float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
        if (WrPhaseIsTeleport(sqrtf(dx * dx + dy * dy + dz * dz), s0, h))
        { st[i] = -1; g_tele++; continue; }
        bool c = WrPhaseIsContact(a.vz, b.vz, h, G);
        st[i] = c ? 1 : 0;
        if (c) g_contact++; else g_air++;
    }

    for (int i = 0; i + 1 < n; i++)
    {
        PushStrafe(p, i, st, n);
        PushStrafeWindow(p, i, st, n);
    }

    // Sustained contact -> a fitted plane -> the ramp's angle.
    for (int i = 0; i + 1 < n; )
    {
        if (st[i] != 1) { i++; continue; }
        int j = i;
        while (j + 1 < n && st[j] == 1) j++;
        if (j - i >= 8)
        {
            int m = j - i + 1;
            float *vs = (float *)malloc(sizeof(float) * 3 * (size_t)m);
            if (vs)
            {
                bool fast = true;
                for (int k = 0; k < m; k++)
                {
                    Pt &q = p[i + k];
                    vs[3 * k + 0] = q.vx; vs[3 * k + 1] = q.vy; vs[3 * k + 2] = q.vz;
                    if (sqrtf(q.vx * q.vx + q.vy * q.vy + q.vz * q.vz) < 250.0f)
                        fast = false;
                }
                float nrm[3];
                if (fast && WrPhaseFitNormal(vs, m, nrm))
                    PushAngle(WrPhaseSurfaceAngle(nrm));
                free(vs);
            }
        }
        i = j;
    }

    // Boards: the air-to-contact transition, the same rule wr_path.cpp uses.
    float lastBoardT = -1.0f;
    for (int i = 3; i + 7 < n - 1; i++)
    {
        if (!(st[i] == 1 && st[i - 1] == 0 && st[i - 2] == 0 && st[i - 3] == 0))
            continue;
        int stick = 0;
        for (int k = i; k < i + 6; k++) if (st[k] == 1) stick++;
        if (stick < 4) continue;

        Pt &a = p[i], &b = p[i + 1];
        float h = b.t - a.t;
        float vIn[3] = { a.vx, a.vy, a.vz };
        float vOut[3] = { b.vx, b.vy, b.vz };
        float nrm[3];
        if (!WrPhaseNormal(vIn, vOut, h, G, nrm)) continue;
        float nz = nrm[2] < 0 ? -nrm[2] : nrm[2];
        if (nz < WR_PHASE_MIN_RAMP_NZ || nz > WR_PHASE_STANDABLE) continue;

        WrBoardStats s;
        if (!WrPhaseBoard(vIn, vOut, nrm, &s)) continue;
        if (!WrPhaseBoardWorthReporting(&s)) continue;
        g_boards++;
        g_grades[s.grade]++;

        // On the same run only -- a gap measured across a file boundary is the
        // time between two different people's sessions and means nothing.
        if (lastBoardT >= 0.0f && a.t > lastBoardT)
            PushBoardGap(a.t - lastBoardT);
        lastBoardT = a.t;

        // The four ways of asking what the board cost. WrPhaseBoard now returns
        // the projected one, so the differenced ones are computed here rather
        // than read off it -- this table is the record of WHY it does that and
        // it has to keep being able to disagree with it.
        float sIn  = sqrtf(a.vx * a.vx + a.vy * a.vy + a.vz * a.vz);
        float sOut = sqrtf(b.vx * b.vx + b.vy * b.vy + b.vz * b.vz);
        float measured = (sIn > sOut) ? (sIn - sOut) : 0.0f;

        float wide = 0.0f;
        if (i - 1 >= 0 && i + 2 < n)
        {
            Pt &a2 = p[i - 1], &b2 = p[i + 2];
            float w0 = sqrtf(a2.vx * a2.vx + a2.vy * a2.vy + a2.vz * a2.vz);
            float w1 = sqrtf(b2.vx * b2.vx + b2.vy * b2.vy + b2.vz * b2.vz);
            wide = (w0 > w1) ? (w0 - w1) : 0.0f;
        }
        WrBoardStats cs;
        float clean = 0.0f;
        if (CleanBoard(p, i, n, &cs))
        {
            // The differenced form of the clean pair, not cs.loss -- same
            // reason as above.
            Pt &c0 = p[i - 1], &c1 = p[i + 1];
            float k0 = sqrtf(c0.vx * c0.vx + c0.vy * c0.vy + c0.vz * c0.vz);
            float k1 = sqrtf(c1.vx * c1.vx + c1.vy * c1.vy + c1.vz * c1.vz);
            clean = (k0 > k1) ? (k0 - k1) : 0.0f;
            g_gradesClean[cs.grade]++;
            float gap = cs.approachDeg - s.approachDeg;
            PushAngleGap(gap < 0.0f ? -gap : gap);
        }

        if (i - 1 >= 0 && i + 2 < n)
        {
            Pt &m1 = p[i - 1], &p1 = p[i + 1], &p2 = p[i + 2];
            float sm1 = sqrtf(m1.vx * m1.vx + m1.vy * m1.vy + m1.vz * m1.vz);
            float sp1 = sqrtf(p1.vx * p1.vx + p1.vy * p1.vy + p1.vz * p1.vz);
            float sp2 = sqrtf(p2.vx * p2.vx + p2.vy * p2.vy + p2.vz * p2.vz);
            PushTrend(s.speedIn - sm1, sp2 - sp1);
        }

        PushLoss(measured, wide, clean, s.loss);
    }
    free(st);
}

// ---------------------------------------------------------------------------
// The live simulation
// ---------------------------------------------------------------------------

struct Score { long long agree, total, falseAir, falseContact; };

#define NWIN 5
#define NTOL 4
static const float kWin[NWIN] = { 0.05f, 0.10f, 0.15f, 0.20f, 0.30f };
static const float kTol[NTOL] = { 150.0f, 250.0f, 320.0f, 500.0f };
static Score g_score[NWIN][NTOL];
static float g_bob = 2.0f;
static int g_simRuns = 0;

// ---------------------------------------------------------------------------
// The geometry veto
// ---------------------------------------------------------------------------
//
// The kinematic test's error is one-sided: it essentially never misses a ramp
// and it invents one a few percent of the time. A ray traced straight down
// through the map cannot invent a surface that is not there, so the two compose
// -- contact from the kinematics, air from the geometry, and never the reverse.
//
// AND THE GATE IS THE POINT OF MEASURING IT. This reader has no displacements,
// so on a map that uses them "nothing under you" can mean "nothing modelled".
// The tables below score brush-only and displacement maps separately for exactly
// that reason: if the second one is a disaster, the gate in
// WrBspLoadGeometryComplete is earning its keep rather than being cautious.
//
// The feet position is the one the DLL would actually use: the camera less a
// FIXED 64, so the view bob is still in it. Modelling the bob out of the trace
// would be measuring a tool nobody ships.
// The radius swept, in units. A player hull is 32 wide and 72 tall, so contact
// puts a surface within about 16 of the origin sideways and 0 below -- but the
// origin is recovered, the bob is on top of it, and the polygon set is clipped
// brush sides rather than the collision hull the engine used. So the useful
// radius is a question for the data rather than for arithmetic.
#define NRAD 5
static const float kRad[NRAD] = { 8.0f, 16.0f, 24.0f, 32.0f, 48.0f };

#define SHIPPED_TOL 1                       // kTol[1] == 250

static Score g_scoreBrush[NWIN];            // brush-only maps, no veto
static Score g_vetoBrush[NWIN][NRAD];       // brush-only maps, vetoed
static Score g_scoreDisp[NWIN];             // displacement maps, no veto
static Score g_vetoDisp[NWIN][NRAD];        // displacement maps, vetoed anyway

static WrBspMap *g_map = NULL;              // the map the current run is on
static bool g_mapComplete = false;          // ...and whether absence proves
static int g_mapsLoaded = 0, g_mapsMissing = 0, g_mapsDisp = 0;
static const char *g_mapsDir = NULL;

// HOW CLOSE THE NEAREST SURFACE IS, and NOT what is directly underneath.
//
// Straight down was the first thing tried, on the reasoning that a player in
// contact is standing on something. It cost 25% MISSES against 0.3% shipped --
// it takes back the invented surfaces and most of the real ones with them --
// and the reason is geometric rather than a bug. A surf ramp is 45 to 60
// degrees. The player origin is the bottom centre of a 32-wide hull, and on a
// slope that hull rests against the surface on its SIDE, so the ramp is beside
// the origin rather than beneath it and a vertical ray goes past it to the floor
// far below, or to nothing. bsp_sweep sees the same thing from the other end:
// 10,819 of 33,170 sustained ride segments have no face directly under them.
//
// Distance to the nearest polygon of any facing is the question that actually
// matches "am I touching something", and WrBspNearestFace measures to the
// POLYGON rather than to its plane for exactly this reason -- see its own
// comment, which was written about a player origin a few units off a surface.
#define SIM_NEAR_SEARCH 96.0f
#define SIM_NEVER 1e9f

static float SimNearest(const WrBspMap *m, float x, float y, float z)
{
    const float p[3] = { x, y, z };
    int poly = -1;
    float dist = 0.0f;
    if (!WrBspNearestFace(m, p, SIM_NEAR_SEARCH, &poly, &dist))
        return SIM_NEVER;
    return dist;
}

#define VZ_RING 8192

static void SimRun(Pt *p, int n)
{
    // Runs with a teleport in them are skipped whole. This measures the
    // ESTIMATOR; the teleport guard is already pinned in test_phase.
    for (int i = 0; i + 1 < n; i++)
    {
        float h = p[i + 1].t - p[i].t;
        float s0 = sqrtf(p[i].vx * p[i].vx + p[i].vy * p[i].vy + p[i].vz * p[i].vz);
        float dx = p[i + 1].x - p[i].x, dy = p[i + 1].y - p[i].y, dz = p[i + 1].z - p[i].z;
        if (WrPhaseIsTeleport(sqrtf(dx * dx + dy * dy + dz * dz), s0, h))
            return;
    }

    const float t0 = p[0].t, t1 = p[n - 1].t;
    if (t1 - t0 < 4.0f) return;
    g_simRuns++;

    WrVelWindow w;
    memset(&w, 0, sizeof(w));

    static float vzEst[VZ_RING], tEst[VZ_RING];
    static float nearEst[VZ_RING];      // distance to the nearest map surface
    int nEst = 0;
    float ema[3] = { 0.0f, 0.0f, 0.0f };
    bool emaOn = false;
    const float dt = 1.0f / FPS;
    int tick = 0;

    for (float t = t0; t <= t1 && nEst < VZ_RING; t += dt)
    {
        while (tick + 2 < n && p[tick + 1].t <= t) tick++;
        float span = p[tick + 1].t - p[tick].t;
        float u = span > 1e-6f ? (t - p[tick].t) / span : 0.0f;
        if (u < 0.0f) u = 0.0f;
        if (u > 1.0f) u = 1.0f;

        // The camera: the origin, plus an eye height, plus view bob. The
        // constant offset cancels in a difference. The bob does not, and it is
        // the thing everybody assumes is the problem -- so it is a parameter.
        float cx = p[tick].x + (p[tick + 1].x - p[tick].x) * u;
        float cy = p[tick].y + (p[tick + 1].y - p[tick].y) * u;
        float cz = p[tick].z + (p[tick + 1].z - p[tick].z) * u + 64.0f;
        if (g_bob > 0.0f)
            cz += g_bob * sinf(t * 9.4f);

        WrVelPush(&w, cx, cy, cz, dt);

        float ex, ey, ez, mx, my, mz;
        if (!WrVelEstimate(&w, VEL_WINDOW, &ex, &ey, &ez, &mx, &my, &mz))
            continue;

        float a = 1.0f - expf(-dt / VEL_TAU);
        if (!emaOn) { ema[0] = ex; ema[1] = ey; ema[2] = ez; emaOn = true; }
        else
        {
            ema[0] += (ex - ema[0]) * a;
            ema[1] += (ey - ema[1]) * a;
            ema[2] += (ez - ema[2]) * a;
        }
        vzEst[nEst] = ema[2];
        tEst[nEst] = t;

        // The feet, as the DLL computes them: the camera less a fixed 64. The
        // bob rides along, because it does in the game too.
        nearEst[nEst] = g_map ? SimNearest(g_map, cx, cy, cz - 64.0f)
                              : SIM_NEVER;
        nEst++;
    }

    for (int iw = 0; iw < NWIN; iw++)
    {
        int back = (int)(kWin[iw] * FPS + 0.5f);
        if (back < 1) back = 1;

        for (int i = back; i < nEst; i++)
        {
            float h = tEst[i] - tEst[i - back];
            if (!(h > 1e-6f)) continue;

            // The truth at the window's MIDPOINT, which is the instant a finite
            // difference actually describes -- the same argument wr_smooth.h
            // makes about pairing a velocity with a position.
            float mid = 0.5f * (tEst[i] + tEst[i - back]);
            int k = 0;
            while (k + 2 < n && p[k + 1].t <= mid) k++;
            float hh = p[k + 1].t - p[k].t;
            if (!(hh > 1e-6f)) continue;
            bool truth = WrPhaseIsContact(p[k].vz, p[k + 1].vz, hh, G);

            float az = (vzEst[i] - vzEst[i - back]) / h;
            float d = az + G;
            if (d < 0.0f) d = -d;

            for (int it = 0; it < NTOL; it++)
            {
                bool got = d >= kTol[it];
                Score &s = g_score[iw][it];
                s.total++;
                if (got == truth) s.agree++;
                else if (truth) s.falseAir++;
                else s.falseContact++;
            }

            // The veto, at the shipped tolerance only -- sweeping it against
            // four tolerances as well would be twenty tables saying one thing.
            //
            // The map's answer at the same instant the difference describes.
            // The sample grid is uniform, so the midpoint of [i-back, i] is
            // i - back/2 exactly.
            if (g_map)
            {
                const bool got = d >= kTol[SHIPPED_TOL];
                const float near_ = nearEst[i - back / 2];

                Score &plain = g_mapComplete ? g_scoreBrush[iw]
                                             : g_scoreDisp[iw];
                plain.total++;
                if (got == truth) plain.agree++;
                else if (truth) plain.falseAir++;
                else plain.falseContact++;

                for (int ir = 0; ir < NRAD; ir++)
                {
                    const bool vetoed = (got && near_ > kRad[ir]) ? false : got;
                    Score &veto = g_mapComplete ? g_vetoBrush[iw][ir]
                                                : g_vetoDisp[iw][ir];
                    veto.total++;
                    if (vetoed == truth) veto.agree++;
                    else if (truth) veto.falseAir++;
                    else veto.falseContact++;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------

static bool g_live = false;
static int g_cap = 0;
static int g_seen = 0;

// The map for a paths\<map>\ directory. Absent is an ordinary outcome -- a
// library holds runs for maps that are not installed -- and those runs are still
// simulated, just without a veto to compare against.
static bool LoadMapFor(const char *name, WrBspMap *out)
{
    if (!g_mapsDir)
        return false;
    char path[1024];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s.bsp", g_mapsDir, name);

    WrBspRaw raw;
    char err[256];
    if (!WrBspReadRaw(path, &raw, err, sizeof(err)))
    {
        g_mapsMissing++;
        return false;
    }
    bool ok = WrBspBuild(&raw, out, err, sizeof(err));
    WrBspFreeRaw(&raw);
    if (!ok)
    {
        g_mapsMissing++;
        return false;
    }
    g_mapsLoaded++;
    if (out->hasDisplacements)
        g_mapsDisp++;
    return true;
}

static void Walk(const char *dir)
{
    char pat[1024];
    _snprintf_s(pat, sizeof(pat), _TRUNCATE, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (g_cap && g_seen >= g_cap) break;
        if (fd.cFileName[0] == '.') continue;
        char full[1024];
        _snprintf_s(full, sizeof(full), _TRUNCATE, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            // One load per map directory, not per run. Saved and restored
            // rather than assigned, so a nested directory cannot leave the
            // wrong level's geometry behind it.
            WrBspMap *outerMap = g_map;
            const bool outerComplete = g_mapComplete;

            WrBspMap m;
            const bool got = g_live && LoadMapFor(fd.cFileName, &m);
            if (got)
            {
                g_map = &m;
                // THE SHIPPED PREDICATE, not a copy of it. This was a private
                // re-implementation and it was stale two ways: it refused every
                // displacement map rather than every partly-built one, and it
                // left out entBrushes, which the shipped coverage test adds.
                //
                // WHICH MATTERS FOR THE NUMBERS QUOTED OFF THIS TOOL. The
                // 92.2 / 73.8 / 98.3 in wr_energy.cpp and the 83.2 against 93.6
                // in wr_bspload.h were all measured through the line this
                // replaces -- that is, through a gate that was not the gate that
                // shipped. Re-running this sweep is what makes them true again;
                // until then they describe the old gate and say so.
                g_mapComplete = WrBspGeometryComplete(&m);
            }

            Walk(full);

            if (got)
                WrBspFreeMap(&m);
            g_map = outerMap;
            g_mapComplete = outerComplete;
            continue;
        }
        size_t L = strlen(fd.cFileName);
        if (L < 8 || strcmp(fd.cFileName + L - 7, ".wrpath") != 0) continue;
        // surf only: the phase split is a claim about surf maps, and a bhop
        // library would dilute it with a completely different kind of movement.
        if (strstr(dir, "surf_") == 0) continue;
        int n = 0;
        Pt *p = Load(full, &n);
        if (p)
        {
            if (g_live) SimRun(p, n); else SweepRun(p, n);
            free(p);
            g_seen++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

int main(int argc, char **argv)
{
    const char *root = "wrlines_data\\paths";
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--live") == 0) { g_live = true; g_cap = 250; }
        else if (strcmp(argv[i], "--bob") == 0 && i + 1 < argc) g_bob = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--maps") == 0 && i + 1 < argc) g_mapsDir = argv[++i];
        else if (strcmp(argv[i], "--no-clip") == 0) g_wrBspIncludeClip = false;
        else root = argv[i];
    }

    if (!g_live)
    {
        printf("\n=== the corpus, with exact velocities ===\n\n");
        Walk(root);
        if (!g_runs)
        {
            printf("no runs found under %s\n"
                   "this needs a real .wrpath library; see the header.\n\n", root);
            return 1;
        }

        long long tot = g_air + g_contact;
        printf("runs %d   ticks %lld   teleports rejected %lld\n", g_runs, tot, g_tele);
        printf("air %.1f%%   contact %.1f%%\n", 100.0 * g_air / tot,
               100.0 * g_contact / tot);

        if (g_angN)
        {
            qsort(g_ang, g_angN, sizeof(float), CmpF);
            int steep = 0;
            for (int i = 0; i < g_angN; i++)
                if (g_ang[i] > 45.57f) steep++;
            printf("\nsustained-contact segments fitted: %d\n", g_angN);
            printf("  ramp angle  p10 %.1f  p50 %.1f  p90 %.1f deg\n",
                   g_ang[g_angN / 10], g_ang[g_angN / 2], g_ang[g_angN * 9 / 10]);
            printf("  steeper than Source's standable cut: %.1f%%\n",
                   100.0 * steep / g_angN);
        }

        printf("\nboards %d  (%.1f per run)\n", g_boards, (double)g_boards / g_runs);
        if (g_boards)
        {
            printf("  ");
            for (int i = 0; i < WR_GRADE_COUNT; i++)
                printf("%s %.0f%%  ", WrPhaseGradeName((unsigned char)i),
                       100.0 * g_grades[i] / g_boards);
            printf("\n");
        }

        if (g_lossN)
        {
            printf("  differenced loss that rounds to \"-0\": %.1f%%   "
                   "(projected: %.1f%%)\n",
                   100.0 * g_lossZero / g_lossN,
                   100.0 * g_lossProjZero / g_lossN);
            const char *nm[4] = { "measured ", "wide     ", "clean    ",
                                  "projected" };
            float *arr[4] = { g_lossM, g_lossW, g_lossC, g_lossI };
            for (int k = 0; k < 4; k++)
            {
                qsort(arr[k], g_lossN, sizeof(float), CmpF);
                printf("  loss %s  p10 %6.1f  p50 %6.1f  p90 %6.1f u/s\n",
                       nm[k], arr[k][g_lossN / 10], arr[k][g_lossN / 2],
                       arr[k][g_lossN * 9 / 10]);
            }

            printf("  grades on the clean pair:\n    ");
            for (int i = 0; i < WR_GRADE_COUNT; i++)
                printf("%s %.0f%%  ", WrPhaseGradeName((unsigned char)i),
                       100.0 * g_gradesClean[i] / g_boards);
            printf("\n");

            if (g_angGapN)
            {
                qsort(g_angGap, g_angGapN, sizeof(float), CmpF);
                printf("  approach angle moved  p50 %.2f  p90 %.2f deg\n",
                       g_angGap[g_angGapN / 2], g_angGap[g_angGapN * 9 / 10]);
            }

            if (g_bGapN)
            {
                qsort(g_bGap, g_bGapN, sizeof(float), CmpF);
                printf("  gap between boards on one run  p50 %.2f  p90 %.2f  "
                       "p95 %.2f  p99 %.2f s   (n %d)\n",
                       g_bGap[g_bGapN / 2], g_bGap[g_bGapN * 9 / 10],
                       g_bGap[g_bGapN * 95 / 100], g_bGap[g_bGapN * 99 / 100],
                       g_bGapN);
            }
        }

        {
            long long tot2 = 0;
            for (int i = 0; i < QBINS; i++) tot2 += g_qN[i];
            if (tot2)
            {
                printf("\nstrafe quality against %lld airborne ticks"
                       "  (air guard %d, window %d)\n",
                       tot2, Q_AIR_GUARD, Q_WIN);
                printf("     r     per tick    over %2d    1-(1-r)^2\n", Q_WIN);
                for (int i = 0; i < QBINS; i++)
                {
                    if (g_qN[i] < 500) continue;    // too few to mean anything
                    const double r = (i + 0.5) / (QBINS / 2.0);
                    const double pred = 1.0 - (1.0 - r) * (1.0 - r);
                    printf("   %4.2f     %+6.3f", r, g_qSum[i] / g_qN[i]);
                    if (g_qwN[i] >= 500)
                        printf("     %+6.3f", g_qwSum[i] / g_qwN[i]);
                    else
                        printf("         --");
                    printf("      %+6.3f\n", pred);
                }
            }
        }

        {
            if (g_trendN)
            {
                qsort(g_trendA, g_trendN, sizeof(float), CmpF);
                qsort(g_trendB, g_trendN, sizeof(float), CmpF);
                printf("  speed change per tick with no clip in it:\n");
                printf("    in the air just before  p50 %+6.1f u/s\n",
                       g_trendA[g_trendN / 2]);
                printf("    on the ramp just after  p50 %+6.1f u/s\n",
                       g_trendB[g_trendN / 2]);
            }
        }
        printf("\n");
        return 0;
    }

    printf("\n=== live, through the real estimator ===\n\n");
    printf("view bob %.1f units, %.0f fps, velocity window %.3f s, tau %.3f s\n",
           g_bob, FPS, VEL_WINDOW, VEL_TAU);
    Walk(root);
    if (!g_simRuns)
    {
        printf("no runs found under %s\n"
               "this needs a real .wrpath library; see the header.\n\n", root);
        return 1;
    }
    printf("runs simulated: %d\n\n", g_simRuns);

    printf("%-8s", "window");
    for (int it = 0; it < NTOL; it++) printf("  tol %-19.0f", kTol[it]);
    printf("\n");
    for (int iw = 0; iw < NWIN; iw++)
    {
        printf("%-8.2f", kWin[iw]);
        for (int it = 0; it < NTOL; it++)
        {
            Score &s = g_score[iw][it];
            if (!s.total) { printf("  %-23s", "-"); continue; }
            printf("  %5.1f%% (miss%4.1f fake%4.1f)",
                   100.0 * s.agree / s.total,
                   100.0 * s.falseAir / s.total,
                   100.0 * s.falseContact / s.total);
        }
        printf("\n");
    }
    if (g_mapsDir)
    {
        printf("\n--- and with the map's own geometry allowed to veto ---\n");
        printf("%d maps read, %d not installed or refused, %d of the read use "
               "displacements\n", g_mapsLoaded, g_mapsMissing, g_mapsDisp);
        printf("playerclip brushes: %s\n\n",
               g_wrBspIncludeClip ? "included (the default)"
                                  : "SKIPPED (--no-clip)");

        printf("at the shipped tolerance of %.0f, vetoed when the nearest map\n"
               "surface is further than R units from the feet:\n\n",
               kTol[SHIPPED_TOL]);

        struct { const char *title; Score *plain; Score (*veto)[NRAD]; }
        pop[2] = {
            { "BRUSH-ONLY maps -- where absence is evidence",
              g_scoreBrush, g_vetoBrush },
            { "DISPLACEMENT maps -- where it is not, shown to say why",
              g_scoreDisp, g_vetoDisp },
        };

        for (int q = 0; q < 2; q++)
        {
            if (!pop[q].plain[1].total)
                continue;
            printf("  %s\n", pop[q].title);
            printf("  %-7s  %-24s", "window", "as shipped");
            for (int ir = 0; ir < NRAD; ir++)
                printf("  R=%-22.0f", kRad[ir]);
            printf("\n");
            for (int iw = 0; iw < NWIN; iw++)
            {
                Score &a = pop[q].plain[iw];
                if (!a.total) continue;
                printf("  %-7.2f  %5.1f%% (miss%4.1f fake%4.1f)", kWin[iw],
                       100.0 * a.agree / a.total,
                       100.0 * a.falseAir / a.total,
                       100.0 * a.falseContact / a.total);
                for (int ir = 0; ir < NRAD; ir++)
                {
                    Score &b = pop[q].veto[iw][ir];
                    printf("  %5.1f%% (miss%4.1f fake%4.1f)",
                           100.0 * b.agree / b.total,
                           100.0 * b.falseAir / b.total,
                           100.0 * b.falseContact / b.total);
                }
                printf("\n");
            }
            printf("\n");
        }
    }

    printf("\n  miss = it was contact and this said air   (a ramp gone unseen)\n");
    printf("  fake = it was air and this said contact   (a surface invented)\n");
    printf("\nthe shipped pair is window %.2f, tol %.0f -- see WrEnergyPhase.\n\n",
           0.10f, 250.0f);
    return 0;
}
