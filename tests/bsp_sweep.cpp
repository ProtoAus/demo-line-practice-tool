// bsp_sweep.cpp  --  the BSP reader, run over every map on the machine.
//
// Every number in wr_bsp.h's header came from this program, the same way every
// number in wr_phase.h's came from phase_sweep.cpp. It is committed so they can
// be checked rather than believed.
//
// It is NOT part of the harness run. It needs a game install -- 1,304 maps and
// about 2.5 GB of other people's work, which is not in this repository, for the
// same reason tests\parity.ps1 is a separate script. tests\test_bsp.exe is the
// part that runs everywhere, on a synthetic map whose answers are known by
// hand. Built by tests\build.bat so it cannot rot: a change to the reader that
// breaks the tool that measures the reader fails the build.
//
//   tests\bsp_sweep.exe [maps dir] [--verbose] [--limit N] [--only prefix]
//                       [--closure] [--angles] [--rays N] [--verify-normals]
//
// WHAT IT IS ACTUALLY ASKING
//
// Not "does it parse" -- a wrong struct stride parses. The questions are the
// ones that have an answer the file itself can be held to:
//
//   Does every index resolve? A stride that is wrong by even four bytes turns
//   the whole of a lump into a shifted reading of itself, and the very first
//   thing that shows up is an index pointing at nothing. Across the library
//   that is over a hundred million chances to be caught.
//
//   Does the worldspawn tree reach a sensible fraction of the brushes? A tree
//   walked through the wrong field offsets terminates immediately and reports
//   almost nothing owned, which looks exactly like a map with no world geometry
//   rather than like a bug.
//
//   And --verify-normals: does the plane read out of the file agree with the
//   plane recovered from a player's velocity, which shares no code and no
//   input with it? That one is the whole reason both layers exist. What it
//   found, over 39 maps and 2,294 runs:
//
//     AT A BOARD -- arriving out of free flight, so exactly one surface is
//     involved -- the two agree to a median of 1.19 degrees, 91.4% within 15,
//     and only 1.0% disagree grossly. Slope alone, which is the number
//     anything actually prints, is a median of 0.64 degrees.
//
//     MID-RIDE the same estimator is a median of 4.91 and 12.0% gross. That
//     is not the reader: it is corners and seams, where two surfaces push at
//     once and the recovered normal is their sum -- a question with no single
//     answer rather than a wrong answer.
//
//     THE SIGNED MEDIAN IS +0.00 DEGREES in both. No bias anywhere, which is
//     the strongest single statement here: two independent measurements of
//     the same planes, centred on each other.
//
// The default maps directory is momentum\maps, so this can be run from a game
// install root with no arguments.

#include "wr_bsp.h"
#include "wr_phase.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <math.h>

static bool g_verbose = false;

// The map the world owns least of. Worth naming rather than counting: the
// panel has to say something honest on a map where model 0 holds almost
// nothing, and knowing how bad that gets is what decides the wording.
static float g_worstPct = 1e9f;
static char  g_worstMap[128] = "none";

// The map that costs the most to keep resident, and how many maps had a face
// that did not close. Both are budget questions rather than curiosities: the
// first sets WR_BSP_MAX_RESIDENT, and the second is the only signal a struct
// stride is wrong in a way every index check passed.
static float g_biggestBytes = 0.0f;
static char  g_biggestMap[128] = "none";
static int   g_unclosedMaps = 0;

// ---------------------------------------------------------------------------
// Small tallies
// ---------------------------------------------------------------------------

struct Hist
{
    int key[16];
    int n[16];
    int used;
};

static void HistAdd(Hist *h, int key)
{
    for (int i = 0; i < h->used; i++)
        if (h->key[i] == key) { h->n[i]++; return; }
    if (h->used < 16)
    {
        h->key[h->used] = key;
        h->n[h->used] = 1;
        h->used++;
    }
}

static void HistPrint(const Hist *h, const char *label, int total)
{
    for (int i = 0; i < h->used; i++)
        for (int j = i + 1; j < h->used; j++)
            if (h->n[j] > h->n[i])
            {
                int tk = h->key[i], tn = h->n[i];
                ((Hist *)h)->key[i] = h->key[j]; ((Hist *)h)->n[i] = h->n[j];
                ((Hist *)h)->key[j] = tk;        ((Hist *)h)->n[j] = tn;
            }
    printf("%s\n", label);
    for (int i = 0; i < h->used; i++)
        printf("    %-6d %5d  %5.1f%%\n", h->key[i], h->n[i],
               total ? 100.0 * h->n[i] / total : 0.0);
}

// Distinct refusal messages, with the first map that produced each. The
// MESSAGE is the interesting output: a sweep that says "18 maps failed" is
// telling you nothing, and one that says "18 maps said LEAFS lump version 3 is
// not one this reads" is telling you what to go and measure.
#define MAX_REASONS 32
static char g_reason[MAX_REASONS][192];
static char g_reasonMap[MAX_REASONS][96];
static int  g_reasonN[MAX_REASONS];
static int  g_reasons = 0;

static void NoteReason(const char *msg, const char *map)
{
    for (int i = 0; i < g_reasons; i++)
        if (strcmp(g_reason[i], msg) == 0) { g_reasonN[i]++; return; }
    if (g_reasons >= MAX_REASONS)
        return;
    _snprintf_s(g_reason[g_reasons], sizeof(g_reason[0]), _TRUNCATE, "%s", msg);
    _snprintf_s(g_reasonMap[g_reasons], sizeof(g_reasonMap[0]), _TRUNCATE,
                "%s", map);
    g_reasonN[g_reasons] = 1;
    g_reasons++;
}

static int CmpF(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static float Pct(float *v, int n, double p)
{
    if (n <= 0)
        return 0.0f;
    int i = (int)(p * n);
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return v[i];
}

// ---------------------------------------------------------------------------

// Area-weighted, one bin a degree, over every UP-FACING face. Area-weighted
// and not face-counted, because a surf map is a handful of enormous ramps and
// several thousand small brushes, and counting faces measures the brushes.
static double g_angle[91];
static double g_angleArea = 0.0;

// ---------------------------------------------------------------------------
// The grid, against the thing it accelerates
// ---------------------------------------------------------------------------
//
// tests\test_bsp.exe already compares them, on a fixture whose grid is three
// cells by three by one. That is enough to catch an arithmetic mistake and not
// nearly enough to catch a WALK mistake: a DDA that skips a cell under some
// direction sign, or stops a step early at a boundary, needs a grid with
// somewhere to go wrong in. Real maps have grids of thousands of cells, and
// the answer is still checkable, because brute force does not care how many
// polygons there are -- only how long it takes.

// ---------------------------------------------------------------------------
// The file's normal against the one the velocity trace recovers
// ---------------------------------------------------------------------------
//
// THIS IS THE ONE CHECK NEITHER LAYER COULD HAVE DONE ALONE, and it is the
// reason both of them exist rather than one.
//
// wr_phase.h recovers the surface a player DID touch, from the fact that free
// flight has vertical acceleration exactly -g and a surface changes it. It
// needs no map. wr_bsp.cpp reads the planes out of the file. They are two
// completely independent measurements of the same plane -- different inputs,
// different arithmetic, no shared code between them -- so where they agree,
// both are probably right, and where they disagree, at least one is wrong and
// the size of the disagreement says which is worth looking at.
//
// WHAT WOULD MAKE THIS DISAGREE FOR AN HONEST REASON
//
// Plenty, and it has to be subtracted rather than argued away:
//
//   The ramp is a DISPLACEMENT. Then there is no brush face under the player
//   at all and the nearest one is some unrelated wall. Over half of maps have
//   displacements, so this is the big one.
//   The ramp is a func_*. Same outcome, different cause -- 4.7% of brushes.
//   The player was on a PLAYERCLIP that is not solid. 141,841 of those exist
//   and none of them are read.
//   The .wrpath is not for the map file we have. A map gets updated.
//
// All four produce the same symptom: a face is found, but it is not the face
// the player was on. The filter is the SAME distance test in every case -- if
// the nearest polygon is not within a player hull's reach of the recorded
// origin, the comparison is thrown away and counted, not scored. What is left
// is segments where a brush face really was underfoot, which is the only
// population the question makes sense over.

// The player origin is at the bottom centre of a 32x32x72 hull, so on a ramp
// the surface can be up to about half the hull's width away from the point the
// demo recorded. 48 gives that room and not much more.
#define VN_MAX_FACE_DIST 48.0f

// A contact segment shorter than this is not fitted. The fit needs velocities
// that span the plane rather than pointing one way, and a two-tick clip does
// not have them -- this is the same 8 the plan's measurement used.
#define VN_MIN_SEGMENT 8

// ONLY THE SURF BAND, and this is a correctness bound rather than a choice of
// what is interesting.
//
// The kinematic recovery assumes the only force other than gravity is the
// surface's normal force. On a ramp too steep to stand on that is true: Source
// applies neither friction nor ground acceleration there, which is exactly what
// makes it a surf ramp. On GROUND both of those act, they act horizontally, and
// they land in the same (a - g) this recovers the normal from -- so a ground
// segment produces a normal tilted by whatever the player was doing with the
// keyboard, and comparing it against the floor's true (0,0,1) measures
// friction, not the reader.
//
// Measured, and this is why the bound moved: scoring every contact segment with
// a recovered normal.z under 0.95 gave a median disagreement of 8.10 degrees
// and put 16.7% of segments over 30. Restricted to the band it is the figure
// printed at the end of this file's run. The first number was not the reader
// being wrong; it was the question being asked where it has no answer.
#define VN_MIN_NZ WR_BSP_BAND_LO
#define VN_MAX_NZ WR_BSP_BAND_HI

static int    g_vnFiles = 0, g_vnSegments = 0, g_vnScored = 0;
static int    g_vnNoFace = 0, g_vnTooFar = 0, g_vnNoFit = 0, g_vnOffBand = 0;
static float *g_vnErr = NULL;
static int    g_vnErrN = 0, g_vnErrCap = 0;
static int    g_vnMapsWithPaths = 0;

#define VN_BUCKET_CAP 40000
static float g_vnBucket[3][VN_BUCKET_CAP];
static int   g_vnBucketN[3] = { 0, 0, 0 };

// The second population: SINGLE CLIPS, where the impulse is large.
//
// The fit and the single clip are two different estimators of the same normal
// and they fail in opposite conditions, which is why both are measured here
// rather than one being chosen in advance.
//
// WrPhaseFitNormal builds the normal from cross products of the velocities in
// a sustained ride, on the argument that every one of them lies IN the plane.
// True, and ill-conditioned exactly where it is being used: a player riding a
// ramp turns slowly, so consecutive velocities are nearly collinear and their
// cross product amplifies any out-of-plane error by 1/sin of a small angle.
// wr_phase.h says as much about the eigenvector method it replaced, and the
// cross-product version inherits the same conditioning.
//
// WrPhaseNormal takes one tick's (a - g) directly. That is noise-dominated on
// a gentle ride, where the normal force is small -- and it is the well-behaved
// one at a BOARD, where a player arriving at 3,000 u/s has the whole of that
// deflection in a single tick.
//
// So: sustained rides go to the fit, hard clips go to the single tick, and the
// file says which of them was worth believing.
#define VN_CLIP_MIN_IMPULSE 300.0f      // u/s of velocity change in one tick

// TWO POPULATIONS, AND THE DIFFERENCE BETWEEN THEM IS THE RESULT.
//
// [0] is every tick whose impulse clears the threshold, wherever it happens.
// [1] is the subset that arrives out of free flight -- three clear airborne
// ticks and then this one, which is the rule wr_phase.h's board detector uses.
//
// They are both kept because the contrast is the finding rather than a detail
// of how it was reached. A hard tick in the middle of a ride can be a wall
// graze, a seam between two brushes, or a corner where two surfaces push at
// once -- and in a corner the recovered normal is the SUM of two normal forces,
// which is not an error, it is a question with no single answer. Arriving out
// of the air, there is exactly one surface involved.
enum { VC_ANY = 0, VC_BOARD, VC_POPS };
static const char *kVcName[VC_POPS] = {
    "any hard tick, wherever it happened",
    "arriving out of free flight -- a board"
};

struct VcPop
{
    float *err; int errN, errCap;
    int tried, noFace;
    float signed_[VN_BUCKET_CAP]; int signedN;
    float slope[VN_BUCKET_CAP];   int slopeN;
    float head[VN_BUCKET_CAP];    int headN;
};
static VcPop g_vc[VC_POPS];

// WHICH FACE WAS THE PLAYER ON. Straight down, not nearest.
//
// Nearest-polygon was the first answer and it is not good enough, which the
// numbers said clearly: with it, both estimators put about 25% of readings
// within a degree and then scattered -- a bimodal shape, which is what "we
// found the right face sometimes" looks like, and not what a noisy estimator
// looks like. The cause is geometric. The recorded position is the player
// ORIGIN, at the bottom centre of a 32-wide hull, so on a ramp it sits about a
// half-width off the surface -- and at that range the ramp's own side faces
// and the brushes next to it are all comparably near.
//
// A ray straight down finds the surface a player is standing over, which is
// the thing the question is about. And it is straight DOWN rather than along
// the recovered normal on purpose: casting along -n would preferentially find
// faces whose normal is near +n, which is the answer this is supposed to be
// checking. The comparison has to be blind to the thing being compared.
//
// Started slightly above the origin because on a slanted surface the hull rests
// on a corner and the origin can be a little inside the brush.
static bool VnFaceUnder(const WrBspMap *m, const float at[3], int *polyOut,
                        float *dropOut)
{
    const float start[3] = { at[0], at[1], at[2] + 8.0f };
    const float down[3] = { 0.0f, 0.0f, -1.0f };
    float t = 0.0f;
    if (!WrBspTraceRay(m, start, down, 96.0f, polyOut, &t))
        return false;
    if (dropOut) *dropOut = t - 8.0f;
    return true;
}

static void VcPush(VcPop *q, float e)
{
    if (q->errN >= q->errCap)
    {
        int cap = q->errCap ? q->errCap * 2 : 4096;
        float *p = (float *)realloc(q->err, (size_t)cap * sizeof(float));
        if (!p)
            return;
        q->err = p;
        q->errCap = cap;
    }
    q->err[q->errN++] = e;
}

// The worst map, by median error, so a systematic failure has a name rather
// than being averaged into the tail.
static float g_vnWorstMedian = -1.0f;
static char  g_vnWorstMap[128] = "none";

static void VnPush(float e)
{
    if (g_vnErrN >= g_vnErrCap)
    {
        int cap = g_vnErrCap ? g_vnErrCap * 2 : 4096;
        float *p = (float *)realloc(g_vnErr, (size_t)cap * sizeof(float));
        if (!p)
            return;
        g_vnErr = p;
        g_vnErrCap = cap;
    }
    g_vnErr[g_vnErrN++] = e;
}

// One .wrpath, read straight rather than through wr_path.cpp.
//
// Deliberately a second reader. Linking the real loader would drag wr_dp,
// wr_extract, wr_profile and the engine layer in behind it, and this needs
// three fields and an array of floats. The format is stated in wr_path.h and
// in wr_path.cpp's writer, and the offsets below are from there.
static bool VnLoadPath(const char *path, float **ptsOut, int *nOut, float *hOut)
{
    FILE *f = NULL;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return false;

    unsigned char hdr[0x100];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
    {
        fclose(f);
        return false;
    }
    unsigned int count = 0;
    float tick = 0.0f;
    memcpy(&count, hdr + 0x10, 4);
    memcpy(&tick, hdr + 0x18, 4);
    if (count < 2 || count > 4000000u || !(tick > 1e-4f) || !(tick < 1.0f))
    {
        fclose(f);
        return false;
    }

    // 28 bytes a point: x y z vx vy vz t, all float32.
    float *pts = (float *)malloc((size_t)count * 7 * sizeof(float));
    if (!pts)
    {
        fclose(f);
        return false;
    }
    if (fread(pts, 28, count, f) != count)
    {
        free(pts);
        fclose(f);
        return false;
    }
    fclose(f);

    *ptsOut = pts;
    *nOut = (int)count;
    *hOut = tick;
    return true;
}

// Every tick whose velocity change is big enough for the normal force to be
// the only thing in it, scored one tick at a time. Independent of the segment
// walk below -- a hard clip is a single tick and does not need a ride around
// it.
static void VcCheckRun(const WrBspMap *m, const float *p, int n, float h)
{
    for (int i = 0; i + 1 < n; i++)
    {
        const float *a = p + 7 * i, *b = p + 7 * (i + 1);

        float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
        float step = sqrtf(dx * dx + dy * dy + dz * dz);
        float sp = sqrtf(a[3] * a[3] + a[4] * a[4] + a[5] * a[5]);
        if (WrPhaseIsTeleport(step, sp, h))
            continue;

        // (a - g) itself, which is the impulse the surface delivered. Its
        // magnitude is what decides whether this tick is worth reading -- the
        // whole point of the threshold is that the normal force has to be
        // large compared with everything else in the same difference.
        float cx = b[3] - a[3];
        float cy = b[4] - a[4];
        float cz = (b[5] - a[5]) + 800.0f * h;
        float imp = sqrtf(cx * cx + cy * cy + cz * cz);
        if (imp < VN_CLIP_MIN_IMPULSE)
            continue;

        // Did this tick arrive out of free flight? Three clear airborne ticks
        // before it, which is the rule wr_phase.h's board detector uses.
        bool board = false;
        if (i >= 3)
        {
            board = true;
            for (int k = i - 3; k < i && board; k++)
                if (WrPhaseIsContact(p[7 * k + 5], p[7 * (k + 1) + 5], h,
                                     800.0f))
                    board = false;
        }

        float nrm[3];
        if (!WrPhaseNormal(a + 3, b + 3, h, 800.0f, nrm))
            continue;
        if (nrm[2] < VN_MIN_NZ || nrm[2] > VN_MAX_NZ)
            continue;

        // The position AFTER the clip. The clip happens during the tick, so
        // the second sample is the one standing on the surface -- the first is
        // still in the air on its way to it.
        const float at[3] = { b[0], b[1], b[2] };
        int poly = -1;
        float drop = 0.0f;
        bool hit = VnFaceUnder(m, at, &poly, &drop);

        for (int q = 0; q < VC_POPS; q++)
        {
            if (q == VC_BOARD && !board)
                continue;
            VcPop *P = &g_vc[q];
            P->tried++;
            if (!hit)
            {
                P->noFace++;
                continue;
            }

            const float *fp = m->polys[poly].plane;
            VcPush(P, WrBspAngleBetween(nrm, fp));

            // SIGNED, which the angle between two vectors cannot be. An
            // unsigned 5 degrees is two completely different findings
            // depending on whether it is scatter about zero or a consistent
            // lean one way: scatter is a noisy estimator, and a lean is
            // something systematically missing from the model. Comparing the
            // two SURFACE angles rather than the vectors is what gives it a
            // sign at all.
            float sg = WrBspSurfaceAngle(nrm) - WrBspSurfaceAngle(fp);
            if (P->signedN < VN_BUCKET_CAP)
                P->signed_[P->signedN++] = sg;

            // SLOPE AND HEADING ARE NOT THE SAME QUESTION, and the angle
            // between two normals contains both.
            //
            // Slope is how steep the ramp is -- the number the panel prints,
            // and the number that decides whether a face is in the surf band
            // at all. Heading is which way it faces around the vertical, which
            // nothing in this tool ever shows. They are perturbed by different
            // things, so reporting them together was hiding the answer.
            if (P->slopeN < VN_BUCKET_CAP)
                P->slope[P->slopeN++] = (sg < 0.0f) ? -sg : sg;

            float ah = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1]);
            float bh = sqrtf(fp[0] * fp[0] + fp[1] * fp[1]);
            if (ah > 1e-3f && bh > 1e-3f && P->headN < VN_BUCKET_CAP)
            {
                float c = (nrm[0] * fp[0] + nrm[1] * fp[1]) / (ah * bh);
                if (c > 1.0f) c = 1.0f;
                if (c < -1.0f) c = -1.0f;
                P->head[P->headN++] = (float)(acos(c) * 57.29577951308232);
            }
        }
    }
}

static void VnCheckRun(const WrBspMap *m, const float *p, int n, float h)
{
    // Sustained contact, found the same way the board detector finds one: a
    // run of consecutive ticks whose vertical acceleration is not -g.
    int i = 0;
    while (i + 1 < n)
    {
        // Skip to the start of a contact run, rejecting teleports as we go.
        while (i + 1 < n)
        {
            const float *a = p + 7 * i, *b = p + 7 * (i + 1);
            float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
            float step = sqrtf(dx * dx + dy * dy + dz * dz);
            float sp = sqrtf(a[3] * a[3] + a[4] * a[4] + a[5] * a[5]);
            if (!WrPhaseIsTeleport(step, sp, h) &&
                WrPhaseIsContact(a[5], b[5], h, 800.0f))
                break;
            i++;
        }
        if (i + 1 >= n)
            return;

        int start = i;
        while (i + 1 < n)
        {
            const float *a = p + 7 * i, *b = p + 7 * (i + 1);
            float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
            float step = sqrtf(dx * dx + dy * dy + dz * dz);
            float sp = sqrtf(a[3] * a[3] + a[4] * a[4] + a[5] * a[5]);
            if (WrPhaseIsTeleport(step, sp, h) ||
                !WrPhaseIsContact(a[5], b[5], h, 800.0f))
                break;
            i++;
        }
        int len = i - start + 1;
        if (len < VN_MIN_SEGMENT)
            continue;

        g_vnSegments++;

        // The fit wants the velocities themselves, packed. They are already
        // three consecutive floats inside a seven-float record, so this is a
        // gather rather than a cast.
        static float vel[WR_PHASE_FIT_MAX * 3];
        int take = len;
        int stride = 1;
        if (take > WR_PHASE_FIT_MAX)
        {
            stride = (take + WR_PHASE_FIT_MAX - 1) / WR_PHASE_FIT_MAX;
            take = 0;
            for (int k = start; k <= i && take < WR_PHASE_FIT_MAX; k += stride)
            {
                vel[3 * take + 0] = p[7 * k + 3];
                vel[3 * take + 1] = p[7 * k + 4];
                vel[3 * take + 2] = p[7 * k + 5];
                take++;
            }
        }
        else
        {
            for (int k = 0; k < len; k++)
            {
                vel[3 * k + 0] = p[7 * (start + k) + 3];
                vel[3 * k + 1] = p[7 * (start + k) + 4];
                vel[3 * k + 2] = p[7 * (start + k) + 5];
            }
        }

        float nrm[3];
        if (!WrPhaseFitNormal(vel, take, nrm))
        {
            g_vnNoFit++;
            continue;
        }
        if (nrm[2] < VN_MIN_NZ || nrm[2] > VN_MAX_NZ)
            continue;       // ground or a wall; see VN_MIN_NZ for why not

        // The middle of the ride, which is the position the fitted plane is
        // most about. The ends of a segment are where the player is arriving
        // and leaving, and are the least representative points on it.
        const float *mid = p + 7 * ((start + i) / 2);
        const float at[3] = { mid[0], mid[1], mid[2] };

        int poly = -1;
        float dist = 0.0f;
        if (!VnFaceUnder(m, at, &poly, &dist))
        {
            g_vnNoFace++;
            continue;
        }
        if (dist > VN_MAX_FACE_DIST)
        {
            g_vnTooFar++;
            continue;
        }

        // Is the face this landed on itself a surf face? When it is not, the
        // nearest polygon is something the player was beside rather than on --
        // a wall at the edge of the ramp, or the ramp's own end cap. Counted
        // rather than filtered, because filtering on the answer is how a check
        // is made to pass.
        if (!WrBspIsSurfBand(m->polys[poly].plane[2]))
            g_vnOffBand++;

        float e = WrBspAngleBetween(nrm, m->polys[poly].plane);
        VnPush(e);
        g_vnScored++;

        // The same error, bucketed by how far the matched face actually was.
        //
        // This is the diagnostic that separates the two ways this check can
        // fail, which otherwise look identical in the aggregate: a reader that
        // gets angles wrong, and a search that finds the wrong face. If the
        // near bucket is tight and the far one is not, the reader is fine and
        // the far bucket is players standing NEXT to a ramp rather than on it.
        int b = (dist <= 8.0f) ? 0 : (dist <= 24.0f ? 1 : 2);
        if (g_vnBucketN[b] < VN_BUCKET_CAP)
            g_vnBucket[b][g_vnBucketN[b]++] = e;
    }
}

static void VnCheckMap(const WrBspMap *m, const char *mapName,
                       const char *pathsRoot)
{
    char pattern[1024];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\%s\\*.wrpath",
                pathsRoot, mapName);

    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE)
        return;

    g_vnMapsWithPaths++;
    int before = g_vnErrN;

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        char file[1024];
        _snprintf_s(file, sizeof(file), _TRUNCATE, "%s\\%s\\%s",
                    pathsRoot, mapName, fd.cFileName);

        float *pts = NULL;
        int n = 0;
        float h = 0.0f;
        if (!VnLoadPath(file, &pts, &n, &h))
            continue;
        g_vnFiles++;
        VnCheckRun(m, pts, n, h);
        VcCheckRun(m, pts, n, h);
        free(pts);
    } while (FindNextFileA(find, &fd));
    FindClose(find);

    // This map's own median, so a map that is entirely displacement-built --
    // where every comparison that survived the distance filter is against the
    // wrong face -- can be named instead of being buried in the aggregate.
    int got = g_vnErrN - before;
    if (got >= 20)
    {
        float *tmp = (float *)malloc((size_t)got * sizeof(float));
        if (tmp)
        {
            memcpy(tmp, g_vnErr + before, (size_t)got * sizeof(float));
            qsort(tmp, got, sizeof(float), CmpF);
            float med = tmp[got / 2];
            if (med > g_vnWorstMedian)
            {
                g_vnWorstMedian = med;
                _snprintf_s(g_vnWorstMap, sizeof(g_vnWorstMap), _TRUNCATE,
                            "%s (%d segments)", mapName, got);
            }
            free(tmp);
        }
    }
}

// ---------------------------------------------------------------------------

static unsigned int g_seed = 987654321u;
static float Rnd(float lo, float hi)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * ((g_seed >> 8) & 0xFFFFFF) / (float)0x1000000;
}

static bool BruteRay(const WrBspMap *m, const float s[3], const float d[3],
                     float maxDist, int *polyOut, float *tOut)
{
    int best = -1;
    float bestT = maxDist;
    for (int i = 0; i < m->polyCount; i++)
    {
        float t;
        if (!WrBspRayPoly(s, d, m->polys[i].plane, m->verts + m->polys[i].first,
                          m->polys[i].count, &t))
            continue;
        if (t < bestT) { bestT = t; best = i; }
    }
    if (best < 0)
        return false;
    if (polyOut) *polyOut = best;
    if (tOut) *tOut = bestT;
    return true;
}

static long long g_rayGrid = 0, g_rayBrute = 0;      // ticks
static long long g_rayCount = 0, g_rayHits = 0, g_rayMismatch = 0;
static char g_mismatchMap[128] = "none";

static void RayCheck(const WrBspMap *m, const char *name, int rays)
{
    if (m->polyCount <= 0)
        return;

    LARGE_INTEGER a, b, c;
    float (*S)[3] = (float (*)[3])malloc((size_t)rays * sizeof(float) * 3);
    float (*D)[3] = (float (*)[3])malloc((size_t)rays * sizeof(float) * 3);
    if (!S || !D) { free(S); free(D); return; }

    // Rays from inside the map's own box, in every direction. Starting inside
    // is what makes them interesting: a ray from outside enters the grid once
    // and the slab clip does most of the work, where one from inside exercises
    // the step.
    for (int i = 0; i < rays; i++)
    {
        for (int k = 0; k < 3; k++)
            S[i][k] = m->mins[k] + (m->maxs[k] - m->mins[k]) * Rnd(0.0f, 1.0f);
        float len;
        do {
            for (int k = 0; k < 3; k++)
                D[i][k] = Rnd(-1.0f, 1.0f);
            len = (float)sqrt(D[i][0]*D[i][0] + D[i][1]*D[i][1] + D[i][2]*D[i][2]);
        } while (len < 1e-3f);
        for (int k = 0; k < 3; k++)
            D[i][k] /= len;
    }

    const float reach = 4096.0f;

    int *pg = (int *)malloc((size_t)rays * sizeof(int));
    float *tg = (float *)malloc((size_t)rays * sizeof(float));
    unsigned char *hg = (unsigned char *)malloc((size_t)rays);

    QueryPerformanceCounter(&a);
    for (int i = 0; i < rays; i++)
    {
        pg[i] = -1; tg[i] = 0.0f;
        hg[i] = WrBspTraceRay(m, S[i], D[i], reach, &pg[i], &tg[i]) ? 1 : 0;
    }
    QueryPerformanceCounter(&b);
    for (int i = 0; i < rays; i++)
    {
        int pb = -1; float tb = 0.0f;
        bool hb = BruteRay(m, S[i], D[i], reach, &pb, &tb);
        g_rayCount++;
        if (hb) g_rayHits++;
        // The polygon index may legitimately differ where two faces meet at
        // exactly the same distance -- a floor and a wall sharing an edge --
        // so the DISTANCE is what has to agree. A skipped cell moves it.
        if ((hg[i] != 0) != hb || (hb && fabs(tg[i] - tb) > 0.05))
        {
            if (g_rayMismatch == 0)
                _snprintf_s(g_mismatchMap, sizeof(g_mismatchMap), _TRUNCATE,
                            "%s", name);
            g_rayMismatch++;
        }
    }
    QueryPerformanceCounter(&c);

    g_rayGrid  += b.QuadPart - a.QuadPart;
    g_rayBrute += c.QuadPart - b.QuadPart;

    free(S); free(D); free(pg); free(tg); free(hg);
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    const char *root = "momentum\\maps";
    const char *only = NULL;
    const char *pathsRoot = "wrlines_data\\paths";
    int limit = 0, rays = 0;
    bool build = false, angles = false, verifyNormals = false;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--verbose") == 0) g_verbose = true;
        else if (strcmp(argv[i], "--closure") == 0) build = true;
        else if (strcmp(argv[i], "--angles") == 0) { build = true; angles = true; }
        else if (strcmp(argv[i], "--rays") == 0)
        { build = true; rays = (i + 1 < argc && argv[i + 1][0] != '-')
                               ? atoi(argv[++i]) : 200; }
        else if (strcmp(argv[i], "--verify-normals") == 0)
        { build = true; verifyNormals = true; }
        else if (strcmp(argv[i], "--paths") == 0 && i + 1 < argc)
            pathsRoot = argv[++i];
        else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc)
            limit = atoi(argv[++i]);
        else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc)
            only = argv[++i];
        else root = argv[i];
    }

    char pattern[1024];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*.bsp", root);

    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE)
    {
        printf("no .bsp files under %s\n", root);
        printf("usage: bsp_sweep [maps dir] [--verbose] [--limit N]\n");
        return 2;
    }

    printf("bsp_sweep over %s\n\n", root);

    Hist versions = { { 0 }, { 0 }, 0 };
    int maps = 0, parsed = 0, walked = 0, refusedRead = 0, refusedWalk = 0;
    int compressed = 0;
    long long totalBrushes = 0, totalOwned = 0, totalBytes = 0;
    long long refs = 0;

    int built = 0, refusedBuild = 0;
    long long sideTotal = 0, sideDropped = 0, sideDegenerate = 0;
    long long sideNotClosed = 0, sideTooFar = 0, clipOnly = 0;
    long long polys = 0, verts = 0;
    double solidArea = 0.0, surfArea = 0.0;

    float *ownedPct = (float *)malloc(4096 * sizeof(float));
    int ownedN = 0;
    float *resident = (float *)malloc(4096 * sizeof(float));
    int residentN = 0;

    // What one call to WrBspLoad costs, end to end: open, decompress, walk,
    // clip, grid. This is the number the worker thread in wr_bspload.cpp is
    // sized against, so it is measured per map rather than averaged over the
    // sweep -- an average would hide the tail, and the tail is what a hitch is.
    float *loadMs = (float *)malloc(4096 * sizeof(float));
    int loadN = 0;
    float worstLoadMs = 0.0f;
    char worstLoadMap[128] = { 0 };

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (only && _strnicmp(fd.cFileName, only, strlen(only)) != 0)
            continue;
        if (limit && maps >= limit)
            break;
        maps++;

        char path[1024];
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", root, fd.cFileName);

        WrBspRaw r;
        char err[192] = { 0 };
        // Read and build only. The explicit walk below and the checks after it
        // are the sweep's own work and the worker never does them; WrBspBuild
        // walks the tree itself, so read + build IS the whole of a load.
        long long loadTicks = 0;
        LARGE_INTEGER m0, m1;
        QueryPerformanceCounter(&m0);
        bool readOk = WrBspReadRaw(path, &r, err, (int)sizeof(err));
        QueryPerformanceCounter(&m1);
        loadTicks += m1.QuadPart - m0.QuadPart;
        if (!readOk)
        {
            refusedRead++;
            NoteReason(err, fd.cFileName);
            if (g_verbose)
                printf("  [read] %-44s %s\n", fd.cFileName, err);
            continue;
        }
        parsed++;
        HistAdd(&versions, r.version);
        if (r.compressed)
            compressed++;
        totalBytes += r.totalBytes;

        const int nb = r.count[WR_BSP_L_BRUSHES];
        unsigned char *owned = (unsigned char *)malloc((size_t)nb);
        int n = 0;
        if (!WrBspWorldBrushes(&r, owned, &n, err, (int)sizeof(err)))
        {
            refusedWalk++;
            NoteReason(err, fd.cFileName);
            if (g_verbose)
                printf("  [walk] %-44s %s\n", fd.cFileName, err);
        }
        else
        {
            walked++;
            totalBrushes += nb;
            totalOwned += n;
            if (nb > 0)
            {
                float pct = 100.0f * (float)n / (float)nb;
                if (ownedN < 4096)
                    ownedPct[ownedN++] = pct;
                if (pct < g_worstPct)
                {
                    g_worstPct = pct;
                    _snprintf_s(g_worstMap, sizeof(g_worstMap), _TRUNCATE,
                                "%s (%d of %d)", fd.cFileName, n, nb);
                }
            }

            // Every index in the file, not just the ones the walk followed.
            // The walk only visits brushes the world owns; a stride that is
            // wrong shows up just as readily in the ones it does not.
            for (int i = 0; i < r.count[WR_BSP_L_BRUSHSIDES]; i++)
            {
                int pn = WrBspBrushSidePlane(&r, i);
                refs++;
                if (pn < 0 || pn >= r.count[WR_BSP_L_PLANES])
                {
                    NoteReason("a brushside references a plane that is not there",
                               fd.cFileName);
                    break;
                }
            }
        }

        free(owned);

        if (build)
        {
            WrBspMap map;
            QueryPerformanceCounter(&m0);
            bool buildOk = WrBspBuild(&r, &map, err, (int)sizeof(err));
            QueryPerformanceCounter(&m1);
            loadTicks += m1.QuadPart - m0.QuadPart;
            if (!buildOk)
            {
                refusedBuild++;
                NoteReason(err, fd.cFileName);
                if (g_verbose)
                    printf("  [build] %-43s %s\n", fd.cFileName, err);
            }
            else
            {
                built++;
                sideTotal      += map.sideTotal;
                sideDropped    += map.sideDropped;
                sideDegenerate += map.sideDegenerate;
                sideNotClosed  += map.sideNotClosed;
                sideTooFar     += map.sideTooFar;
                clipOnly       += map.brushClipOnly;
                polys          += map.polyCount;
                verts          += map.vertCount;
                solidArea      += map.solidArea;
                surfArea       += map.surfArea;
                if (residentN < 4096)
                    resident[residentN++] = (float)map.bytes / 1048576.0f;
                if ((float)map.bytes > g_biggestBytes)
                {
                    g_biggestBytes = (float)map.bytes;
                    _snprintf_s(g_biggestMap, sizeof(g_biggestMap), _TRUNCATE,
                                "%s (%d polys)", fd.cFileName, map.polyCount);
                }
                if ((map.sideNotClosed || map.sideTooFar) && g_unclosedMaps < 12)
                {
                    printf("  [closure] %-41s %d did not close, %d left the "
                           "world, of %d sides\n", fd.cFileName,
                           map.sideNotClosed, map.sideTooFar, map.sideTotal);
                    g_unclosedMaps++;
                }

                if (rays > 0)
                    RayCheck(&map, fd.cFileName, rays);

                if (verifyNormals)
                {
                    // The .wrpath directories are named after the map, with no
                    // extension.
                    char bare[128];
                    _snprintf_s(bare, sizeof(bare), _TRUNCATE, "%s",
                                fd.cFileName);
                    char *dot = strrchr(bare, '.');
                    if (dot) *dot = 0;
                    VnCheckMap(&map, bare, pathsRoot);
                }

                if (angles)
                    for (int i = 0; i < map.polyCount; i++)
                    {
                        const float nz = map.polys[i].plane[2];
                        if (nz <= 0.0f)
                            continue;       // a ceiling is not a ramp
                        int bin = (int)(WrBspSurfaceAngle(map.polys[i].plane)
                                        + 0.5f);
                        if (bin < 0) bin = 0;
                        if (bin > 90) bin = 90;
                        g_angle[bin] += map.polys[i].area;
                        g_angleArea += map.polys[i].area;
                    }

                WrBspFreeMap(&map);

                float ms = (float)(1000.0 * (double)loadTicks
                                   / (double)freq.QuadPart);
                if (loadN < 4096)
                    loadMs[loadN++] = ms;
                if (ms > worstLoadMs)
                {
                    worstLoadMs = ms;
                    _snprintf_s(worstLoadMap, sizeof(worstLoadMap), _TRUNCATE,
                                "%s", fd.cFileName);
                }
            }
        }

        WrBspFreeRaw(&r);
    } while (FindNextFileA(find, &fd));

    FindClose(find);
    QueryPerformanceCounter(&t1);
    double secs = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;

    printf("%d maps, %d parsed, %d walked, in %.1f s (%.0f ms a map)\n\n",
           maps, parsed, walked, secs, maps ? 1000.0 * secs / maps : 0.0);

    HistPrint(&versions, "BSP version", parsed);
    printf("\n    %d of %d have their collision lumps LZMA-compressed"
           " (%.1f%%)\n", compressed, parsed,
           parsed ? 100.0 * compressed / parsed : 0.0);
    printf("    %.2f MB of lumps read, %.2f MB a map\n",
           totalBytes / 1048576.0, parsed ? totalBytes / 1048576.0 / parsed : 0.0);

    printf("\nworldspawn ownership\n");
    printf("    %lld of %lld brushes owned by model 0 (%.1f%%)\n",
           totalOwned, totalBrushes,
           totalBrushes ? 100.0 * totalOwned / totalBrushes : 0.0);
    if (ownedN)
    {
        qsort(ownedPct, ownedN, sizeof(float), CmpF);
        printf("    per map: p10 %.1f%%  p50 %.1f%%  p90 %.1f%%  min %.1f%%\n",
               Pct(ownedPct, ownedN, 0.10), Pct(ownedPct, ownedN, 0.50),
               Pct(ownedPct, ownedN, 0.90), ownedPct[0]);
        printf("    least world-owned map: %.1f%%  %s\n",
               g_worstPct, g_worstMap);
    }
    printf("    %lld brushside plane references checked\n", refs);

    if (build)
    {
        printf("\nthe clip: %d maps built\n", built);
        printf("    %lld sides in, %lld polygons out\n", sideTotal, polys);
        printf("    %lld dropped as bevels or slivers (%.1f%%)\n",
               sideDropped, sideTotal ? 100.0 * sideDropped / sideTotal : 0.0);
        printf("    %lld had a plane that was not a unit normal\n",
               sideDegenerate);
        printf("    %lld had a vertex outside the world\n", sideTooFar);
        printf("    %lld FAILED THE CLOSURE ASSERTION (%.4f%%)\n",
               sideNotClosed,
               sideTotal ? 100.0 * sideNotClosed / sideTotal : 0.0);
        printf("    %d maps had a side refused for either reason\n",
               g_unclosedMaps);
        printf("    %lld solid brushes were playerclip-only and skipped\n",
               clipOnly);
        printf("    %.0f sq units solid, %.0f in the surf band (%.1f%%)\n",
               solidArea, surfArea, solidArea ? 100.0 * surfArea / solidArea : 0.0);

        if (residentN)
        {
            qsort(resident, residentN, sizeof(float), CmpF);
            printf("\n    resident: p50 %.2f MB  p90 %.2f MB  p99 %.2f MB\n",
                   Pct(resident, residentN, 0.50), Pct(resident, residentN, 0.90),
                   Pct(resident, residentN, 0.99));
            printf("    largest: %.2f MB  %s\n", g_biggestBytes / 1048576.0f,
                   g_biggestMap);
            printf("    %lld vertices across the library\n", verts);
        }

        if (loadN)
        {
            qsort(loadMs, loadN, sizeof(float), CmpF);
            printf("\n    one load (read + build): p50 %.0f ms  p90 %.0f ms  "
                   "p99 %.0f ms\n",
                   Pct(loadMs, loadN, 0.50), Pct(loadMs, loadN, 0.90),
                   Pct(loadMs, loadN, 0.99));
            printf("    slowest: %.0f ms  %s\n", worstLoadMs, worstLoadMap);
        }
    }

    if (rays > 0 && g_rayCount)
    {
        const double f = (double)freq.QuadPart;
        printf("\nthe grid against brute force\n");
        printf("    %lld rays, %lld of them hit something (%.1f%%)\n",
               g_rayCount, g_rayHits, 100.0 * g_rayHits / g_rayCount);
        printf("    %lld DISAGREED with brute force", g_rayMismatch);
        if (g_rayMismatch)
            printf("  (first on %s)", g_mismatchMap);
        printf("\n");
        printf("    grid  %.1f us a ray\n",
               1e6 * (g_rayGrid / f) / (double)g_rayCount);
        printf("    brute %.1f us a ray  -- %.0fx\n",
               1e6 * (g_rayBrute / f) / (double)g_rayCount,
               g_rayGrid ? (double)g_rayBrute / (double)g_rayGrid : 0.0);
    }

    if (verifyNormals)
    {
        printf("\nthe file's normal against the one the velocity trace "
               "recovers\n");
        if (g_vnMapsWithPaths == 0)
        {
            printf("    no .wrpath files under %s -- nothing to compare\n",
                   pathsRoot);
        }
        else
        {
            printf("    %d maps had runs, %d files, %d sustained contact "
                   "segments\n",
                   g_vnMapsWithPaths, g_vnFiles, g_vnSegments);
            printf("    of those, %d recovered a normal inside the surf band --\n"
                   "      the only place the recovery is valid, since ground has\n"
                   "      friction and player input in the same (a - g).\n",
                   g_vnScored + g_vnNoFace + g_vnTooFar);
            printf("    %d scored; dropped %d with no brush face within %.0f "
                   "units\n", g_vnScored, g_vnNoFace + g_vnTooFar,
                   VN_MAX_FACE_DIST);
            printf("      (displacements, func_* and playerclip -- what this "
                   "reader does not have),\n");
            printf("      and %d that would not fit.\n", g_vnNoFit);
            if (g_vnScored)
                printf("    %d of the scored matched a face that is NOT itself "
                       "surfable (%.1f%%)\n", g_vnOffBand,
                       100.0 * g_vnOffBand / g_vnScored);

            if (g_vnErrN > 0)
            {
                qsort(g_vnErr, g_vnErrN, sizeof(float), CmpF);
                int w1 = 0, w5 = 0, w15 = 0, gross = 0;
                for (int i = 0; i < g_vnErrN; i++)
                {
                    if (g_vnErr[i] <= 1.0f) w1++;
                    if (g_vnErr[i] <= 5.0f) w5++;
                    if (g_vnErr[i] <= 15.0f) w15++;
                    if (g_vnErr[i] > 30.0f) gross++;
                }
                printf("\n    disagreement: p50 %.2f deg  p90 %.2f  p99 %.2f  "
                       "worst %.1f\n",
                       Pct(g_vnErr, g_vnErrN, 0.50), Pct(g_vnErr, g_vnErrN, 0.90),
                       Pct(g_vnErr, g_vnErrN, 0.99), g_vnErr[g_vnErrN - 1]);
                printf("    within 1 deg  %.1f%%\n",
                       100.0 * w1 / g_vnErrN);
                printf("    within 5 deg  %.1f%%\n",
                       100.0 * w5 / g_vnErrN);
                printf("    within 15 deg %.1f%%\n",
                       100.0 * w15 / g_vnErrN);
                // The tail is the interesting half, and it took four tries to
                // find out what is in it. It is NOT the wrong face: matching
                // by a downward ray instead of by proximity moved this figure
                // from 13.0% to 12.4%. It is not conditioning either, and it
                // is not the impulse. It is mid-ride contacts, and the proof
                // is the board population below -- the same estimator, the
                // same faces, restricted to ticks arriving out of free flight,
                // drops it to 1.0%.
                printf("    over 30 deg   %.1f%%\n", 100.0 * gross / g_vnErrN);
                if (g_vnWorstMedian >= 0.0f)
                    printf("    worst map by median: %.1f deg  %s\n",
                           g_vnWorstMedian, g_vnWorstMap);

                static const char *kB[3] = { "face within 8 units",
                                             "8 to 24 units",
                                             "24 to 48 units" };
                printf("\n    by how far the matched face was:\n");
                for (int b = 0; b < 3; b++)
                {
                    if (g_vnBucketN[b] <= 0)
                        continue;
                    qsort(g_vnBucket[b], g_vnBucketN[b], sizeof(float), CmpF);
                    int w5 = 0;
                    for (int i = 0; i < g_vnBucketN[b]; i++)
                        if (g_vnBucket[b][i] <= 5.0f) w5++;
                    printf("      %-20s %6d   p50 %6.2f  p90 %7.2f  "
                           "within 5 deg %.1f%%\n",
                           kB[b], g_vnBucketN[b],
                           Pct(g_vnBucket[b], g_vnBucketN[b], 0.50),
                           Pct(g_vnBucket[b], g_vnBucketN[b], 0.90),
                           100.0 * w5 / g_vnBucketN[b]);
                }
            }

            for (int q = 0; q < VC_POPS; q++)
            {
                VcPop *P = &g_vc[q];
                if (P->errN <= 0)
                    continue;
                qsort(P->err, P->errN, sizeof(float), CmpF);

                int c1 = 0, c5 = 0, c15 = 0, cg = 0;
                for (int i = 0; i < P->errN; i++)
                {
                    if (P->err[i] <= 1.0f) c1++;
                    if (P->err[i] <= 5.0f) c5++;
                    if (P->err[i] <= 15.0f) c15++;
                    if (P->err[i] > 30.0f) cg++;
                }
                printf("\n  one tick, impulse over %.0f u/s: %s\n",
                       VN_CLIP_MIN_IMPULSE, kVcName[q]);
                printf("    %d ticks, %d scored, %d with no face under them\n",
                       P->tried, P->errN, P->noFace);
                printf("    normal to normal: p50 %.2f deg  p90 %.2f  "
                       "p99 %.2f\n",
                       Pct(P->err, P->errN, 0.50), Pct(P->err, P->errN, 0.90),
                       Pct(P->err, P->errN, 0.99));
                printf("      within 1 deg %.1f%%   within 5 %.1f%%   "
                       "within 15 %.1f%%   over 30 %.1f%%\n",
                       100.0 * c1 / P->errN, 100.0 * c5 / P->errN,
                       100.0 * c15 / P->errN, 100.0 * cg / P->errN);

                if (P->signedN > 0)
                {
                    qsort(P->signed_, P->signedN, sizeof(float), CmpF);
                    printf("    signed, recovered minus file, in surface "
                           "degrees:\n");
                    printf("      p10 %+.2f  p25 %+.2f  p50 %+.2f  p75 %+.2f  "
                           "p90 %+.2f\n",
                           Pct(P->signed_, P->signedN, 0.10),
                           Pct(P->signed_, P->signedN, 0.25),
                           Pct(P->signed_, P->signedN, 0.50),
                           Pct(P->signed_, P->signedN, 0.75),
                           Pct(P->signed_, P->signedN, 0.90));
                }

                if (P->slopeN > 0)
                {
                    qsort(P->slope, P->slopeN, sizeof(float), CmpF);
                    int s1 = 0, s3 = 0, s5 = 0;
                    for (int i = 0; i < P->slopeN; i++)
                    {
                        if (P->slope[i] <= 1.0f) s1++;
                        if (P->slope[i] <= 3.0f) s3++;
                        if (P->slope[i] <= 5.0f) s5++;
                    }
                    printf("    SLOPE alone, which is the number anything "
                           "prints:\n");
                    printf("      p50 %.2f deg  p90 %.2f  p99 %.2f   "
                           "within 1 %.1f%%  within 3 %.1f%%  within 5 %.1f%%\n",
                           Pct(P->slope, P->slopeN, 0.50),
                           Pct(P->slope, P->slopeN, 0.90),
                           Pct(P->slope, P->slopeN, 0.99),
                           100.0 * s1 / P->slopeN, 100.0 * s3 / P->slopeN,
                           100.0 * s5 / P->slopeN);
                }
                if (P->headN > 0)
                {
                    qsort(P->head, P->headN, sizeof(float), CmpF);
                    printf("    HEADING alone, which nothing prints:\n");
                    printf("      p50 %.2f deg  p90 %.2f  p99 %.2f\n",
                           Pct(P->head, P->headN, 0.50),
                           Pct(P->head, P->headN, 0.90),
                           Pct(P->head, P->headN, 0.99));
                }
            }
        }
    }

    if (angles && g_angleArea > 0.0)
    {
        printf("\nup-facing surface angle, weighted by area\n");
        printf("    0 is a floor, 90 a wall. 45.6 is Source's own standable\n"
               "    cut -- below it you slide, and that is what a surf ramp is.\n\n");
        double peak = 0.0;
        for (int i = 0; i <= 90; i++)
            if (g_angle[i] > peak)
                peak = g_angle[i];
        double band = 0.0;
        for (int i = 0; i <= 90; i++)
        {
            double frac = g_angle[i] / g_angleArea;
            if (WrBspIsSurfBand((float)cos(i * 3.14159265358979 / 180.0)))
                band += frac;
            if (frac < 0.002 && g_angle[i] < peak * 0.02)
                continue;
            int bars = (int)(60.0 * g_angle[i] / peak + 0.5);
            printf("    %2d deg  %5.2f%%  ", i, 100.0 * frac);
            for (int k = 0; k < bars; k++)
                putchar('#');
            putchar('\n');
        }
        printf("\n    %.1f%% of up-facing area is inside the surf band\n",
               100.0 * band);
    }

    printf("\nrefused: %d on read, %d on walk, %d on build\n",
           refusedRead, refusedWalk, refusedBuild);
    for (int i = 0; i < g_reasons; i++)
        printf("    %4d  %s\n            first: %s\n",
               g_reasonN[i], g_reason[i], g_reasonMap[i]);

    free(g_vnErr);
    for (int q = 0; q < VC_POPS; q++)
        free(g_vc[q].err);
    free(loadMs);
    free(ownedPct);
    free(resident);

    // The exit code is the whole assertion. Anything refused is a layout this
    // does not know about, and the point of running it is to find out.
    bool clean = (refusedRead == 0 && refusedWalk == 0 &&
                  refusedBuild == 0 && sideNotClosed == 0 &&
                  g_rayMismatch == 0 && g_reasons == 0);
    printf("\n%s\n", clean ? "every map read and walked"
                           : "SOME MAPS WERE REFUSED");
    return clean ? 0 : 1;
}
