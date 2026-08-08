// wr_render.cpp  --  world-space paths, drawn with ImGui's draw list.
//
// We project points ourselves and hand ImGui screen-space polylines rather than
// building D3D11 line geometry. That buys thickness, per-run colour, alpha,
// distance fade and text labels with zero device-state risk, since
// ImGui_ImplDX11_RenderDrawData already saves and restores everything it
// touches. The price is no depth test: lines draw through walls. For a route
// tool that is arguably the desired behaviour, and it is the honest trade for
// not having to capture the game's depth buffer.
//
// The two things that have to be right:
//
//   Near-plane clipping. w(P) is affine in world position, so a segment that
//   straddles the camera plane can be clipped by lerping in WORLD space to the
//   w = NEAR_W plane, exactly. Dropping such segments instead is what makes
//   lines visibly pop as you turn around.
//
//   Polyline continuity. AddPolyline takes one colour and one thickness per
//   call, so anything that varies per point -- distance fade, colour by speed --
//   has to be quantised into buckets and flushed when the bucket changes.

#include "wr_render.h"
#include "wr_engine.h"
#include "wr_path.h"
#include "wr_steam.h"
#include "wr_energy.h"
#include "wr_start.h"
#include "wr_imgui.h"
#include "wr_stress.h"
#include "wr_hook.h"
#include "wr_log.h"

#include "imgui.h"

#include <math.h>
#include <stdio.h>
#include <float.h>

WrRenderSettings g_render;

#define NEAR_W 1.0f
#define MAX_BATCH 4096
// Buckets for the distance FADE, not for the alpha setting -- see EmitPath.
// Twelve rather than eight makes the fade ramp smoother; the cost is at most a
// few extra polyline flushes on a path that spans the whole fade band.
#define ALPHA_BUCKETS 12
#define COLOUR_BUCKETS 16
// ODD, deliberately. An even count over a symmetric range has no bucket at zero,
// so eta 0 -- free flight, the value a player sees most -- landed on +0.067 and
// drew as a faintly green grey, while the velocity vector drew the same eta as
// true neutral. See WrEtaBucket in wr_stress.h.
#define EFF_BUCKETS 17
#define EFF_NO_DATA_CLASS EFF_BUCKETS       // one more class, for "no reading"
#define EFF_CLASSES (EFF_BUCKETS + 1)
#define OFFSCREEN_BREAK 8

static ImVec2 g_batch[MAX_BATCH];
static int g_batchCount = 0;
static int g_statSegments = 0;
static int g_statPoints = 0;
static int g_statBatches = 0;
static float g_statMillis = 0.0f;

void WrRenderDefaults(void)
{
    g_render.thickness = 2.5f;
    g_render.alpha = 0.85f;
    g_render.maxDrawDistance = 4000.0f;
    g_render.fadeStartFraction = 0.75f;
    g_render.pixelTolerance = 2.0f;
    // 256, not 8. Eight was chosen when a map's runs were whatever the game had
    // downloaded and the interesting ones were the top few; with a board you can
    // fetch in bulk, a cap of eight silently hides most of what you just asked
    // for.
    //
    // What this costs, measured rather than assumed, on a compact stage where
    // the whole leg sits inside the draw distance and nothing is culled:
    //
    //     8 drawn    0.24 ms/frame        256 drawn   8.1 ms/frame
    //                                    1000 drawn  32.5 ms/frame
    //
    // plus about 1.7 ms of ImGui tessellation at 256, for ~15 MB of vertex and
    // index data built every frame. A 60 Hz frame is 16.7 ms.
    //
    // So this is not free, and the point budget does NOT bound it -- that is per
    // run (see EmitPath), so the total is the budget times the number of lines.
    // Distance culling is the only thing that does bound it, and the compact
    // stage is precisely the case where culling rejects nothing.
    //
    // It is still the right default, because the number that matters is how many
    // runs are ENABLED, and that is one out of the box: FinishLoad enables the
    // first run and the auto-enable picks one more nearby. Nothing reaches 256
    // lines without ticking All on a map that has them. This cap is what stops
    // that press from being a freeze; it is not what makes it cheap.
    g_render.maxRunsDrawn = 256;
    g_render.pointBudget = 1500;
    g_render.lineColour = WR_LINE_FLAT;
    g_render.speedMin = 250.0f;
    g_render.speedMax = 3500.0f;

    // Energy is a HEIGHT in world units -- z + |v|^2/2g -- so it is an absolute
    // map coordinate, not a per-run quantity. That is deliberate: the same
    // colour then means the same energy on every line, which is the only way
    // two runs can be compared by eye. It also means the default range is a
    // guess until it is fitted to the map, which is what the button is for.
    g_render.energyMin = 0.0f;
    g_render.energyMax = 4000.0f;

    // On. The pre-roll is not part of the run, and showing it is what makes a
    // line appear to start somewhere the player was not yet racing.
    g_render.hidePreRoll = true;

    g_render.drawMarkers = true;
    g_render.markerRadius = 6.0f;
    g_render.drawLive = true;
    g_render.liveColour = 0xFF66FF66;
    g_render.drawTags = true;
    g_render.tagAvatars = true;
    g_render.tagScale = 1.0f;
    // Twelve, not forty. Forty legible tags do not fit on a screen, and drawing
    // forty overlapping ones is worse than saying so in the UI.
    g_render.maxTags = 12;
    g_render.drawDipSpeeds = true;
    g_render.maxDipsPerRun = 24;
    g_render.dipLabel = WR_LABEL_SPEED;

    // Tops default OFF, unlike bottoms. Every arc has one of each, so turning
    // both on doubles the numbers on screen, and the bottom is the one that
    // answers "did this line carry its speed". A top is what you turn on when
    // you are asking a narrower question.
    g_render.drawPeaks = false;
    g_render.maxPeaksPerRun = 24;
    g_render.peakLabel = WR_LABEL_ENERGY;
    g_render.markerLabel = WR_LABEL_TIME;
    g_render.maxMarkersPerRun = 24;
    // A label is far bigger than a line segment and cannot be decimated, so the
    // budget that matters is a global one. Eight runs with four-line labels at
    // every checkpoint is unreadable long before it is slow.
    g_render.maxLabelsPerFrame = 40;
    g_render.drawVelocity = true;
    // Symmetric. Measured p60 of in-band eta is +0.589, and once the loss side
    // stopped being discarded it carries more mass than the gain side, not less.
    g_render.effSaturation = 0.60f;
    g_render.effNeutralBand = 0.12f;
    g_render.effNeutralMix = 0.70f;
    g_render.effNoDataAlpha = 0.35f;
    g_render.effColourblind = false;
    g_render.lineKey = true;

    g_render.pickEnabled = true;
    // 48 px on a 1080p screen is about 2.5 degrees of arc at a 90 degree fov --
    // a bit wider than a crosshair, which is what you want when the thing being
    // aimed at is a two-pixel line that may be moving across the view.
    g_render.pickRadiusPx = 48.0f;
    g_render.pickDepthBias = 0.35f;
    g_render.pickThickBoost = 1.8f;
    g_render.pickHoldSeconds = 0.25f;
    g_render.pickLabel = WR_LABEL_SPEED | WR_LABEL_ENERGY | WR_LABEL_TIME;
    g_render.pickRing = true;

    g_render.rankColour = WR_RANK_OFF;
    // 25% off the record is fully red. Measured on the 66 surf_demise runs here
    // (37.17 to 79.08 s): at 25% the pack near 40 s stays green and only the
    // genuinely slow runs redden, which is the point of the time mode.
    g_render.rankFullBehind = 25.0f;
    g_render.rankLegend = true;
}

// ---------------------------------------------------------------------------
// Stage timers
// ---------------------------------------------------------------------------

static long long g_stageStart[WR_STAGE_COUNT];
static float g_stageMs[WR_STAGE_COUNT];

// QueryPerformanceFrequency is a constant for the life of the process. It was
// being asked for once per frame.
static long long Qpf(void)
{
    static long long f = 0;
    if (f == 0)
    {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        f = q.QuadPart ? q.QuadPart : 1;
    }
    return f;
}

void WrStageBegin(WrStage s)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    g_stageStart[s] = t.QuadPart;
}

void WrStageEnd(WrStage s)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    float ms = (float)((double)(t.QuadPart - g_stageStart[s]) * 1000.0 / (double)Qpf());
    // Smoothed, or the numbers are unreadable at 300 fps.
    g_stageMs[s] = g_stageMs[s] * 0.95f + ms * 0.05f;
}

float WrStageMillis(WrStage s)
{
    return (s >= 0 && s < WR_STAGE_COUNT) ? g_stageMs[s] : 0.0f;
}

const char *WrStageName(WrStage s)
{
    switch (s)
    {
    case WR_STAGE_IDLE:   return "bookkeeping";
    case WR_STAGE_EMIT:   return "lines";
    case WR_STAGE_UI:     return "panel";
    case WR_STAGE_SUBMIT: return "imgui submit";
    default:              return "?";
    }
}

// Would this frame put anything on screen?
//
// Asked from inside Present, before any device state is touched. If the answer
// is no, the entire draw path is skipped -- see HookedPresent in wr_hook.cpp for
// why that matters more than the work it saves.
bool WrHasAnythingToDraw(void)
{
    if (WrMenuOpen())
        return true;

    // Everything else is drawn inside WrRenderWorld, which gives up immediately
    // without a world-to-screen matrix. No matrix -- main menu, loading screen,
    // scan still searching -- means nothing can appear no matter what is ticked.
    VMatrix m;
    if (!WrWorldToScreen(&m))
        return false;

    // The energy readouts need a camera as much as the lines do.
    if ((g_energy.showHud || g_energy.showOverlay) && WrEnergyValid())
        return true;
    if (g_render.drawLive && WrLiveEnabled())
        return true;
    for (int i = 0; i < WrRunCount(); i++)
    {
        const WrRun *r = WrRunAt(i);
        if (r && r->enabled && r->pointCount >= 2)
            return true;
    }
    return false;
}

void WrRenderStats(int *segments, int *pointsConsidered, int *batches,
                   float *millis)
{
    if (segments) *segments = g_statSegments;
    if (pointsConsidered) *pointsConsidered = g_statPoints;
    if (batches) *batches = g_statBatches;
    if (millis) *millis = g_statMillis;
}

// ---------------------------------------------------------------------------
// Projection
// ---------------------------------------------------------------------------

static const VMatrix *g_m;
static float g_sw, g_sh;
static Vec3 g_cam;

static inline float ClipW(const Vec3 &p)
{
    return g_m->m[3][0] * p.x + g_m->m[3][1] * p.y + g_m->m[3][2] * p.z + g_m->m[3][3];
}

static bool ProjectKnownW(const Vec3 &p, float w, ImVec2 *out)
{
    float x = g_m->m[0][0] * p.x + g_m->m[0][1] * p.y + g_m->m[0][2] * p.z + g_m->m[0][3];
    float y = g_m->m[1][0] * p.x + g_m->m[1][1] * p.y + g_m->m[1][2] * p.z + g_m->m[1][3];
    float inv = 1.0f / w;
    float sx = (g_sw * 0.5f) + (0.5f * x * inv * g_sw);
    float sy = (g_sh * 0.5f) - (0.5f * y * inv * g_sh);
    if (!WrSaneFloat(sx) || !WrSaneFloat(sy))
        return false;
    // Bound the coordinates so a point just past the near plane cannot hand
    // ImGui a 1e6-pixel vertex; well off screen, so no visible kink.
    float lim = 32.0f * (g_sw > g_sh ? g_sw : g_sh);
    out->x = WrClampF(sx, -lim, lim);
    out->y = WrClampF(sy, -lim, lim);
    return true;
}

static bool Project(const Vec3 &p, ImVec2 *out)
{
    float w = ClipW(p);
    if (w < NEAR_W)
        return false;
    return ProjectKnownW(p, w, out);
}

// Clip segment a->b against the w = NEAR_W plane in world space. Returns false
// if the whole segment is behind the camera.
static bool ClipToNear(Vec3 *a, Vec3 *b)
{
    float wa = ClipW(*a);
    float wb = ClipW(*b);
    if (wa < NEAR_W && wb < NEAR_W)
        return false;
    if (wa >= NEAR_W && wb >= NEAR_W)
        return true;

    float denom = wb - wa;
    if (fabsf(denom) < 1e-6f)
        return false;
    float t = (NEAR_W - wa) / denom;
    t = WrClampF(t, 0.0f, 1.0f);
    Vec3 hit = WrAdd(*a, WrScale(WrSub(*b, *a), t));
    if (wa < NEAR_W)
        *a = hit;
    else
        *b = hit;
    return true;
}

// ---------------------------------------------------------------------------
// Batching
// ---------------------------------------------------------------------------

static void FlushBatch(ImDrawList *dl, unsigned int colour, float thickness)
{
    if (g_batchCount >= 2)
    {
        dl->AddPolyline(g_batch, g_batchCount, colour, ImDrawFlags_None, thickness);
        g_statSegments += g_batchCount - 1;
        g_statBatches++;
    }
    g_batchCount = 0;
}

static inline unsigned int WithAlpha(unsigned int colour, float a)
{
    unsigned int base = colour & 0x00FFFFFFu;
    unsigned int alpha = (unsigned int)(WrClampF(a, 0.0f, 1.0f) * 255.0f + 0.5f);
    return base | (alpha << 24);
}

// The field ramp: green at the front, red at the back, t in [0, 1].
//
// Amber through the middle rather than a straight green-to-red lerp, which
// passes through a muddy olive that reads as neither. Blue stays at or below
// 0.25 for the whole length, which is what leaves violet free to mean first
// place and nothing else.
//
// Its own function so the on-screen key can draw the ACTUAL endpoints of the
// ramp rather than two colours that merely look about right -- the same reason
// WrRunColour exists at all.
static unsigned int RankRampColour(float t)
{
    t = WrClampF(t, 0.0f, 1.0f);

    float r, g, b;
    if (t < 0.5f)
    {
        float u = t / 0.5f;
        r = 0.30f + 0.70f * u; g = 0.90f; b = 0.25f * (1.0f - u);
    }
    else
    {
        float u = (t - 0.5f) / 0.5f;
        r = 1.00f; g = 0.90f - 0.75f * u; b = 0.10f * u;
    }
    unsigned int ri = (unsigned int)(WrClampF(r, 0.0f, 1.0f) * 255.0f + 0.5f);
    unsigned int gi = (unsigned int)(WrClampF(g, 0.0f, 1.0f) * 255.0f + 0.5f);
    unsigned int bi = (unsigned int)(WrClampF(b, 0.0f, 1.0f) * 255.0f + 0.5f);
    return 0xFF000000u | (bi << 16) | (gi << 8) | ri;
}

// What colour is this run?
//
// Every place that used to read run->colour goes through here. That is the whole
// point: colour is a run's identity on screen, and if the line changed but its
// name tag, its ramp numbers, its checkpoints and its comparison ring did not,
// the run would be two different colours at once.
//
// Off by default, in which case this is exactly run->colour -- including any
// colour picked by hand in the Runs tab, which a rank mode necessarily
// overrides while it is on.
unsigned int WrRunColour(const WrRun *run)
{
    if (!run)
        return 0xFFCCCCCCu;
    if (g_render.rankColour == WR_RANK_OFF)
        return run->colour;

    int total = 0;
    int rank = WrRunRankInTrack(run, &total);
    if (rank <= 0)
        return run->colour;

    if (rank == 1) return WR_COL_FIRST;

    // t is 0 at the front of the field and 1 at the back.
    float t = 0.5f;
    if (g_render.rankColour == WR_RANK_BY_TIME)
    {
        // How far off the best this run is, as a fraction of the best. Truthful
        // rather than even: on a board where everyone is within a second of the
        // record, everyone stays green, because they are all nearly as fast.
        const WrRun *best = NULL;
        for (int i = 0; i < WrRunCount(); i++)
        {
            const WrRun *c = WrRunAt(i);
            if (c && c->pointCount >= 2 && c->trackType == run->trackType &&
                c->trackNum == run->trackNum)
            {
                best = c;       // store is time-sorted, so the first match is it
                break;
            }
        }
        float span = g_render.rankFullBehind;
        if (span < 0.5f)
            span = 0.5f;
        if (best && best->runTime > 0.001)
            t = (float)((run->runTime - best->runTime) / best->runTime) *
                (100.0f / span);
        t = WrClampF(t, 0.0f, 1.0f);
    }
    else
    {
        // Even spread over everyone behind the winner: rank 2 is the front of
        // the ramp, last is the back. Only one run is held out of the ramp, so
        // second and third are shaded like anyone else -- which is the point.
        // On a tight board they come out barely off green, and that is a truer
        // picture than a silver medal that says "second" and nothing about by
        // how much.
        int spread = total - 1;
        t = (spread > 1) ? (float)(rank - 2) / (float)(spread - 1) : 0.0f;
        t = WrClampF(t, 0.0f, 1.0f);
    }

    return RankRampColour(t);
}

// Blue -> cyan -> green -> yellow -> red across the configured speed range.
static unsigned int SpeedColour(float speed)
{
    float t = (speed - g_render.speedMin) /
              (g_render.speedMax - g_render.speedMin + 1e-3f);
    t = WrClampF(t, 0.0f, 1.0f);
    float r, g, b;
    if (t < 0.25f)      { float u = t / 0.25f;          r = 0.0f;      g = u;          b = 1.0f; }
    else if (t < 0.5f)  { float u = (t - 0.25f) / 0.25f; r = 0.0f;      g = 1.0f;       b = 1.0f - u; }
    else if (t < 0.75f) { float u = (t - 0.5f) / 0.25f;  r = u;         g = 1.0f;       b = 0.0f; }
    else                { float u = (t - 0.75f) / 0.25f; r = 1.0f;      g = 1.0f - u;   b = 0.0f; }
    unsigned int ri = (unsigned int)(r * 255.0f);
    unsigned int gi = (unsigned int)(g * 255.0f);
    unsigned int bi = (unsigned int)(b * 255.0f);
    return 0xFF000000u | (bi << 16) | (gi << 8) | ri;   // ImGui packs as ABGR
}

// Emit one path. Points are world space.
//
// velScale converts a point's stored .vel into units/second. Extracted runs
// already store true velocity, so they pass 1. The live recorder stores a raw
// per-sample delta instead, which is not a speed at all, so it passes 0 to mean
// "no usable speed here" and colour-by-speed falls back to the flat colour.
// Defined below, with the rest of the energy drawing.
static unsigned int EfficiencyColour(float eta, unsigned int runColour);
static unsigned int MixColour(unsigned int c, float r, float g, float b, float t);

// A descriptor rather than eleven positional arguments, three of which are
// floats that mean entirely different things. The body below keeps its old local
// names, so this is an interface change and not a rewrite.
struct WrPathDraw
{
    const WrPoint *pts;
    int count;
    int first;                  // where the RUN starts; 0 for the live trail
    const int *breaks;
    int breakCount;
    const signed char *eff;
    unsigned int baseColour;
    float velScale;
    float thickness;
    float lift;                 // 0 normally; mixes toward white for the pick
};

static void EmitPath(ImDrawList *dl, const WrPathDraw &d)
{
    const WrPoint *pts = d.pts;
    const int count = d.count;
    const int *breaks = d.breaks;
    const int breakCount = d.breakCount;
    const signed char *eff = d.eff;
    const float velScale = d.velScale;
    const float thickness = d.thickness;

    unsigned int baseColour = d.baseColour;
    if (d.lift > 0.0f)
        baseColour = MixColour(baseColour, 1.0f, 1.0f, 1.0f, d.lift);

    if (count < 2)
        return;

    int first = d.first;
    if (first < 0 || first > count - 2)
        first = 0;

    const float maxDist = g_render.maxDrawDistance;
    const float maxDistSqr = maxDist * maxDist;
    const float fadeStart = maxDist * g_render.fadeStartFraction;
    const float tol = g_render.pixelTolerance;

    // Index-space budget, on top of the screen-space decimation below.
    //
    // Distance culling does most of the work on a big open map -- on surf_demise
    // the path spans 25000 units, so at a 4000-unit draw distance the large
    // majority of it is rejected before it costs anything. A compact staged map
    // is the opposite case: the whole leg fits inside the draw distance, nothing
    // is culled, and the per-frame cost is the full point count times the number
    // of runs. That is what made it stutter more with every run enabled.
    //
    // Screen decimation alone does not save it, because those points still have
    // to be projected to find out they are close together. Skipping in index
    // space bounds the work before any of it happens.
    int step = 1;
    int span = count - first;
    if (g_render.pointBudget > 16 && span > g_render.pointBudget)
        step = (span + g_render.pointBudget - 1) / g_render.pointBudget;

    int lastBucket = -1;
    unsigned int lastColour = 0;
    ImVec2 lastEmitted(0.0f, 0.0f);
    int offscreenRun = 0;
    bool have = false;

    // Teleports come from wr_path.cpp, found at full resolution when the file
    // was loaded. They must not be re-derived here from a distance test on the
    // strided chord: that test has to be widened in proportion to the stride,
    // and by the time a long run is decimated enough to draw cheaply the
    // threshold is wider than the teleports themselves. Measured on
    // surf_tensor2's 38751-point main run, 12 of its 23 teleports were being
    // joined into straight bars up to 10000 units long, and the artifact
    // appeared and vanished as the point budget slider was moved.
    //
    // breaks[] is sorted, and i only increases, so a cursor walks it in step.
    int bi = 0;

    for (int i = first; i + step < count; i += step)
    {
        Vec3 a = pts[i].pos;
        Vec3 b = pts[i + step].pos;
        g_statPoints++;

        while (bi < breakCount && breaks[bi] < i)
            bi++;
        if (bi < breakCount && breaks[bi] < i + step)
        {
            // This chord spans a teleport somewhere inside it.
            if (have)
            {
                FlushBatch(dl, lastColour, thickness);
                have = false;
            }
            continue;
        }

        // Distance cull against the camera, cheap and squared.
        float dsq = WrDistSqr(a, g_cam);
        if (dsq > maxDistSqr)
        {
            if (have)
            {
                FlushBatch(dl, lastColour, thickness);
                have = false;
            }
            continue;
        }

        // Whether clipping is about to move this segment's START. If it does,
        // `a` is no longer where the previous segment ended, and the batch has
        // to be flushed -- see the push below, which only ever emits `pa` when
        // starting a fresh batch.
        bool clippedStart = (ClipW(a) < NEAR_W);

        if (!ClipToNear(&a, &b))
        {
            if (have)
            {
                FlushBatch(dl, lastColour, thickness);
                have = false;
            }
            continue;
        }

        // A segment whose start was pulled onto the near plane does not begin
        // where the last one finished: the previous segment was clipped at its
        // *end*, landing on a different point of that plane -- often on the
        // opposite side of the view. Continuing the batch would draw a line
        // from the wrong place, sweeping right across the screen. This happens
        // whenever the route doubles back through the camera plane, which is
        // ordinary on switchbacks and 180 turns.
        if (have && clippedStart)
        {
            FlushBatch(dl, lastColour, thickness);
            have = false;
        }

        ImVec2 pa, pb;
        float wa = ClipW(a), wb = ClipW(b);
        if (!ProjectKnownW(a, wa, &pa) || !ProjectKnownW(b, wb, &pb))
        {
            if (have)
            {
                FlushBatch(dl, lastColour, thickness);
                have = false;
            }
            continue;
        }

        // Break the polyline after a sustained off-screen stretch. The
        // discontinuity is by definition not visible.
        bool onGuard = (pa.x > -g_sw && pa.x < g_sw * 2.0f &&
                        pa.y > -g_sh && pa.y < g_sh * 2.0f);
        if (!onGuard)
        {
            if (++offscreenRun >= OFFSCREEN_BREAK)
            {
                if (have)
                {
                    FlushBatch(dl, lastColour, thickness);
                    have = false;
                }
                continue;
            }
        }
        else
        {
            offscreenRun = 0;
        }

        // Fade with distance. Only the FADE is quantised -- it varies per point,
        // so runs of equal fade are what batch together. The alpha setting is
        // constant across the whole path and is multiplied in exactly.
        //
        // Quantising their product, which is what this used to do, meant the
        // slider only had ALPHA_BUCKETS reachable values and moved in visible
        // steps. Splitting them costs nothing: the batch key is still the fade
        // bucket, so the number of AddPolyline calls is unchanged.
        float dist = sqrtf(dsq);
        float fade = 1.0f;
        if (dist > fadeStart && maxDist > fadeStart)
            fade = 1.0f - (dist - fadeStart) / (maxDist - fadeStart);
        int fBucket = (int)(WrClampF(fade, 0.0f, 1.0f) * (ALPHA_BUCKETS - 1) + 0.5f);
        float a01 = WrClampF(g_render.alpha * (float)fBucket / (ALPHA_BUCKETS - 1),
                             0.0f, 1.0f);

        unsigned int colour;
        int bucket;
        if (g_render.lineColour == WR_LINE_EFFICIENCY && eff)
        {
            signed char e = eff[i];
            if (e == WR_ETA_NO_DATA)
            {
                // Not neutral. A booster, a gap across a teleport, or the ends
                // of the run where a centred difference has nothing to work
                // with. Faded, in the run's own colour, so it reads as an
                // absence rather than as free flight.
                colour = WithAlpha(baseColour, a01 * g_render.effNoDataAlpha);
                bucket = fBucket * EFF_CLASSES + EFF_NO_DATA_CLASS;
            }
            else
            {
                int cBucket = WrEtaBucket(WrEtaFromByte(e), EFF_BUCKETS);
                colour = WithAlpha(
                    EfficiencyColour(WrEtaFromBucket(cBucket, EFF_BUCKETS),
                                     baseColour), a01);
                bucket = fBucket * EFF_CLASSES + cBucket;
            }
        }
        else if (g_render.lineColour == WR_LINE_SPEED && velScale > 0.0f)
        {
            float speed = WrLength(pts[i].vel) * velScale;
            float t = WrClampF((speed - g_render.speedMin) /
                               (g_render.speedMax - g_render.speedMin + 1e-3f),
                               0.0f, 1.0f);
            int cBucket = (int)(t * (COLOUR_BUCKETS - 1) + 0.5f);
            colour = WithAlpha(SpeedColour(g_render.speedMin +
                        (g_render.speedMax - g_render.speedMin) *
                        ((float)cBucket / (COLOUR_BUCKETS - 1))), a01);
            bucket = fBucket * COLOUR_BUCKETS + cBucket;
        }
        else if (g_render.lineColour == WR_LINE_ENERGY && velScale > 0.0f)
        {
            // z + |v|^2/2g, and cheaper than the speed branch above because it
            // needs no sqrt. Absolute, not relative to the run's own start:
            // a colour has to mean the same thing on every line or two lines
            // cannot be read against each other, which is the whole point.
            //
            // Quantised through the same bucket key as speed, so the number of
            // AddPolyline calls is unchanged. Reusing SpeedColour's ramp is
            // deliberate too -- a second ramp for a second quantity would be
            // one more thing to learn for no gain.
            float e = WrEnergyOf(pts[i].pos, pts[i].vel);
            float t = WrClampF((e - g_render.energyMin) /
                               (g_render.energyMax - g_render.energyMin + 1e-3f),
                               0.0f, 1.0f);
            int cBucket = (int)(t * (COLOUR_BUCKETS - 1) + 0.5f);
            colour = WithAlpha(SpeedColour(g_render.speedMin +
                        (g_render.speedMax - g_render.speedMin) *
                        ((float)cBucket / (COLOUR_BUCKETS - 1))), a01);
            bucket = fBucket * COLOUR_BUCKETS + cBucket;
        }
        else
        {
            colour = WithAlpha(baseColour, a01);
            bucket = fBucket;
        }

        if (have && bucket != lastBucket)
        {
            FlushBatch(dl, lastColour, thickness);
            have = false;
        }

        if (!have)
        {
            g_batch[g_batchCount++] = pa;
            lastEmitted = pa;
            have = true;
        }
        else
        {
            // Screen-space decimation: skip points that barely move on screen.
            float dx = pb.x - lastEmitted.x;
            float dy = pb.y - lastEmitted.y;
            if (dx * dx + dy * dy < tol * tol)
            {
                lastBucket = bucket;
                lastColour = colour;
                continue;
            }
        }

        g_batch[g_batchCount++] = pb;
        lastEmitted = pb;
        lastBucket = bucket;
        lastColour = colour;

        if (g_batchCount >= MAX_BATCH - 2)
        {
            ImVec2 carry = g_batch[g_batchCount - 1];
            FlushBatch(dl, colour, thickness);
            g_batch[g_batchCount++] = carry;    // keep the line continuous
            have = true;
        }
    }

    if (have)
        FlushBatch(dl, lastColour, thickness);
}

// ---------------------------------------------------------------------------
// Labels on the line, and keeping them apart
// ---------------------------------------------------------------------------
//
// A rectangle reservation shared by ramp-bottom numbers, checkpoint numbers and
// player tags. It used to belong to the tags alone, so a tag would never cover
// another tag but numbers covered everything including each other -- which was
// survivable when a ramp label was one number and is not once it can be four
// lines at every checkpoint of eight runs.
//
// Numbers register BEFORE tags, deliberately. A number is attached to a specific
// point on a line and means nothing anywhere else; a name has a whole line to
// slide along and six nudges to do it with.

#define WR_MAX_TAG_RECTS 128
#define WR_TAG_NUDGES 6

struct TagRect { float x0, y0, x1, y1; };
static TagRect g_tagRects[WR_MAX_TAG_RECTS];
static int g_tagRectCount = 0;
static int g_statTags = 0;
static int g_statLabels = 0;

static bool TagOverlaps(const TagRect &r)
{
    for (int i = 0; i < g_tagRectCount; i++)
    {
        const TagRect &o = g_tagRects[i];
        if (r.x0 < o.x1 && r.x1 > o.x0 && r.y0 < o.y1 && r.y1 > o.y0)
            return true;
    }
    return false;
}

static bool ReserveRect(const TagRect &r)
{
    if (g_tagRectCount >= WR_MAX_TAG_RECTS || TagOverlaps(r))
        return false;
    g_tagRects[g_tagRectCount++] = r;
    return true;
}

// Build a label from whatever the user has asked for. `theirs` is the run point
// the label belongs to; `t` its time, negative when there is no trustworthy one.
//
// Every line is optional and the whole thing may come out empty, which is not a
// failure -- it is what "show me nothing here" looks like.
static void BuildLabel(char *out, size_t cap, unsigned int what,
                       const WrPoint *theirs, float t, bool timeTrusted)
{
    out[0] = '\0';
    size_t used = 0;
    char part[64];

    #define APPEND(...)                                                        \
        do {                                                                   \
            _snprintf_s(part, sizeof(part), _TRUNCATE, __VA_ARGS__);           \
            if (used) { _snprintf_s(out + used, cap - used, _TRUNCATE, "\n");  \
                        used = strlen(out); }                                  \
            _snprintf_s(out + used, cap - used, _TRUNCATE, "%s", part);        \
            used = strlen(out);                                                \
        } while (0)

    if (what & WR_LABEL_SPEED)
    {
        // Horizontal only: that is what a surfer is trying to carry through a
        // dip, and the vertical part is about to be traded for height anyway.
        float sp = sqrtf(theirs->vel.x * theirs->vel.x +
                         theirs->vel.y * theirs->vel.y);
        APPEND("%.0f", sp);
    }

    if (what & WR_LABEL_ENERGY)
    {
        float e = WrEnergyOf(theirs->pos, theirs->vel);
        if (WrEnergyHaveRef())
            e -= WrEnergyRefZ() - g_energy.eyeHeight;   // their points are feet
        APPEND("E %.0f", e);
    }

    if ((what & WR_LABEL_TIME) && t >= 0.0f && timeTrusted)
    {
        int mins = (int)(t / 60.0f);
        float secs = t - mins * 60.0f;
        if (mins > 0) APPEND("%d:%05.2f", mins, secs);
        else          APPEND("%.2f", secs);
    }

    if (what & WR_LABEL_DELTA)
    {
        // Yours minus theirs, from your own recorded line -- so it appears only
        // once you have actually been here, and says nothing until then rather
        // than guessing.
        const WrPoint *mine = WrLiveNearest(theirs->pos, 256.0f);
        if (mine)
        {
            float mySp = sqrtf(mine->vel.x * mine->vel.x +
                               mine->vel.y * mine->vel.y);
            float theirSp = sqrtf(theirs->vel.x * theirs->vel.x +
                                  theirs->vel.y * theirs->vel.y);
            if (t >= 0.0f && timeTrusted && mine->t > 0.0f)
                APPEND("%+.0f  %+.2fs", mySp - theirSp, mine->t - t);
            else
                APPEND("%+.0f", mySp - theirSp);
        }
    }
    #undef APPEND
}

// Draw a built label at a screen point, reserving room for it. Returns false if
// it was dropped -- out of budget, or something is already there.
static bool DrawLabel(ImDrawList *dl, ImVec2 at, const char *text,
                      unsigned int colour)
{
    if (!text || !text[0])
        return false;
    if (g_statLabels >= g_render.maxLabelsPerFrame)
        return false;

    float ls = 0.0f;
    ImFont *lf = WrFontFor(14.0f * g_render.tagScale, &ls);
    ImVec2 m = lf->CalcTextSizeA(ls, FLT_MAX, 0.0f, text);

    TagRect r;
    r.x0 = at.x; r.y0 = at.y;
    r.x1 = at.x + m.x + 2.0f; r.y1 = at.y + m.y + 2.0f;
    if (!ReserveRect(r))
        return false;

    dl->AddText(lf, ls, ImVec2(at.x + 1.0f, at.y + 1.0f), 0xC0000000u, text);
    dl->AddText(lf, ls, at, colour, text);
    g_statLabels++;
    return true;
}

// Numbers at the turning points of a line: the bottom of each ramp, where it
// stops falling and starts climbing, and the top of each arc, where it stops
// climbing and starts falling. Drawn in the run's own colour so a number can be
// traced back to a line when several are on screen.
//
// One function for both because they differ in exactly two things -- which
// index list to walk, and which way the tick points -- and having written them
// separately once, the second copy immediately drifted.
static void EmitTurns(ImDrawList *dl, const WrRun *run, bool tops)
{
    bool on = tops ? g_render.drawPeaks : g_render.drawDipSpeeds;
    unsigned int what = tops ? g_render.peakLabel : g_render.dipLabel;
    const int *list = tops ? run->peaks : run->dips;
    int listCount = tops ? run->peakCount : run->dipCount;
    int budget = tops ? g_render.maxPeaksPerRun : g_render.maxDipsPerRun;

    if (!on || !what)
        return;
    if (!list || listCount <= 0)
        return;

    const float maxDistSqr = g_render.maxDrawDistance * g_render.maxDrawDistance;
    int drawn = 0;

    // The per-frame label budget belongs in the guard, not just inside DrawLabel.
    //
    // `drawn` only advances when a label is actually placed, so once the global
    // budget is spent DrawLabel refuses every call, `drawn` stops moving, and
    // this loop walks the entire dip list of every remaining run -- distance
    // test, projection, and a formatted string per point, all discarded on
    // arrival. Invisible at eight drawn runs. At 256 it is around half a
    // millisecond a frame spent building text nothing will show.
    for (int i = 0; i < listCount && drawn < budget &&
                    g_statLabels < g_render.maxLabelsPerFrame; i++)
    {
        int idx = list[i];
        if (idx < 0 || idx >= run->pointCount)
            continue;

        const WrPoint *p = &run->points[idx];
        if (WrDistSqr(p->pos, g_cam) > maxDistSqr)
            continue;

        ImVec2 s;
        if (!Project(p->pos, &s))
            continue;
        if (s.x < 0.0f || s.x > g_sw || s.y < 0.0f || s.y > g_sh)
            continue;

        // A dip's time is index-derived rather than measured, so it is only
        // offered on runs whose recovered timing survived the trust test --
        // 0.36x to 10.32x on the worst map measured. See CheckTimes.
        char label[128];
        BuildLabel(label, sizeof(label), what, p,
                   WrRunTimeAt(run, idx), run->timingTrusted);
        if (!label[0])
            continue;

        // A short tick so the number is clearly attached to this point on the
        // line rather than floating near it, and pointing the way the label
        // sits: down from a top, up from a bottom. Without that the two kinds
        // of number are indistinguishable once several are on screen, which is
        // the whole reason for having both.
        float side = tops ? 1.0f : -1.0f;
        ImVec2 tp(s.x + 4.0f, tops ? s.y + 9.0f : s.y - 16.0f);
        if (!DrawLabel(dl, tp, label, WithAlpha(WrRunColour(run), 1.0f)))
            continue;
        dl->AddLine(ImVec2(s.x, s.y), ImVec2(s.x, s.y + 7.0f * side),
                    WithAlpha(WrRunColour(run), 0.9f), 1.5f);
        drawn++;
    }
}

// Where the comparison is actually reading from.
//
// Every "+240 vs .x" on screen is your energy against theirs AT ONE POINT of
// their line -- the point of it nearest you, picked fresh every frame. That
// point was invisible, so a number that jumped had no explanation: you could
// not tell a real difference from the reference having slid twenty units along
// a ramp, or from it having latched onto the wrong line entirely where two
// routes cross.
//
// So it is drawn: a ring on their line at exactly the point being read, and a
// thin leader from you to it. The leader is the part that matters when several
// runs are enabled, because it says WHICH line won the comparison.
static void EmitComparePoint(ImDrawList *dl)
{
    if (!g_energy.showComparePoint)
        return;

    const WrRun *ref = WrEnergyReferenceRun();
    if (!ref || ref->nearestIndex < 0 || ref->nearestIndex >= ref->pointCount)
        return;

    const Vec3 &p = ref->points[ref->nearestIndex].pos;
    ImVec2 s;
    if (!Project(p, &s))
        return;

    unsigned int col = WithAlpha(WrRunColour(ref), 1.0f);

    // From your midsection, the same origin the velocity vector uses, so the
    // two read as coming from the same place -- you.
    if (g_energy.comparePointLeader)
    {
        Vec3 a = g_cam;
        a.z -= (g_energy.eyeHeight - 36.0f);
        Vec3 b = p;
        ImVec2 pa, pb;
        if (ClipToNear(&a, &b) && Project(a, &pa) && Project(b, &pb))
            dl->AddLine(pa, pb, WithAlpha(WrRunColour(ref), 0.35f), 1.0f);
    }

    // A ring rather than a disc: it has to sit ON the line without hiding the
    // stretch of it you are trying to read.
    float r = 7.0f;
    dl->AddCircle(s, r + 1.0f, 0xC0000000u, 20, 3.0f);      // dark halo
    dl->AddCircle(s, r, col, 20, 2.0f);
    dl->AddCircleFilled(s, 2.0f, col, 8);
}

static void EmitMarkers(ImDrawList *dl, const WrRun *run)
{
    if (!g_render.drawMarkers || run->markerCount <= 0)
        return;
    if (!(run->flags & WRPATH_FLAG_MARKERS_OK))
        return;     // anchoring was not trusted; better nothing than wrong

    int drawn = 0;
    for (int i = 0; i < run->markerCount && drawn < g_render.maxMarkersPerRun; i++)
    {
        const WrMarker *mk = &run->markers[i];
        if (mk->pointIndex >= (unsigned int)run->pointCount)
            continue;
        Vec3 p = run->points[mk->pointIndex].pos;
        if (WrDistSqr(p, g_cam) > g_render.maxDrawDistance * g_render.maxDrawDistance)
            continue;

        ImVec2 s;
        if (!Project(p, &s))
            continue;
        if (s.x < 0.0f || s.x > g_sw || s.y < 0.0f || s.y > g_sh)
            continue;

        float r = g_render.markerRadius;
        dl->AddCircleFilled(s, r, WithAlpha(WrRunColour(run), 0.95f), 16);
        // Dark ring so the marker survives a bright surf texture behind it.
        dl->AddCircle(s, r, 0xE0000000u, 16, 1.5f);
        drawn++;

        if (!g_render.markerLabel)
            continue;

        // A checkpoint carries the exact engine velocity it was crossed with,
        // straight out of the demo's own stats -- better than anything the path
        // can be differenced for, and it was parsed and then never read until
        // now. Its split time is measured rather than index-derived, so unlike a
        // ramp label this one is trustworthy whatever CheckTimes decided.
        WrPoint at;
        at.pos = p;
        at.vel = mk->vel;
        at.t = (float)mk->timeReached;

        char label[128];
        BuildLabel(label, sizeof(label), g_render.markerLabel, &at,
                   (float)mk->timeReached, true);
        if (!label[0])
            continue;

        ImVec2 tp(s.x + r + 3.0f, s.y - 7.0f);
        DrawLabel(dl, tp, label, WithAlpha(WrRunColour(run), 1.0f));
    }
}

// ---------------------------------------------------------------------------
// Player tags
// ---------------------------------------------------------------------------
//
// Anchored at the fade distance rather than at the nearest point or the start.
// Nearest-point tags pile up wherever lines converge, which on a surf map is
// exactly where you are looking; start-point tags are off in another stage half
// the time. Pinning them to a fixed distance ahead means each tag sits further
// along its own line, so forty runs fan out instead of stacking.

// The rectangle reservation these share with the line labels is up with
// EmitDips, since that is where the first thing to register lives.

// How far the anchor may move in one frame before we stop easing and just put
// it there. A genuine branch change should not slide across the map.
#define TAG_SNAP_UNITS 512.0f
#define TAG_SMOOTH 0.25f

// Keeping last frame's answer is worth this much: a previous anchor is kept
// unless the new candidate beats it by more than this fraction.
#define TAG_HYSTERESIS 0.15f

// Score a point as a tag anchor: how far its distance from the camera is from
// the distance we want it at.
//
// Behind-camera points are penalised rather than skipped. Skipping them made
// the candidate set change shape as you turned, so simply rotating on the spot
// could move the anchor -- which is the one thing a world-anchored tag must
// never do. A penalty keeps the choice stable and still prefers a visible point
// whenever one exists.
static inline float TagScore(const WrRun *run, int i, float want)
{
    const Vec3 &p = run->points[i].pos;
    float s = fabsf(WrDist(p, g_cam) - want);
    if (ClipW(p) < NEAR_W)
        s += 1e6f;
    return s;
}

// The point on this run at the fade distance.
//
// Coarse pass to seed, full-resolution refine in the bracket, then interpolate
// between the two points that straddle the wanted distance. Picking the best of
// 256 strided samples -- which is what this used to do -- quantises the anchor
// to roughly 200 world units on a long run, so the tag visibly hopped forward
// and back along the line as you moved. Same shape as WrUpdateNearest in
// wr_path.cpp, for the same reason.
static bool TagAnchor(WrRun *run, Vec3 *out)
{
    float want = g_render.maxDrawDistance * g_render.fadeStartFraction;
    if (want < 64.0f)
        want = 64.0f;

    int step = run->pointCount / 256;
    if (step < 1)
        step = 1;

    bool hadPrev = (run->tagIndex >= 0 && run->tagIndex < run->pointCount);

    float best = 1e18f;
    int bestIdx = -1;
    for (int i = 0; i < run->pointCount; i += step)
    {
        float s = TagScore(run, i, want);
        if (s < best)
        {
            best = s;
            bestIdx = i;
        }
    }
    if (bestIdx < 0)
        return false;

    // A switchback can put two stretches of the same path at the same distance.
    // Without this the anchor flips between them as the scores trade places.
    if (hadPrev)
    {
        float prev = TagScore(run, run->tagIndex, want);
        if (prev <= best + fabsf(best) * TAG_HYSTERESIS + 1.0f)
            bestIdx = run->tagIndex;
    }

    int lo = bestIdx - step, hi = bestIdx + step;
    if (lo < 0) lo = 0;
    if (hi > run->pointCount - 1) hi = run->pointCount - 1;
    best = 1e18f;
    int refined = bestIdx;
    for (int i = lo; i <= hi; i++)
    {
        float s = TagScore(run, i, want);
        if (s < best)
        {
            best = s;
            refined = i;
        }
    }
    run->tagIndex = refined;

    // Sub-point: find the neighbour on the far side of `want` and lerp to the
    // exact crossing. Without this the anchor still steps by one point spacing,
    // which is small but visible on a fast strafe.
    Vec3 target = run->points[refined].pos;
    float dHere = WrDist(target, g_cam);
    int other = -1;
    if (refined + 1 < run->pointCount &&
        (WrDist(run->points[refined + 1].pos, g_cam) - want) * (dHere - want) < 0.0f)
        other = refined + 1;
    else if (refined - 1 >= 0 &&
             (WrDist(run->points[refined - 1].pos, g_cam) - want) * (dHere - want) < 0.0f)
        other = refined - 1;
    if (other >= 0)
    {
        float dOther = WrDist(run->points[other].pos, g_cam);
        float denom = dOther - dHere;
        if (fabsf(denom) > 1e-3f)
        {
            float f = WrClampF((want - dHere) / denom, 0.0f, 1.0f);
            target = WrAdd(target, WrScale(WrSub(run->points[other].pos, target), f));
        }
    }

    if (!hadPrev || !WrSaneVec(run->tagPos) ||
        WrDist(run->tagPos, target) > TAG_SNAP_UNITS)
        run->tagPos = target;
    else
        run->tagPos = WrAdd(run->tagPos,
                            WrScale(WrSub(target, run->tagPos), TAG_SMOOTH));

    *out = run->tagPos;
    return true;
}

static void EmitTag(ImDrawList *dl, WrRun *run)
{
    if (g_tagRectCount >= WR_MAX_TAG_RECTS || g_statTags >= g_render.maxTags)
        return;

    Vec3 anchor;
    if (!TagAnchor(run, &anchor))
        return;

    ImVec2 s;
    if (!Project(anchor, &s))
        return;
    if (s.x < -64.0f || s.x > g_sw + 64.0f || s.y < -32.0f || s.y > g_sh + 32.0f)
        return;

    // Queue the Steam lookup HERE, past the tag budget and the screen test, and
    // not in the draw loop where it used to sit under a comment claiming this is
    // what it did.
    //
    // The cache is 96 entries, it has no eviction, and nothing resets it for the
    // life of the process. From the draw loop it was fed by every drawn run, so
    // at the old default of eight it filled slowly and by accident; with 256 it
    // fills in a single frame the first time you tick All, and every player you
    // meet afterwards -- on this map or any later one -- gets no avatar for the
    // rest of the session. Twelve tags are drawn by default and 32 at the
    // slider's ceiling, so asking only for tags that actually render keeps the
    // 96 slots several times larger than anything that can consume them.
    WrSteamWant(run->steamId);

    const char *name = WrSteamPersona(run->steamId);
    if (!name || !*name)
        name = run->player[0] ? run->player : "(unknown)";

    int avatarSize = 0;
    void *avatar = NULL;
    if (g_render.tagAvatars)
        avatar = WrSteamAvatar(run->steamId, &avatarSize);

    float scale = g_render.tagScale;
    float icon = 22.0f * scale;
    float pad = 4.0f * scale;
    float tagSize = 0.0f;
    ImFont *tagFont = WrFontFor(14.0f * g_render.tagScale, &tagSize);
    ImVec2 text = tagFont->CalcTextSizeA(tagSize, FLT_MAX, 0.0f, name);
    float w = icon + pad * 3.0f + text.x;
    float h = (icon > text.y ? icon : text.y) + pad * 2.0f;

    // Nudge downward out of anything already placed this frame, then give up
    // rather than draw an unreadable pile.
    TagRect r;
    bool placed = false;
    float oy = 0.0f;
    for (int attempt = 0; attempt < WR_TAG_NUDGES; attempt++)
    {
        r.x0 = s.x + 10.0f;
        r.y0 = s.y - h * 0.5f + oy;
        r.x1 = r.x0 + w;
        r.y1 = r.y0 + h;
        if (!TagOverlaps(r))
        {
            placed = true;
            break;
        }
        oy += h + 2.0f;
    }
    if (!placed)
        return;

    g_tagRects[g_tagRectCount++] = r;
    g_statTags++;

    // Leader from the line to the tag, so a nudged tag still reads as belonging
    // to its own path.
    dl->AddLine(s, ImVec2(r.x0, r.y0 + h * 0.5f), WithAlpha(WrRunColour(run), 0.55f),
                1.0f);

    dl->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), 0xB0000000u,
                      4.0f * scale);
    dl->AddRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1),
                WithAlpha(WrRunColour(run), 0.85f), 4.0f * scale, 0, 1.5f);

    ImVec2 ic(r.x0 + pad, r.y0 + (h - icon) * 0.5f);
    if (avatar)
        dl->AddImage((ImTextureID)avatar, ic, ImVec2(ic.x + icon, ic.y + icon));
    else
        dl->AddCircleFilled(ImVec2(ic.x + icon * 0.5f, ic.y + icon * 0.5f),
                            icon * 0.42f, WithAlpha(WrRunColour(run), 1.0f), 14);

    ImVec2 tp(ic.x + icon + pad, r.y0 + (h - text.y) * 0.5f);
    dl->AddText(tagFont, tagSize, ImVec2(tp.x + 1.0f, tp.y + 1.0f), 0xC0000000u, name);
    dl->AddText(tagFont, tagSize, tp, 0xFFFFFFFFu, name);
}

// ---------------------------------------------------------------------------
// Energy readouts
// ---------------------------------------------------------------------------
//
// Two of them, and neither is a panel: these are numbers you watch while
// surfing, not something you open a menu to look at. The crosshair readout is
// the one you actually use; the corner block is the same figures plus the
// working, for when you are standing still and want the detail.

// The run to compare against: the fastest enabled one whose line is actually
// near you. Runs are already sorted fastest-first, so the first match is it.
//
// The distance test is not cosmetic. Momentum records a separate demo per stage,
// so on a staged map the fastest enabled run is frequently a different stage
// several thousand units away, and comparing your energy against a point on it
// produces a confident, meaningless number.
const WrRun *WrEnergyReferenceRun(void)
{
    for (int i = 0; i < WrRunCount(); i++)
    {
        WrRun *r = WrRunAt(i);
        if (!r || !r->enabled || r->nearestIndex < 0 ||
            r->nearestIndex >= r->pointCount)
            continue;
        if (r->nearestDist < 0.0f || r->nearestDist > g_energy.compareRadius)
            continue;
        return r;
    }
    return NULL;
}

// Their energy at the point of their line nearest you, and how much better or
// worse off you are.
//
// Both energies are absolute, which is the whole reason this works without
// aligning anything -- but their z is the player origin and yours is the eye, so
// one constant has to come off. See wr_energy.h.
static bool EnergyGap(const WrRun *ref, float *theirs, float *gap)
{
    if (!ref)
        return false;
    float t = WrEnergyOf(ref->points[ref->nearestIndex].pos,
                         ref->points[ref->nearestIndex].vel);
    if (theirs) *theirs = t;
    if (gap) *gap = (WrEnergyNow() - g_energy.eyeHeight) - t;
    return true;
}

// ---------------------------------------------------------------------------
// Crosshair readout
// ---------------------------------------------------------------------------
//
// The corner overlay turned out to be unreadable while surfing -- by the time
// your eye has travelled to it and back you have missed the ramp. This is the
// same information where you are already looking.

static void EmitEnergyHud(ImDrawList *dl)
{
    if (!g_energy.showHud || !WrEnergyValid())
        return;

    char big[48], sub[48], cmp[64];
    float rel = WrEnergyRelative();
    int dir = WrEnergyTrendDir();

    // The worst case each mode's two lines can produce. Reserved rather than
    // measured from this frame's text -- see the comment at the layout below,
    // which is the defect this exists to prevent.
    const char *wideBig = "-99999 v";
    const char *wideSub = "-99999 u/s";

    unsigned int bigCol = 0xFFFFFFFFu;
    WrEnergyBudget bud;
    bool haveBudget = WrEnergyBudgetNow(&bud);

    if (!WrEnergyHaveRef())
    {
        // Say so rather than print the number. With no anchor the reference is
        // wherever you are standing, so the relative figure is zero by
        // construction -- which looks exactly like a working readout that
        // thinks you have no energy, and is what "it doesn't detect the start"
        // looks like from the outside.
        dir = 0;
        bigCol = 0xFF909090u;
        _snprintf_s(big, sizeof(big), _TRUNCATE, "--");
        _snprintf_s(sub, sizeof(sub), _TRUNCATE, "no anchor");
    }
    else
    {
        // Every mode keeps the block at the same three lines and the same
        // colouring rule -- the arrow's rise/fall signal -- so switching modes
        // never changes what a colour means, only which numbers are shown.
        const char *arrow = (dir > 0) ? " ^" : (dir < 0 ? " v" : "");
        if (dir > 0)      bigCol = 0xFF80FF80u;   // ABGR
        else if (dir < 0) bigCol = 0xFF8080FFu;

        switch (g_energy.hudMode)
        {
        case WR_HUD_CARRIED:
            if (haveBudget && bud.carriedValid)
                _snprintf_s(big, sizeof(big), _TRUNCATE, "%.0f%%%s", bud.carried,
                            arrow);
            else
                _snprintf_s(big, sizeof(big), _TRUNCATE, "--%s", arrow);
            _snprintf_s(sub, sizeof(sub), _TRUNCATE, "sp %.0f  bk %.0f",
                        haveBudget ? bud.spent : 0.0f,
                        haveBudget ? bud.banked : 0.0f);
            wideBig = "-999% v";
            wideSub = "sp -99999  bk -99999";
            break;

        case WR_HUD_BUDGET:
            _snprintf_s(big, sizeof(big), _TRUNCATE, "sp %.0f%s",
                        haveBudget ? bud.spent : 0.0f, arrow);
            _snprintf_s(sub, sizeof(sub), _TRUNCATE, "bk %.0f  lost %.0f",
                        haveBudget ? bud.banked : 0.0f,
                        haveBudget ? bud.wasted : 0.0f);
            wideBig = "sp -99999 v";
            wideSub = "bk -99999  lost -99999";
            break;

        case WR_HUD_GAINED:
            _snprintf_s(big, sizeof(big), _TRUNCATE, "+%.0f%s", WrEnergyGained(),
                        arrow);
            _snprintf_s(sub, sizeof(sub), _TRUNCATE, "lost %.0f%s",
                        WrEnergyLost(),
                        WrEnergyBudgetSpliced() ? "  spliced" : "");
            wideBig = "+99999 v";
            wideSub = "lost 99999  spliced";
            break;

        default:
            _snprintf_s(big, sizeof(big), _TRUNCATE, "%.0f%s", rel, arrow);
            _snprintf_s(sub, sizeof(sub), _TRUNCATE, "%.0f u/s",
                        WrEnergyEquivSpeed());
            break;
        }
    }

    bool haveCmp = false;
    unsigned int cmpCol = 0xFFFFFFFFu;
    float barFrac = 0.0f;       // -1..+1, how far the bar leans
    bool barOver = false;       // the gap is off the end of the scale
    if (g_energy.compareToRun)
    {
        const WrRun *ref = WrEnergyReferenceRun();
        float gap = 0.0f;
        if (EnergyGap(ref, NULL, &gap))
        {
            const char *who = WrSteamPersona(ref->steamId);
            if (!who || !*who)
                who = ref->player[0] ? ref->player : "ref";
            _snprintf_s(cmp, sizeof(cmp), _TRUNCATE, "%+.0f vs %s", gap, who);
            cmpCol = gap >= 0.0f ? 0xFF66FF66u : 0xFF6666FFu;
            haveCmp = true;

            // The bar can measure either quantity. Energy is the default
            // because it discriminates better: across 28,243 matched samples a
            // slower run has more energy than the fastest one at 15.6% of
            // points but more horizontal speed at 20.4%.
            float value = gap, scale = g_energy.barMaxEnergy;
            if (g_energy.barMode == WR_BAR_SPEED)
            {
                const WrPoint *p = &ref->points[ref->nearestIndex];
                float theirSpeed = sqrtf(p->vel.x * p->vel.x +
                                         p->vel.y * p->vel.y);
                value = WrEnergyHorizontalSpeed() - theirSpeed;
                scale = g_energy.barMaxSpeed;
                _snprintf_s(cmp, sizeof(cmp), _TRUNCATE, "%+.0f u/s vs %s",
                            value, who);
                cmpCol = value >= 0.0f ? 0xFF66FF66u : 0xFF6666FFu;
            }
            if (scale < 1.0f)
                scale = 1.0f;
            barFrac = value / scale;
            barOver = (barFrac > 1.0f || barFrac < -1.0f);
            barFrac = WrClampF(barFrac, -1.0f, 1.0f);
        }
    }

    // Baked sizes, never scaled ones. Asking for 66 pixels from a 13-pixel atlas
    // is what made this blurry; WrFontFor returns the nearest size that actually
    // exists and the size to draw it at. See wr_imgui.cpp.
    float sBig = 0.0f, sSub = 0.0f;
    ImFont *fBig = WrFontFor(22.0f * g_energy.hudScale, &sBig);
    ImFont *fSub = WrFontFor(14.0f * g_energy.hudScale, &sSub);

    ImVec2 mBig = fBig->CalcTextSizeA(sBig, FLT_MAX, 0.0f, big);
    ImVec2 mSub = fSub->CalcTextSizeA(sSub, FLT_MAX, 0.0f, sub);
    ImVec2 mCmp = haveCmp ? fSub->CalcTextSizeA(sSub, FLT_MAX, 0.0f, cmp)
                          : ImVec2(0.0f, 0.0f);

    // Lay out on a WORST-CASE width, not on this frame's text.
    //
    // The block used to be measured from whatever it currently said. Its widest
    // line therefore changed as the numbers changed, and since a right-aligned
    // block is positioned by subtracting its width, the whole thing slid
    // sideways several times a second. Reserving room for the widest string any
    // of these lines can produce makes the position constant.
    // Per mode, since each shows different numbers. The block does step when you
    // deliberately switch modes, which is fine -- what must never happen is it
    // moving while the digits change.
    ImVec2 wBig = fBig->CalcTextSizeA(sBig, FLT_MAX, 0.0f, wideBig);
    ImVec2 wSub = fSub->CalcTextSizeA(sSub, FLT_MAX, 0.0f, wideSub);

    bool drawBar = haveCmp && g_energy.showBar;
    float barH = drawBar ? (g_energy.barHeight * g_energy.hudScale + 4.0f) : 0.0f;

    float w = wBig.x;
    if (wSub.x > w) w = wSub.x;
    if (mCmp.x > w) w = mCmp.x;     // the name is not ours to bound
    float h = mBig.y + mSub.y + (haveCmp ? mCmp.y : 0.0f) + barH;

    bool rightAlign = (g_energy.hudOffsetX < 0.0f);
    float x = g_sw * 0.5f + g_energy.hudOffsetX - (rightAlign ? w : 0.0f);
    float y = g_sh * 0.5f + g_energy.hudOffsetY - h * 0.5f;

    if (g_energy.hudBacking)
    {
        float p = 5.0f * g_energy.hudScale;
        dl->AddRectFilled(ImVec2(x - p, y - p), ImVec2(x + w + p, y + h + p),
                          0x70000000u, 4.0f);
    }

    float ty = y;
    struct { const char *s; float size; float width; unsigned int col; } rows[3];
    int n = 0;
    rows[n].s = big; rows[n].size = sBig; rows[n].width = mBig.x; rows[n].col = bigCol; n++;
    rows[n].s = sub; rows[n].size = sSub; rows[n].width = mSub.x; rows[n].col = 0xFFB0B0B0u; n++;
    if (haveCmp)
    {
        rows[n].s = cmp; rows[n].size = sSub; rows[n].width = mCmp.x; rows[n].col = cmpCol; n++;
    }

    // Held reads as dimmed, never as blank and never as hidden. A readout that
    // silently stops is indistinguishable from one that is stuck, which is the
    // exact complaint the fail-trigger latch produced.
    bool held = WrEnergyHeld();

    for (int i = 0; i < n; i++)
    {
        ImFont *f = (i == 0) ? fBig : fSub;
        float tx = rightAlign ? (x + w - rows[i].width) : x;
        unsigned int col = held ? MixColour(rows[i].col, 0.45f, 0.45f, 0.45f, 0.55f)
                                : rows[i].col;
        dl->AddText(f, rows[i].size, ImVec2(tx + 1.0f, ty + 1.0f), 0xC0000000u,
                    rows[i].s);
        dl->AddText(f, rows[i].size, ImVec2(tx, ty), col, rows[i].s);
        ty += (i == 0) ? mBig.y : mSub.y;
    }

    // The leaning bar. Centre-zero, filling toward whoever is ahead.
    //
    // Width is exactly the block's reserved width, so it can never change the
    // block's horizontal extent or move the text sideways -- the same invariant
    // the worst-case string reservation above exists to protect. The height is
    // added to `h` once, on toggle, not as the value changes.
    if (drawBar)
    {
        float by = y + h - barH + 2.0f;
        float bh = g_energy.barHeight * g_energy.hudScale;
        float cx = x + w * 0.5f;

        dl->AddRectFilled(ImVec2(x, by), ImVec2(x + w, by + bh), 0x50000000u,
                          2.0f);
        float ex = cx + barFrac * w * 0.5f;
        float x0 = (ex < cx) ? ex : cx, x1 = (ex < cx) ? cx : ex;
        unsigned int bc = held ? MixColour(cmpCol, 0.45f, 0.45f, 0.45f, 0.55f)
                               : cmpCol;
        dl->AddRectFilled(ImVec2(x0, by), ImVec2(x1, by + bh), bc, 2.0f);
        // Centre tick, drawn last so the fill never hides where zero is.
        dl->AddRectFilled(ImVec2(cx - 0.5f, by - 1.0f),
                          ImVec2(cx + 0.5f, by + bh + 1.0f), 0xFFB0B0B0u);

        // A pinned bar and a merely-large one look identical, and the gap runs
        // to 42,000 units at the 99th percentile because boosters exist. So an
        // overflow says so with an arrowhead past the end rather than silently
        // sitting at full deflection.
        if (barOver)
        {
            float t = bh * 0.6f;
            float tipX = (barFrac > 0.0f) ? (x + w + t) : (x - t);
            float baseX = (barFrac > 0.0f) ? (x + w) : x;
            dl->AddTriangleFilled(ImVec2(baseX, by), ImVec2(baseX, by + bh),
                                  ImVec2(tipX, by + bh * 0.5f), bc);
        }
    }

    if (held)
    {
        // Two bars, drawn from the block's own metrics so they scale with it.
        float bh = mSub.y * 0.7f, bw = bh * 0.28f;
        float bx = rightAlign ? (x - bw * 3.4f) : (x + w + bw * 1.4f);
        float by = y + (h - bh) * 0.5f;
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), 0xC0B0B0B0u);
        dl->AddRectFilled(ImVec2(bx + bw * 1.7f, by),
                          ImVec2(bx + bw * 2.7f, by + bh), 0xC0B0B0B0u);
    }
}

// Where you will be in a quarter of a second if nothing changes.
//
// Length maps to TIME, not to a bare scale factor. `speed * seconds` has a
// meaning the player can act on -- the tip lands on the surface they are about
// to meet -- whereas `speed * k` has no interpretation and every k is arbitrary.
//
// Measured across 76,897 sampled points on 17 maps, speeds run p50 2216, p90
// 3467, p99 4543 u/s, so a quarter second is 550-1140 units: long enough to
// reach the next ramp, short enough not to cross the level.
#define VECTOR_SECONDS 0.25f
#define VECTOR_MIN 64.0f
#define VECTOR_MAX 1200.0f

// Green where energy is being added near the physical ceiling, red where it is
// being thrown away, and the run's OWN colour where nothing is happening.
//
// Three things changed here after the first version was reported as making no
// sense, and only one of them was cosmetic.
//
// Neutral keeps the run's colour. Colouring by efficiency used to replace the
// line colour outright over its whole length, so turning the mode on destroyed
// every cue about whose line was whose -- and since a slow run sits near eta 0
// for a third of its length, the mode looked broken on exactly the runs a
// learner loads. Pulling neutral most of the way to grey keeps it obviously
// unsaturated while leaving the identity readable.
//
// The ramp is SYMMETRIC. It saturated at +0.6 but -0.5, which had no measured
// support in either direction once the loss side stopped being thrown away.
//
// And "no reading" is drawn as a faded version of the run's colour rather than
// as neutral. A booster, a gap across a teleport and the unmeasurable ends of a
// run are not free flight.
static unsigned int MixColour(unsigned int c, float r, float g, float b, float t)
{
    float cr = (float)(c & 0xFF) / 255.0f;
    float cg = (float)((c >> 8) & 0xFF) / 255.0f;
    float cb = (float)((c >> 16) & 0xFF) / 255.0f;
    float mr = cr + (r - cr) * t;
    float mg = cg + (g - cg) * t;
    float mb = cb + (b - cb) * t;
    unsigned int ri = (unsigned int)(WrClampF(mr, 0.0f, 1.0f) * 255.0f);
    unsigned int gi = (unsigned int)(WrClampF(mg, 0.0f, 1.0f) * 255.0f);
    unsigned int bi = (unsigned int)(WrClampF(mb, 0.0f, 1.0f) * 255.0f);
    return 0xFF000000u | (bi << 16) | (gi << 8) | ri;   // ImGui packs ABGR
}

static unsigned int EfficiencyColour(float eta, unsigned int runColour)
{
    float band = WrClampF(g_render.effNeutralBand, 0.0f, 0.9f);
    float sat = g_render.effSaturation;
    if (sat < band + 0.05f)
        sat = band + 0.05f;

    // Neutral: the run's colour, pulled most of the way to grey.
    float a = (eta < 0.0f) ? -eta : eta;
    if (a <= band)
        return MixColour(runColour, 0.5f, 0.5f, 0.5f, g_render.effNeutralMix);

    float u = WrClampF((a - band) / (sat - band), 0.0f, 1.0f);

    // Red/green is the worst possible pair for the ~8% of men with deuteranomaly,
    // so there is an alternative. Blue/orange survives every common form.
    float gr, gg, gb, lr, lg, lb;
    if (g_render.effColourblind)
    {
        gr = 0.20f; gg = 0.55f; gb = 1.00f;     // gaining: blue
        lr = 1.00f; lg = 0.55f; lb = 0.10f;     // losing:  orange
    }
    else
    {
        gr = 0.10f; gg = 1.00f; gb = 0.10f;
        lr = 1.00f; lg = 0.13f; lb = 0.10f;
    }

    unsigned int neutral =
        MixColour(runColour, 0.5f, 0.5f, 0.5f, g_render.effNeutralMix);
    if (eta > 0.0f)
        return MixColour(neutral, gr, gg, gb, u);
    return MixColour(neutral, lr, lg, lb, u);
}

// The fitted start zone, drawn so the guesswork is visible rather than magic.
//
// Three things, and the third is the point: a ring for the arming region, an
// arrow for the direction the runs left in, and the TRIGGER LINE with a band
// either side of it that is the measured p90 spread of where those runs' clocks
// actually started. The band is the uncertainty, drawn to scale, in the world.
// Anything that infers a place from data ought to show its error bars.
static void EmitStartZone(ImDrawList *dl)
{
    if (!g_start.enabled || !g_start.showZone)
        return;

    const WrStartZone *z = WrStartZoneHere();
    if (!z)
    {
        float d = 0.0f;
        z = WrStartZoneNearest(&d);
        // Only when it is close enough to be worth looking at. A ring drawn
        // across the map is a distraction, not information.
        if (!z || d > g_render.maxDrawDistance)
            return;
    }

    bool armed = (WrStartStateNow() == WR_START_ARMED);
    unsigned int ring = armed ? 0xFF66FF66u : 0xFFAAAAAAu;
    unsigned int band = 0x40FFFFFFu;
    float zBase = z->zLo + 128.0f;      // back to the members' own floor
    float zr = WrStartZoneRadius(z);    // fitted, with the size setting on

    // The ring, as a world-space polygon so it lies flat on the floor and
    // shrinks with distance like everything else here.
    const int kSegs = 48;
    ImVec2 pts[kSegs];
    int n = 0;
    bool broken = false;
    for (int i = 0; i < kSegs; i++)
    {
        float a = (float)i / (float)kSegs * 6.28318531f;
        Vec3 p = WrVec(z->centre.x + cosf(a) * zr,
                       z->centre.y + sinf(a) * zr, zBase);
        if (!Project(p, &pts[n]))
        {
            broken = true;      // wraps behind the camera; draw what is left
            if (n >= 2)
                dl->AddPolyline(pts, n, ring, 0, g_render.thickness);
            n = 0;
            continue;
        }
        n++;
    }
    if (n >= 2)
        dl->AddPolyline(pts, n, ring, broken ? 0 : ImDrawFlags_Closed,
                        g_render.thickness);

    if (WrLength(z->outDir) < 0.5f)
        return;                 // no usable heading: no line and no arrow

    // The trigger line: across the ring, through the centre, perpendicular to
    // the way out.
    Vec3 side = WrVec(-z->outDir.y, z->outDir.x, 0.0f);
    Vec3 c = WrVec(z->centre.x, z->centre.y, zBase);

    for (int k = -1; k <= 1; k++)
    {
        float off = (float)k * z->alongSpread;
        Vec3 a = WrAdd(WrAdd(c, WrScale(side, -zr)),
                       WrScale(z->outDir, off));
        Vec3 b = WrAdd(WrAdd(c, WrScale(side, zr)),
                       WrScale(z->outDir, off));
        ImVec2 pa, pb;
        if (!ClipToNear(&a, &b) || !Project(a, &pa) || !Project(b, &pb))
            continue;
        dl->AddLine(pa, pb, k == 0 ? ring : band,
                    g_render.thickness * (k == 0 ? 1.6f : 0.8f));
    }

    // And the way out, from the centre.
    {
        Vec3 a = c;
        Vec3 b = WrAdd(c, WrScale(z->outDir, zr * 0.9f));
        ImVec2 pa, pb;
        if (ClipToNear(&a, &b) && Project(a, &pa) && Project(b, &pb))
        {
            dl->AddLine(pa, pb, ring, g_render.thickness);
            for (int s = -1; s <= 1; s += 2)
            {
                Vec3 h0 = b;
                Vec3 h1 = WrAdd(b, WrAdd(WrScale(z->outDir, -48.0f),
                                         WrScale(side, 28.0f * (float)s)));
                ImVec2 q0, q1;
                if (ClipToNear(&h0, &h1) && Project(h0, &q0) && Project(h1, &q1))
                    dl->AddLine(q0, q1, ring, g_render.thickness);
            }
        }
    }
}

static void EmitVelocityVector(ImDrawList *dl)
{
    if (!g_render.drawVelocity)
        return;

    Vec3 v;
    if (!WrEnergyVelocity(&v))
        return;
    float speed = WrLength(v);
    if (speed < 1.0f)
        return;

    // From the midsection rather than the eye. The camera is eyeHeight above the
    // feet and a standing Source player is 72 tall, so the middle is about 36 up.
    Vec3 base = g_cam;
    base.z -= (g_energy.eyeHeight - 36.0f);

    float len = WrClampF(speed * VECTOR_SECONDS, VECTOR_MIN, VECTOR_MAX);
    Vec3 dir = WrScale(v, 1.0f / speed);
    Vec3 tip = WrAdd(base, WrScale(dir, len));

    // Coloured by the SAME signal as the arrow beside the crosshair -- energy
    // rising, steady, or falling -- and deliberately not by live efficiency.
    //
    // It used to be live efficiency, and that was measuring noise. The input is
    // a 0.30 s difference of a 0.30 s average of a camera-differenced velocity;
    // on a trajectory where energy is exactly constant it swings +-25 units/s at
    // 2000 u/s and +-40 at 3200, which saturates this ramp 14% and 36% of the
    // time respectively. Full colour needed a swing of 6.75 energy units -- less
    // than the 12-unit dead band the arrow already refuses to trust, and larger
    // than the 5-unit step the same number is rounded to for display.
    //
    // The 0.75 s trend sits safely above that noise, and using it means the
    // vector and the arrow can no longer disagree with each other.
    int trend = WrEnergyTrendDir();
    float vecEta = (trend > 0) ? g_render.effSaturation
                               : (trend < 0 ? -g_render.effSaturation : 0.0f);
    unsigned int col = EfficiencyColour(vecEta, 0xFFB0B0B0u);

    Vec3 a = base, b = tip;
    ImVec2 pa, pb;
    if (!ClipToNear(&a, &b) || !Project(a, &pa) || !Project(b, &pb))
        return;
    dl->AddLine(pa, pb, col, g_render.thickness * 1.4f);

    // The head is built in WORLD space, so it stays attached to the vector and
    // shrinks with distance. Built in screen space it would read as a separate
    // object stuck to the camera.
    // dir x up, by hand -- wr_common.h has no cross product and this is the
    // only place that wants one.
    Vec3 side = WrVec(dir.y * 1.0f - 0.0f, 0.0f - dir.x * 1.0f, 0.0f);
    if (WrLength(side) > 0.1f)
    {
        float hl = len * 0.18f;
        for (int s = -1; s <= 1; s += 2)
        {
            Vec3 back = WrAdd(WrScale(dir, -0.9f),
                              WrScale(WrNormalize(side), 0.42f * (float)s));
            Vec3 h0 = tip, h1 = WrAdd(tip, WrScale(WrNormalize(back), hl));
            ImVec2 q0, q1;
            if (ClipToNear(&h0, &h1) && Project(h0, &q0) && Project(h1, &q1))
                dl->AddLine(q0, q1, col, g_render.thickness * 1.4f);
        }
    }
}

// The key for the efficiency colours.
//
// There was none, anywhere, for any colour scheme in this tool -- and the
// reported problem with this one was not that the mapping was wrong but that
// nothing on screen said what it meant. A tooltip two tabs deep in a panel you
// close before you play is not an answer.
//
// It goes in the corner OPPOSITE the energy overlay, so the two cannot collide
// whichever corner that has been moved to.
// ONE key, whichever modes are on.
//
// It was a hardcoded four rows and three footers for the efficiency colours,
// with its height written out as `lh * 7.0f`. A second box for the rank colours
// had nowhere to go: it sits in the corner opposite the energy overlay, and with
// two of the four corners already claimed the third would eventually collide
// with something. So the rows are built rather than fixed, and both modes append
// to the same box.
static void EmitEfficiencyLegend(ImDrawList *dl)
{
    bool effOn = g_render.lineColour == WR_LINE_EFFICIENCY && g_render.lineKey;
    bool spdOn = g_render.lineColour == WR_LINE_SPEED && g_render.lineKey;
    bool nrgOn = g_render.lineColour == WR_LINE_ENERGY && g_render.lineKey;
    bool rankOn = g_render.rankColour != WR_RANK_OFF && g_render.rankLegend;
    if (!effOn && !spdOn && !nrgOn && !rankOn)
        return;

    float size = 0.0f;
    ImFont *font = WrFontFor(14.0f * g_render.tagScale, &size);
    float pad = 9.0f * g_render.tagScale;
    float lh = size * 1.35f;
    float sw = size * 1.6f;         // swatch width

    struct Row { unsigned int colour; bool dim; const char *text; };
    Row rows[12];
    const char *foots[8];
    int n = 0, nf = 0;

    // Held across the call because the rows point at them rather than copying.
    static char sLo[48], sMid[48], sHi[48];

    if (rankOn)
    {
        // The ramp swatches come from RankRampColour itself, so the key cannot
        // drift from the lines it is describing.
        rows[n].colour = WR_COL_FIRST;         rows[n].dim = false;
        rows[n++].text = "1st on this leg";
        rows[n].colour = RankRampColour(0.0f); rows[n].dim = false;
        rows[n++].text = "2nd";
        rows[n].colour = RankRampColour(1.0f); rows[n].dim = false;
        rows[n++].text = "last";
        foots[nf++] = (g_render.rankColour == WR_RANK_BY_TIME)
            ? "everyone behind: green to red by how far off the best they are"
            : "everyone behind: green to red by placing, evenly spread";
        // The thing that would otherwise look like a bug on a staged map.
        foots[nf++] = "placed within each leg -- a bonus cannot out-place a "
                      "main run";
    }

    // Speed and energy share SpeedColour's ramp, so both keys are built the same
    // way: the swatches are taken from the ramp function itself at the ends and
    // the middle, which is what stops the key from drifting away from the lines
    // it describes.
    if (spdOn || nrgOn)
    {
        float lo = nrgOn ? g_render.energyMin : g_render.speedMin;
        float hi = nrgOn ? g_render.energyMax : g_render.speedMax;
        const char *unit = nrgOn ? "units of height" : "u/s";
        _snprintf_s(sLo, sizeof(sLo), _TRUNCATE, "%.0f %s and below", lo, unit);
        _snprintf_s(sMid, sizeof(sMid), _TRUNCATE, "%.0f", (lo + hi) * 0.5f);
        _snprintf_s(sHi, sizeof(sHi), _TRUNCATE, "%.0f and above", hi);

        rows[n].colour = SpeedColour(lo);                 rows[n].dim = false;
        rows[n++].text = sLo;
        rows[n].colour = SpeedColour((lo + hi) * 0.5f);   rows[n].dim = false;
        rows[n++].text = sMid;
        rows[n].colour = SpeedColour(hi);                 rows[n].dim = false;
        rows[n++].text = sHi;

        if (nrgOn)
        {
            // The one thing that is not obvious about the energy mode, and the
            // reason it is worth having at all: it is an absolute height, so the
            // same colour on two different lines is the same energy.
            foots[nf++] = "energy = height + speed^2 / 2g, in world units";
            foots[nf++] = "absolute, so the same colour means the same energy on "
                          "every line";
        }
        else
        {
            foots[nf++] = "the range is yours to set -- fit it to the map and "
                          "the middle becomes readable";
        }
    }

    if (effOn)
    {
        static const float kEta[4] = { 1.0f, 0.0f, -1.0f, 0.0f };
        static const char *kText[4] = {
            "gaining -- strafing is adding energy",
            "nothing happening -- free flight",
            "losing -- a ramp entry, a wall, a landing",
            "no reading -- a booster, or a gap",
        };
        for (int i = 0; i < 4; i++)
        {
            bool noData = (i == 3);
            rows[n].colour = noData
                ? WithAlpha(0xFFB0B0B0u, g_render.effNoDataAlpha)
                : EfficiencyColour(kEta[i] * g_render.effSaturation, 0xFFB0B0B0u);
            rows[n].dim = noData;
            rows[n++].text = kText[i];
        }
        foots[nf++] = "full colour = 37 energy/s, all air strafing can add";
        // The two things people ask the moment this is on, because the same
        // red-and-green ramp is used for two different quantities and nothing
        // said so. They are ON THE LINES YOU ARE CHASING, and the arrow is a
        // TREND -- and the two disagreeing is not a fault, it is the
        // interesting case: you can strafe well while a ramp takes more than
        // you are adding.
        foots[nf++] = "demo lines only -- your own line cannot be measured "
                      "this finely";
        foots[nf++] = "the velocity arrow is your energy TREND, not this; they "
                      "differ on purpose";
    }

    float w = 0.0f;
    for (int i = 0; i < nf; i++)
    {
        float fw = font->CalcTextSizeA(size, FLT_MAX, 0.0f, foots[i]).x;
        if (fw > w) w = fw;
    }
    for (int i = 0; i < n; i++)
    {
        float lw = sw + pad + font->CalcTextSizeA(size, FLT_MAX, 0.0f,
                                                  rows[i].text).x;
        if (lw > w) w = lw;
    }
    float h = lh * (float)(n + nf) + pad * 2.0f;
    w += pad * 2.0f;

    // Opposite corner to the overlay, in both axes.
    float m = 18.0f;
    int corner = (~g_energy.overlayCorner) & 3;
    float x = (corner & 1) ? (g_sw - w - m) : m;
    float y = (corner & 2) ? (g_sh - h - m) : m;

    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), 0xA0000000u, 5.0f);

    // For the efficiency rows a neutral grey stands in for "some run's colour",
    // since the swatch shows what the ramp does and every run has its own base.
    // The rank swatches are the real thing -- a medal is not relative to
    // anything.
    for (int i = 0; i < n; i++)
    {
        float ry = y + pad + lh * i;
        dl->AddRectFilled(ImVec2(x + pad, ry + size * 0.15f),
                          ImVec2(x + pad + sw, ry + size * 0.95f),
                          rows[i].colour, 2.0f);
        ImVec2 p(x + pad + sw + pad, ry);
        dl->AddText(font, size, ImVec2(p.x + 1.0f, p.y + 1.0f), 0xC0000000u,
                    rows[i].text);
        dl->AddText(font, size, p, 0xFFE0E0E0u, rows[i].text);
    }
    for (int i = 0; i < nf; i++)
    {
        ImVec2 fp(x + pad, y + pad + lh * (float)(n + i));
        dl->AddText(font, size, ImVec2(fp.x + 1.0f, fp.y + 1.0f), 0xC0000000u,
                    foots[i]);
        dl->AddText(font, size, fp, 0xFF909090u, foots[i]);
    }
}

static void EmitEnergyOverlay(ImDrawList *dl)
{
    if (!g_energy.showOverlay || !WrEnergyValid())
        return;

    char lines[10][96];
    unsigned int cols[10];
    int n = 0;

    _snprintf_s(lines[n], sizeof(lines[0]), _TRUNCATE, "energy  %.0f  (%.0f u/s)",
                WrEnergyRelative(), WrEnergyEquivSpeed());
    cols[n++] = 0xFFFFFFFFu;

    _snprintf_s(lines[n], sizeof(lines[0]), _TRUNCATE, "speed   %.0f  (h %.0f)",
                WrEnergySpeed(), WrEnergyHorizontalSpeed());
    cols[n++] = 0xFFCCCCCCu;

    // The same three numbers the HUD can show, always visible here since the
    // corner block is the one you read while standing still.
    WrEnergyBudget bud;
    if (WrEnergyBudgetNow(&bud))
    {
        _snprintf_s(lines[n], sizeof(lines[0]), _TRUNCATE,
                    "spent %.0f  kept %.0f", bud.spent, bud.banked);
        cols[n++] = 0xFFCCCCCCu;

        if (bud.carriedValid)
            _snprintf_s(lines[n], sizeof(lines[0]), _TRUNCATE,
                        "carried %.0f%%  wasted %.0f", bud.carried, bud.wasted);
        else
            _snprintf_s(lines[n], sizeof(lines[0]), _TRUNCATE,
                        "carried --  wasted %.0f", bud.wasted);
        // Green when most of the drop survived as speed. 100% is not the target
        // -- it is the physical ceiling for a map that only descends.
        cols[n++] = (bud.carriedValid && bud.carried >= 80.0f) ? 0xFF66FF66u
                                                               : 0xFFCCCCCCu;
    }

    _snprintf_s(lines[n], sizeof(lines[0]), _TRUNCATE, "gained +%.0f  lost %.0f%s",
                WrEnergyGained(), WrEnergyLost(),
                WrEnergyBudgetSpliced() ? "  (spliced)" : "");
    cols[n++] = 0xFFAAAAAAu;

    if (WrEnergyHaveGround())
    {
        float d = WrEnergySinceGround();
        _snprintf_s(lines[n], sizeof(lines[0]), _TRUNCATE, "since jump %+.0f", d);
        cols[n++] = d >= 0.0f ? 0xFF66FF66u : 0xFF6666FFu;   // ABGR
    }

    float ds = WrEnergySinceStart();
    _snprintf_s(lines[n], sizeof(lines[0]), _TRUNCATE, "since start %+.0f", ds);
    cols[n++] = ds >= 0.0f ? 0xFF66FF66u : 0xFF6666FFu;

    const WrRun *ref = g_energy.compareToRun ? WrEnergyReferenceRun() : NULL;
    float theirs = 0.0f, gap = 0.0f;
    if (EnergyGap(ref, &theirs, &gap))
    {
        _snprintf_s(lines[n], sizeof(lines[0]), _TRUNCATE, "%s  %.0f  (%+.0f)",
                    ref->player[0] ? ref->player : "ref", theirs, gap);
        cols[n++] = gap >= 0.0f ? 0xFF66FF66u : 0xFF6666FFu;
    }

    // Box it so the numbers survive a bright skybox.
    //
    // The width is reserved from the LONGEST STRING THESE LINES CAN PRODUCE, not
    // from what they say this frame. In a bottom-right or top-right corner the
    // origin is `screen - width`, so a width that changed with the digits moved
    // the entire block several times a second and made it unreadable. With the
    // monospaced face and a fixed reservation the block is nailed down.
    float size = 0.0f;
    ImFont *font = WrFontFor(15.0f * g_energy.overlayScale, &size);
    float pad = 10.0f * g_energy.overlayScale;
    float lh = size * 1.35f;

    static const char *kWidest = "carried -999%  wasted -99999  (spliced)";
    float w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, kWidest).x;
    for (int i = 0; i < n; i++)
    {
        // A player name can be longer than anything we can predict.
        float lw = font->CalcTextSizeA(size, FLT_MAX, 0.0f, lines[i]).x;
        if (lw > w) w = lw;
    }
    float h = lh * n + pad * 2.0f;
    w += pad * 2.0f;

    float m = 18.0f;
    float x = (g_energy.overlayCorner & 1) ? (g_sw - w - m) : m;
    float y = (g_energy.overlayCorner & 2) ? (g_sh - h - m) : m;

    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), 0xA0000000u, 5.0f);
    for (int i = 0; i < n; i++)
    {
        ImVec2 p(x + pad, y + pad + lh * i);
        dl->AddText(font, size, ImVec2(p.x + 1.0f, p.y + 1.0f), 0xC0000000u, lines[i]);
        dl->AddText(font, size, p, cols[i], lines[i]);
    }
}

// ---------------------------------------------------------------------------
// Frame entry point
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Aiming at a line
// ---------------------------------------------------------------------------
//
// The problem is cost, not geometry. 256 drawn runs measures 8.1 ms against a
// 16.7 ms frame at 60 Hz, and EmitPath does at most pointBudget = 1500 chord
// iterations per run -- so about 21 ns each, which is one cache miss plus some
// arithmetic. Anything that touches points at a comparable rate costs a
// comparable share of 8 ms.
//
// That rules out the two obvious designs. Folding the test into EmitPath, where
// the screen coordinates are already in hand, would make pick accuracy a
// function of the point-budget slider -- the exact failure recorded at the top
// of EmitPath, where teleports re-derived from strided chords appeared and
// vanished as that slider moved. And a flat "sample N points per run" is either
// too coarse to identify a line on a 38 751-point run or too expensive.
//
// So: reject in bulk against precomputed bounds, project only what survives, and
// walk points at full resolution only on the winner. The same shape TagAnchor,
// MeasureNearest and WrUpdateNearest all already use.
//
// Two hard caps, because a bound that depends on the geometry being reasonable
// is not a bound. Six chunks per run and 8192 projections per frame: pathological
// geometry degrades the pick, it does not blow the frame.
#define WR_PICK_CHUNKS_PER_RUN 6
#define WR_PICK_MAX_PROJECTIONS 8192
#define WR_PICK_HYSTERESIS 0.20f
#define WR_PICK_FLOOR_PX 4.0f
#define WR_PICK_TIE_PX 6.0f
#define WR_PICK_REFINE 16

// One pick at a time, so this is a singleton rather than state on WrRun. On the
// run, a line that stopped being drawn -- or stopped being enabled -- would keep
// claiming the crosshair.
static struct
{
    int slot;               // store index, or -1
    int index;              // which point of it
    Vec3 pos;               // smoothed, for the ring
    float scorePx;
    float holdFor;
    int tied;
    bool dim;               // held over from a frame that had no answer
} g_pick = { -1, 0, { 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, 0, false };

// The best score seen for each of the nearest few RUNS, so the tie count is a
// count of lines and not of samples.
//
// Counting every sample within the tie window was the obvious version and it is
// wrong in a way that shows: one line running alongside another contributes a
// hundred samples, and the plate then claims a hundred other lines were just as
// close. The number is there to admit a coin toss, so it has to be countable.
#define WR_PICK_CAND 8
static struct { int slot; float score; } g_cand[WR_PICK_CAND];
static int g_candCount = 0;

static void CandNote(int slot, float score)
{
    for (int i = 0; i < g_candCount; i++)
        if (g_cand[i].slot == slot)
        {
            if (score < g_cand[i].score)
                g_cand[i].score = score;
            return;
        }
    if (g_candCount < WR_PICK_CAND)
    {
        g_cand[g_candCount].slot = slot;
        g_cand[g_candCount].score = score;
        g_candCount++;
        return;
    }
    // Full: replace the worst, but only if this is better than it. The array is
    // eight long and only ever read for a count, so an exact top-eight is more
    // than the answer needs.
    int worst = 0;
    for (int i = 1; i < g_candCount; i++)
        if (g_cand[i].score > g_cand[worst].score)
            worst = i;
    if (score < g_cand[worst].score)
    {
        g_cand[worst].slot = slot;
        g_cand[worst].score = score;
    }
}

static int g_pickChunks = 0, g_pickPoints = 0;
static float g_pickMs = 0.0f;

// Screen offset from the CROSSHAIR, not a screen position. ProjectKnownW adds
// half the screen size and this would immediately subtract it again, so the
// crosshair is simply made the origin.
static bool PickProject(const Vec3 &p, float *w, ImVec2 *off)
{
    float ww = ClipW(p);
    if (ww < NEAR_W)
        return false;
    ImVec2 s;
    if (!ProjectKnownW(p, ww, &s))
        return false;
    *w = ww;
    off->x = s.x - g_sw * 0.5f;
    off->y = s.y - g_sh * 0.5f;
    return true;
}

// Squared distance from the origin to a segment, plus where along it that was.
// Squared and unrooted: the winner takes the one sqrt at the end.
static float SegDistSqr(const ImVec2 &a, const ImVec2 &b, float *tOut)
{
    float dx = b.x - a.x, dy = b.y - a.y;
    float l2 = dx * dx + dy * dy;
    float t = 0.0f;
    if (l2 > 1e-6f)
    {
        t = -(a.x * dx + a.y * dy) / l2;
        t = WrClampF(t, 0.0f, 1.0f);
    }
    float px = a.x + dx * t, py = a.y + dy * t;
    if (tOut)
        *tOut = t;
    return px * px + py * py;
}

// Sphere against the aim cone. No divide, no sqrt.
static inline bool SphereNearRay(const Vec3 &c, float r, const Vec3 &fwd,
                                 float range, float tanHalf)
{
    Vec3 d = WrSub(c, g_cam);
    float along = WrDot(d, fwd);
    if (along < -r || along > range + r)
        return false;
    float perp2 = WrDot(d, d) - along * along;
    float lim = r + (along > 0.0f ? along * tanHalf : 0.0f);
    return perp2 <= lim * lim;
}

static void UpdatePick(void)
{
    LARGE_INTEGER pt0, pt1;
    QueryPerformanceCounter(&pt0);
    g_pickChunks = 0;
    g_pickPoints = 0;

    // Frame time for the hold. WrRenderWorld only runs on frames that draw, so
    // this is the interval between VISIBLE frames -- which is the right clock
    // for something whose whole job is to stay on screen a moment longer.
    static LARGE_INTEGER last = { 0 };
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = 0.0f;
    if (last.QuadPart)
        dt = WrClampF((float)((double)(now.QuadPart - last.QuadPart) / (double)Qpf()),
                      0.0f, 0.1f);
    last = now;

    Vec3 fwd;
    if (!g_render.pickEnabled || !WrCameraForward(&fwd))
    {
        g_pick.slot = -1;
        return;
    }

    float range = g_render.maxDrawDistance;
    // The cone the pick radius subtends, in world terms. |row0| of the matrix is
    // the horizontal focal length, so a radius in pixels becomes a slope here.
    float focal = WrLength(WrVec(g_m->m[0][0], g_m->m[0][1], g_m->m[0][2]));
    float tanHalf = (focal > 1e-6f && g_sw > 1.0f)
                  ? (g_render.pickRadiusPx * 2.0f / g_sw) / focal
                  : 0.15f;
    tanHalf += 0.02f;           // a little slack: the cone is a reject, not a test

    int bestSlot = -1, bestIndex = 0;
    float bestScore = 0.0f;
    g_candCount = 0;
    int projections = 0;
    int drawn = 0;

    for (int i = 0; i < WrRunCount() && drawn < g_render.maxRunsDrawn; i++)
    {
        WrRun *run = WrRunAt(i);
        // The identical gate the draw loop uses. A run that is not drawn must
        // not be pickable, or the readout names a line that is not on screen.
        if (!run || !run->enabled || run->pointCount < 2)
            continue;
        drawn++;

        if (run->boundRadius <= 0.0f || !run->chunks)
            continue;
        if (!SphereNearRay(run->boundCentre, run->boundRadius, fwd, range, tanHalf))
            continue;

        // Which chunks survive, nearest first, capped.
        int cand[WR_PICK_CHUNKS_PER_RUN];
        float candAlong[WR_PICK_CHUNKS_PER_RUN];
        int nCand = 0;
        int first = g_render.hidePreRoll ? run->startIndex : 0;
        int firstChunk = first / WR_PICK_CHUNK;

        for (int k = firstChunk; k < run->chunkCount; k++)
        {
            g_pickChunks++;
            const WrChunk &ch = run->chunks[k];
            if (!SphereNearRay(ch.c, ch.r, fwd, range, tanHalf))
                continue;
            float along = WrDot(WrSub(ch.c, g_cam), fwd);
            if (nCand < WR_PICK_CHUNKS_PER_RUN)
            {
                cand[nCand] = k;
                candAlong[nCand] = along;
                nCand++;
            }
            else
            {
                // Replace the furthest. Keeping the nearest is what makes the
                // cap a sensible truncation rather than an arbitrary one.
                int worst = 0;
                for (int a = 1; a < nCand; a++)
                    if (candAlong[a] > candAlong[worst])
                        worst = a;
                if (along < candAlong[worst])
                {
                    cand[worst] = k;
                    candAlong[worst] = along;
                }
            }
        }

        for (int c = 0; c < nCand && projections < WR_PICK_MAX_PROJECTIONS; c++)
        {
            int a = cand[c] * WR_PICK_CHUNK;
            int b = a + WR_PICK_CHUNK;
            if (b > run->pointCount)
                b = run->pointCount;
            if (a < first)
                a = first;
            int step = WR_PICK_CHUNK / 4;
            if (step < 1)
                step = 1;

            ImVec2 prev;
            float prevW = 0.0f;
            bool havePrev = false;
            for (int p = a; p < b && projections < WR_PICK_MAX_PROJECTIONS; p += step)
            {
                ImVec2 off;
                float w;
                projections++;
                g_pickPoints++;
                if (!PickProject(run->points[p].pos, &w, &off) || w > range)
                {
                    havePrev = false;       // straddles the near plane: see below
                    continue;
                }
                if (havePrev)
                {
                    // Chords straddling the near plane are rejected rather than
                    // clipped: one endpoint behind the camera projects mirrored
                    // and yields a bogus tiny distance -- the trap EmitPath
                    // documents. A pick at the near plane means nothing anyway.
                    float t = 0.0f;
                    float d2 = SegDistSqr(prev, off, &t);
                    float lim = g_render.pickRadiusPx;
                    if (d2 <= lim * lim)
                    {
                        float wAt = prevW + (w - prevW) * t;
                        // A far line must be meaningfully closer on screen to
                        // take the pick from a near one. Without this, two lines
                        // crossing on screen trade it on sub-pixel noise, which
                        // is precisely the situation the feature exists for.
                        float score = sqrtf(d2) +
                                      g_render.pickDepthBias * lim *
                                      (wAt / (range > 1.0f ? range : 1.0f));
                        CandNote(i, score);
                        if (bestSlot < 0 || score < bestScore)
                        {
                            bestScore = score;
                            bestSlot = i;
                            bestIndex = p;
                        }
                    }
                }
                prev = off;
                prevW = w;
                havePrev = true;
            }
        }
    }

    // Incumbent hysteresis, the same shape TagAnchor uses: last frame's answer
    // keeps the pick unless it is beaten by a real margin. Without it a line
    // crossing another swaps the readout every frame.
    if (bestSlot >= 0 && g_pick.slot >= 0 && g_pick.slot != bestSlot &&
        !g_pick.dim)
    {
        const WrRun *prev = WrRunAt(g_pick.slot);
        // pointCount is not enough on its own: FinishLoad qsorts the store, so
        // slot N can become a different, shorter run between frames and the
        // remembered index then points past its end. The refine loop below
        // would run zero iterations and leave bestIndex there.
        if (prev && prev->enabled && prev->pointCount >= 2 &&
            g_pick.index >= 0 && g_pick.index < prev->pointCount &&
            g_pick.scorePx <= bestScore * (1.0f + WR_PICK_HYSTERESIS) +
                              WR_PICK_FLOOR_PX)
        {
            bestSlot = g_pick.slot;
            bestIndex = g_pick.index;
            bestScore = g_pick.scorePx;
        }
    }

    if (bestSlot >= 0)
    {
        WrRun *run = WrRunAt(bestSlot);

        // Full resolution, winner only. The coarse pass steps a quarter of a
        // chunk, so the index can be out by eight points; this is 33 more
        // distance tests and fixes it.
        // Clamped before use, not just bounded: bestIndex reaches here either
        // from this frame's scan or from last frame's remembered pick, and only
        // one of those two is guaranteed to still be inside this run.
        bestIndex = WrClampI(bestIndex, 0, run->pointCount - 1);
        int lo = bestIndex - WR_PICK_REFINE, hi = bestIndex + WR_PICK_REFINE;
        if (lo < 0) lo = 0;
        if (hi > run->pointCount) hi = run->pointCount;
        float bestD = -1.0f;
        for (int p = lo; p < hi; p++)
        {
            ImVec2 off;
            float w;
            g_pickPoints++;
            if (!PickProject(run->points[p].pos, &w, &off))
                continue;
            float d = off.x * off.x + off.y * off.y;
            if (bestD < 0.0f || d < bestD)
            {
                bestD = d;
                bestIndex = p;
            }
        }

        Vec3 want = run->points[bestIndex].pos;
        if (g_pick.slot != bestSlot || WrDistSqr(want, g_pick.pos) >
            TAG_SNAP_UNITS * TAG_SNAP_UNITS)
            g_pick.pos = want;
        else
            g_pick.pos = WrAdd(g_pick.pos,
                               WrScale(WrSub(want, g_pick.pos), TAG_SMOOTH));

        g_pick.slot = bestSlot;
        g_pick.index = bestIndex;
        g_pick.scorePx = bestScore;

        // Distinct runs that came within a few pixels of the winner. Counted
        // here rather than while scanning, because the same run contributes
        // many samples and only one of them is that run.
        int tied = 0;
        for (int c = 0; c < g_candCount; c++)
            if (g_cand[c].slot != bestSlot &&
                g_cand[c].score < bestScore + WR_PICK_TIE_PX)
                tied++;
        g_pick.tied = tied;
        g_pick.holdFor = g_render.pickHoldSeconds;
        g_pick.dim = false;
    }
    else if (g_pick.slot >= 0)
    {
        // Held, DIMMED, for a moment. Dimming rather than blanking for the same
        // reason the energy readout dims when it is held: a display that
        // silently stops is indistinguishable from one that is stuck.
        g_pick.holdFor -= dt;
        g_pick.dim = true;
        const WrRun *run = WrRunAt(g_pick.slot);
        if (g_pick.holdFor <= 0.0f || !run || !run->enabled ||
            run->pointCount < 2 || g_pick.index >= run->pointCount)
            g_pick.slot = -1;
    }

    QueryPerformanceCounter(&pt1);
    g_pickMs = (float)((double)(pt1.QuadPart - pt0.QuadPart) * 1000.0 / (double)Qpf());
}

const WrRun *WrPickedRun(int *pointIndex, float *screenPx, int *tied)
{
    if (g_pick.slot < 0)
        return NULL;
    const WrRun *run = WrRunAt(g_pick.slot);
    if (!run || !run->enabled || g_pick.index >= run->pointCount)
        return NULL;
    if (pointIndex) *pointIndex = g_pick.index;
    if (screenPx) *screenPx = g_pick.scorePx;
    if (tied) *tied = g_pick.tied;
    return run;
}

void WrRenderPickReset(void)
{
    g_pick.slot = -1;
    g_pick.tied = 0;
    g_pick.holdFor = 0.0f;
    g_pick.dim = false;
    g_candCount = 0;
}

void WrPickStats(int *chunksTested, int *pointsTested, float *millis)
{
    if (chunksTested) *chunksTested = g_pickChunks;
    if (pointsTested) *pointsTested = g_pickPoints;
    if (millis) *millis = g_pickMs;
}

// The ring at the picked point, and the plate that says whose line it is.
static void EmitPickPlate(ImDrawList *dl)
{
    int idx = 0, tied = 0;
    float px = 0.0f;
    const WrRun *run = WrPickedRun(&idx, &px, &tied);
    if (!run)
        return;

    float fade = g_pick.dim ? 0.6f : 1.0f;
    unsigned int col = WithAlpha(WrRunColour(run), fade);

    ImVec2 at;
    bool onScreen = Project(g_pick.pos, &at);

    if (g_render.pickRing && onScreen)
    {
        // The same recipe EmitComparePoint uses, at a bigger radius so the two
        // read as different things when both land on one run.
        dl->AddCircle(at, 11.0f, WithAlpha(0xFF000000u, fade), 0, 3.0f);
        dl->AddCircle(at, 10.0f, col, 0, 2.0f);
        dl->AddCircleFilled(at, 2.5f, col);
    }

    // Eight, and every append below is bounded against it. The worst case is a
    // name, four rows from BuildLabel (its mask has four bits), the track and
    // the tie note -- seven. A five-element array was one label bit away from
    // being written past.
    char lines[8][96];
    unsigned int cols[8];
    const int kMaxLines = 8;
    int n = 0;

    const char *who = WrSteamPersona(run->steamId);
    if (!who || !*who)
        who = run->player[0] ? run->player : "(unknown)";
    _snprintf_s(lines[n], sizeof(lines[0]), _TRUNCATE, "%s", who);
    cols[n++] = WithAlpha(0xFFFFFFFFu, fade);

    // BuildLabel is the same formatter the numbers on the line use, which is why
    // it exists. It separates its parts with newlines, so they become rows here.
    char body[128];
    BuildLabel(body, sizeof(body), g_render.pickLabel, &run->points[idx],
               WrRunTimeAt(run, idx), run->timingTrusted);
    for (char *p = body; *p && n < kMaxLines - 2; )
    {
        char *nl = strchr(p, '\n');
        if (nl)
            *nl = '\0';
        if (*p)
        {
            _snprintf_s(lines[n], sizeof(lines[0]), _TRUNCATE, "%s", p);
            cols[n++] = WithAlpha(0xFFDDDDDDu, fade);
        }
        if (!nl)
            break;
        p = nl + 1;
    }

    if (n < kMaxLines)
    {
        if (run->rank > 0)
            _snprintf_s(lines[n], sizeof(lines[0]), _TRUNCATE, "%s  %d / %d",
                        WrTrackName(run), run->rank, run->rankOutOf);
        else
            _snprintf_s(lines[n], sizeof(lines[0]), _TRUNCATE, "%s",
                        WrTrackName(run));
        cols[n++] = col;
    }

    if (tied > 0 && n < kMaxLines)
    {
        // Said out loud, because the choice between overlapping lines was
        // effectively a coin toss and a confident name would be a lie.
        _snprintf_s(lines[n], sizeof(lines[0]), _TRUNCATE,
                    "%d other line%s just as close", tied, tied == 1 ? "" : "s");
        cols[n++] = WithAlpha(0xFF80B0FFu, fade);
    }

    float size = 0.0f;
    ImFont *font = WrFontFor(14.0f * g_render.tagScale, &size);
    float lh = size * 1.3f;
    float pad = 6.0f * g_render.tagScale;
    float w = 0.0f;
    for (int i = 0; i < n; i++)
    {
        ImVec2 m = font->CalcTextSizeA(size, FLT_MAX, 0.0f, lines[i]);
        if (m.x > w) w = m.x;
    }
    float h = lh * (float)n;

    // Anchored to the picked point when it is on screen, and to the crosshair
    // when it is not -- which happens while the hold is running out after you
    // have looked away.
    float ax = onScreen ? at.x : g_sw * 0.5f;
    float ay = onScreen ? at.y : g_sh * 0.5f;

    // Never on top of the crosshair. Pushed away from screen centre, so the
    // thing you are aiming at stays visible.
    float dx = ax - g_sw * 0.5f, dy = ay - g_sh * 0.5f;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1.0f) { dx = 0.0f; dy = 1.0f; len = 1.0f; }
    float ox = dx / len * 26.0f, oy = dy / len * 26.0f;

    float x = ax + ox;
    float y = ay + oy - h * 0.5f;
    if (x + w + pad * 2.0f > g_sw) x = g_sw - w - pad * 2.0f;
    if (x < 0.0f) x = 0.0f;
    if (y + h + pad * 2.0f > g_sh) y = g_sh - h - pad * 2.0f;
    if (y < 0.0f) y = 0.0f;

    // Drawn last of everything in the frame, so it is never under a name tag or
    // a ramp number. It does NOT reserve a TagRect: that would have to happen
    // before the run loop, and the plate's size is not known until the run has
    // been picked. Being opaque and on top is the cheaper answer to the same
    // problem -- the labels underneath are still readable either side of it.
    ImVec2 p0(x, y), p1(x + w + pad * 2.0f, y + h + pad * 2.0f);
    dl->AddRectFilled(p0, p1, WithAlpha(0xC0000000u, fade), 5.0f);
    dl->AddRect(p0, p1, col, 5.0f, 0, 1.5f);
    if (onScreen)
        dl->AddLine(at, ImVec2(x + (ox < 0.0f ? w + pad * 2.0f : 0.0f),
                               y + h * 0.5f + pad), col, 1.5f);

    for (int i = 0; i < n; i++)
    {
        ImVec2 tp(x + pad, y + pad + lh * (float)i);
        dl->AddText(font, size, ImVec2(tp.x + 1.0f, tp.y + 1.0f),
                    WithAlpha(0xC0000000u, fade), lines[i]);
        dl->AddText(font, size, tp, cols[i], lines[i]);
    }
}

void WrRenderWorld(void)
{
    g_statSegments = 0;
    g_statPoints = 0;
    g_statBatches = 0;
    g_batchCount = 0;
    g_tagRectCount = 0;
    g_statTags = 0;
    g_statLabels = 0;

    VMatrix m;
    if (!WrWorldToScreen(&m))
        return;
    if (!WrCameraOrigin(&g_cam))
        return;

    // Feeds the Runs tab's "near you" column, which is how you tell which leg of
    // a staged map a run belongs to.
    WrUpdateNearest(g_cam);

    // Anchor to where the reference run's RUN starts, once, so that its clock
    // and ours start at the same place -- see wr_timer.h. Only ever taken when
    // nothing has anchored yet, so a manual anchor is never overwritten.
    //
    // points[startIndex], not points[0]. The first recorded point is roughly
    // three quarters of a second of walking into the start zone earlier, so
    // anchoring there put our zero somewhere the run had not begun.
    if (g_energy.anchorToRunStart && WrEnergyAnchorSource() == WR_ANCHOR_NONE)
    {
        const WrRun *ref = WrEnergyReferenceRun();
        if (ref && ref->pointCount >= 2)
            WrEnergyAnchorToFeet(ref->points[ref->startIndex].pos);
    }

    int bw = 0, bh = 0;
    WrBackbufferSize(&bw, &bh);
    if (bw <= 0 || bh <= 0)
        return;

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    g_m = &m;
    g_sw = (float)bw;
    g_sh = (float)bh;

    ImDrawList *dl = ImGui::GetBackgroundDrawList();

    // Before the draw loop: the pick decides which run is drawn last, and the
    // panel reads the answer in the same frame.
    UpdatePick();
    int pickSlot = -1;
    {
        int pi = 0;
        if (WrPickedRun(&pi, NULL, NULL))
            pickSlot = g_pick.slot;
    }

    int drawn = 0;
    for (int i = 0; i < WrRunCount() && drawn < g_render.maxRunsDrawn; i++)
    {
        WrRun *run = WrRunAt(i);
        if (!run || !run->enabled || run->pointCount < 2)
            continue;
        if (i == pickSlot)
        {
            // Drawn after the loop instead, so the highlighted line sits on top
            // of the ones crossing it. Still counted, so turning the pick on
            // cannot change how many runs get drawn.
            drawn++;
            continue;
        }
        WrPathDraw d;
        d.pts = run->points;
        d.count = run->pointCount;
        d.first = g_render.hidePreRoll ? run->startIndex : 0;
        d.breaks = run->breaks;
        d.breakCount = run->breakCount;
        d.eff = run->eff;
        d.baseColour = WrRunColour(run);
        d.velScale = 1.0f;
        d.thickness = g_render.thickness;
        d.lift = 0.0f;
        EmitPath(dl, d);
        EmitTurns(dl, run, false);
        EmitTurns(dl, run, true);
        EmitMarkers(dl, run);
        if (g_render.drawTags)
            EmitTag(dl, run);       // which queues the Steam lookup, if it draws
        drawn++;
    }

    // The picked run, last and on top. No second pass over its points: the same
    // EmitPath, with a thickness and a lift, so the highlight costs nothing that
    // drawing it normally would not have cost anyway.
    if (pickSlot >= 0)
    {
        WrRun *run = WrRunAt(pickSlot);
        if (run && run->enabled && run->pointCount >= 2)
        {
            WrPathDraw d;
            d.pts = run->points;
            d.count = run->pointCount;
            d.first = g_render.hidePreRoll ? run->startIndex : 0;
            d.breaks = run->breaks;
            d.breakCount = run->breakCount;
            d.eff = run->eff;
            d.baseColour = WrRunColour(run);
            d.velScale = 1.0f;
            d.thickness = g_render.thickness * g_render.pickThickBoost;
            d.lift = 0.35f;
            EmitPath(dl, d);
            EmitTurns(dl, run, false);
            EmitTurns(dl, run, true);
            EmitMarkers(dl, run);
            // No name tag for this one: the plate already carries the name, and
            // suppressing it frees a rectangle for something else.
        }
    }

    EmitEnergyOverlay(dl);
    EmitEfficiencyLegend(dl);
    EmitStartZone(dl);
    EmitComparePoint(dl);
    EmitVelocityVector(dl);
    EmitEnergyHud(dl);
    EmitPickPlate(dl);      // last, so it is never drawn under anything

    if (g_render.drawLive)
    {
        int n = 0;
        const WrPoint *live = WrLivePoints(&n);
        // No break list: WrLiveRecord restarts the buffer on any move over 512
        // units, so the live trail cannot contain a teleport by construction.
        //
        // velScale is 1.0 now, not 0: live points carry a real velocity since
        // WrLiveRecord started being handed one, so colour-by-speed works on
        // your own line as well as on everybody else's.
        if (n >= 2)
        {
            WrPathDraw d;
            d.pts = live;
            d.count = n;
            d.first = 0;            // your own line has no pre-roll to hide
            d.breaks = NULL;
            d.breakCount = 0;
            d.eff = NULL;
            d.baseColour = g_render.liveColour;
            d.velScale = 1.0f;
            d.thickness = g_render.thickness;
            d.lift = 0.0f;
            EmitPath(dl, d);
        }
    }

    QueryPerformanceCounter(&t1);
    g_statMillis = (float)((double)(t1.QuadPart - t0.QuadPart) * 1000.0 /
                           (double)Qpf());
    g_stageMs[WR_STAGE_EMIT] = g_stageMs[WR_STAGE_EMIT] * 0.95f + g_statMillis * 0.05f;
}
