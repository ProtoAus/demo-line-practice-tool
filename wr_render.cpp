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
    g_render.maxRunsDrawn = 8;
    g_render.pointBudget = 1500;
    g_render.colourBySpeed = false;
    g_render.speedMin = 250.0f;
    g_render.speedMax = 3500.0f;
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
    g_render.drawVelocity = true;
    g_render.colourByEfficiency = false;
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
static unsigned int EfficiencyColour(float eta);

static void EmitPath(ImDrawList *dl, const WrPoint *pts, int count,
                     const int *breaks, int breakCount,
                     unsigned int baseColour, float velScale,
                     const signed char *eff)
{
    if (count < 2)
        return;

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
    if (g_render.pointBudget > 16 && count > g_render.pointBudget)
        step = (count + g_render.pointBudget - 1) / g_render.pointBudget;

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

    for (int i = 0; i + step < count; i += step)
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
                FlushBatch(dl, lastColour, g_render.thickness);
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
                FlushBatch(dl, lastColour, g_render.thickness);
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
                FlushBatch(dl, lastColour, g_render.thickness);
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
            FlushBatch(dl, lastColour, g_render.thickness);
            have = false;
        }

        ImVec2 pa, pb;
        float wa = ClipW(a), wb = ClipW(b);
        if (!ProjectKnownW(a, wa, &pa) || !ProjectKnownW(b, wb, &pb))
        {
            if (have)
            {
                FlushBatch(dl, lastColour, g_render.thickness);
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
                    FlushBatch(dl, lastColour, g_render.thickness);
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
        if (g_render.colourByEfficiency && eff)
        {
            float eta = WrEtaFromByte(eff[i]);
            int cBucket = (int)((eta * 0.5f + 0.5f) * (COLOUR_BUCKETS - 1) + 0.5f);
            if (cBucket < 0) cBucket = 0;
            if (cBucket >= COLOUR_BUCKETS) cBucket = COLOUR_BUCKETS - 1;
            float q = ((float)cBucket / (COLOUR_BUCKETS - 1)) * 2.0f - 1.0f;
            colour = WithAlpha(EfficiencyColour(q), a01);
            bucket = fBucket * COLOUR_BUCKETS + cBucket;
        }
        else if (g_render.colourBySpeed && velScale > 0.0f)
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
        else
        {
            colour = WithAlpha(baseColour, a01);
            bucket = fBucket;
        }

        if (have && bucket != lastBucket)
        {
            FlushBatch(dl, lastColour, g_render.thickness);
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
            FlushBatch(dl, colour, g_render.thickness);
            g_batch[g_batchCount++] = carry;    // keep the line continuous
            have = true;
        }
    }

    if (have)
        FlushBatch(dl, lastColour, g_render.thickness);
}

// Horizontal speed at the bottom of each ramp.
//
// Only the XY component, because that is what a surfer is trying to carry
// through the dip -- the vertical part is about to be traded for height anyway.
// Drawn in the run's own colour so a number can be traced back to a line when
// several are on screen.
static void EmitDips(ImDrawList *dl, const WrRun *run)
{
    if (!g_render.drawDipSpeeds || !run->dips || run->dipCount <= 0)
        return;

    const float maxDistSqr = g_render.maxDrawDistance * g_render.maxDrawDistance;
    int drawn = 0;

    for (int i = 0; i < run->dipCount && drawn < g_render.maxDipsPerRun; i++)
    {
        int idx = run->dips[i];
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

        float speed = sqrtf(p->vel.x * p->vel.x + p->vel.y * p->vel.y);
        char label[32];
        _snprintf_s(label, sizeof(label), _TRUNCATE, "%.0f", speed);

        // A short tick so the number is clearly attached to this point on the
        // line rather than floating near it.
        dl->AddLine(ImVec2(s.x, s.y), ImVec2(s.x, s.y - 7.0f),
                    WithAlpha(run->colour, 0.9f), 1.5f);

        ImVec2 tp(s.x + 4.0f, s.y - 16.0f);
        float ls = 0.0f;
        ImFont *lf = WrFontFor(14.0f * g_render.tagScale, &ls);
        dl->AddText(lf, ls, ImVec2(tp.x + 1.0f, tp.y + 1.0f), 0xC0000000u, label);
        dl->AddText(lf, ls, tp, WithAlpha(run->colour, 1.0f), label);
        drawn++;
    }
}

static void EmitMarkers(ImDrawList *dl, const WrRun *run)
{
    if (!g_render.drawMarkers || run->markerCount <= 0)
        return;
    if (!(run->flags & WRPATH_FLAG_MARKERS_OK))
        return;     // anchoring was not trusted; better nothing than wrong

    for (int i = 0; i < run->markerCount; i++)
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
        dl->AddCircleFilled(s, r, WithAlpha(run->colour, 0.95f), 16);
        // Dark ring so the marker survives a bright surf texture behind it.
        dl->AddCircle(s, r, 0xE0000000u, 16, 1.5f);

        char label[64];
        int mins = (int)(mk->timeReached / 60.0);
        double secs = mk->timeReached - mins * 60.0;
        if (mins > 0)
            _snprintf_s(label, sizeof(label), _TRUNCATE, "%d:%05.2f", mins, secs);
        else
            _snprintf_s(label, sizeof(label), _TRUNCATE, "%.2f", secs);

        ImVec2 tp(s.x + r + 3.0f, s.y - 7.0f);
        float ls = 0.0f;
        ImFont *lf = WrFontFor(14.0f * g_render.tagScale, &ls);
        dl->AddText(lf, ls, ImVec2(tp.x + 1.0f, tp.y + 1.0f), 0xC0000000u, label);
        dl->AddText(lf, ls, tp, WithAlpha(run->colour, 1.0f), label);
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

#define WR_MAX_TAG_RECTS 64
#define WR_TAG_NUDGES 6

struct TagRect { float x0, y0, x1, y1; };
static TagRect g_tagRects[WR_MAX_TAG_RECTS];
static int g_tagRectCount = 0;
static int g_statTags = 0;

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
    dl->AddLine(s, ImVec2(r.x0, r.y0 + h * 0.5f), WithAlpha(run->colour, 0.55f),
                1.0f);

    dl->AddRectFilled(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1), 0xB0000000u,
                      4.0f * scale);
    dl->AddRect(ImVec2(r.x0, r.y0), ImVec2(r.x1, r.y1),
                WithAlpha(run->colour, 0.85f), 4.0f * scale, 0, 1.5f);

    ImVec2 ic(r.x0 + pad, r.y0 + (h - icon) * 0.5f);
    if (avatar)
        dl->AddImage((ImTextureID)avatar, ic, ImVec2(ic.x + icon, ic.y + icon));
    else
        dl->AddCircleFilled(ImVec2(ic.x + icon * 0.5f, ic.y + icon * 0.5f),
                            icon * 0.42f, WithAlpha(run->colour, 1.0f), 14);

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

    const char *arrow = (dir > 0) ? " ^" : (dir < 0 ? " v" : "");
    _snprintf_s(big, sizeof(big), _TRUNCATE, "%.0f%s", rel, arrow);
    _snprintf_s(sub, sizeof(sub), _TRUNCATE, "%.0f u/s", WrEnergyEquivSpeed());

    unsigned int bigCol = 0xFFFFFFFFu;
    if (dir > 0)      bigCol = 0xFF80FF80u;   // ABGR
    else if (dir < 0) bigCol = 0xFF8080FFu;

    bool haveCmp = false;
    unsigned int cmpCol = 0xFFFFFFFFu;
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
    ImVec2 wBig = fBig->CalcTextSizeA(sBig, FLT_MAX, 0.0f, "-99999 v");
    ImVec2 wSub = fSub->CalcTextSizeA(sSub, FLT_MAX, 0.0f, "-99999 u/s");

    float w = wBig.x;
    if (wSub.x > w) w = wSub.x;
    if (mCmp.x > w) w = mCmp.x;     // the name is not ours to bound
    float h = mBig.y + mSub.y + (haveCmp ? mCmp.y : 0.0f);

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

    for (int i = 0; i < n; i++)
    {
        ImFont *f = (i == 0) ? fBig : fSub;
        float tx = rightAlign ? (x + w - rows[i].width) : x;
        dl->AddText(f, rows[i].size, ImVec2(tx + 1.0f, ty + 1.0f), 0xC0000000u,
                    rows[i].s);
        dl->AddText(f, rows[i].size, ImVec2(tx, ty), rows[i].col, rows[i].s);
        ty += (i == 0) ? mBig.y : mSub.y;
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
// being thrown away, grey in between and for free flight.
//
// The thresholds come from the data, not from taste: median efficiency on the
// surf_demise world record is +0.531, and on the two slowest runs in the same
// set (55.9 s and 57.8 s) it is +0.010 and +0.003. A factor of fifty separates a
// record from a bad run, and it all lives in the positive band -- so that is
// where the resolution goes.
static unsigned int EfficiencyColour(float eta)
{
    float r, g, b;
    if (eta >= 0.0f)
    {
        float u = WrClampF(eta / 0.6f, 0.0f, 1.0f);     // 0.6 is already excellent
        r = 0.65f * (1.0f - u);
        g = 0.65f + 0.35f * u;
        b = 0.65f * (1.0f - u);
    }
    else
    {
        float u = WrClampF(-eta / 0.5f, 0.0f, 1.0f);
        r = 0.65f + 0.35f * u;
        g = 0.65f * (1.0f - u);
        b = 0.65f * (1.0f - u) * 0.4f;
    }
    unsigned int ri = (unsigned int)(WrClampF(r, 0.0f, 1.0f) * 255.0f);
    unsigned int gi = (unsigned int)(WrClampF(g, 0.0f, 1.0f) * 255.0f);
    unsigned int bi = (unsigned int)(WrClampF(b, 0.0f, 1.0f) * 255.0f);
    return 0xFF000000u | (bi << 16) | (gi << 8) | ri;   // ImGui packs ABGR
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

    // Colour it by the same efficiency ramp the lines use, so the vector and the
    // record line you are chasing are directly comparable.
    float ceiling = WrAirPowerCeiling(g_energy.gravity, 0.015f);
    unsigned int col = EfficiencyColour(WrEfficiency(WrEnergyPower(), ceiling));

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

static void EmitEnergyOverlay(ImDrawList *dl)
{
    if (!g_energy.showOverlay || !WrEnergyValid())
        return;

    char lines[6][96];
    unsigned int cols[6];
    int n = 0;

    _snprintf_s(lines[n], sizeof(lines[0]), _TRUNCATE, "energy  %.0f  (%.0f u/s)",
                WrEnergyRelative(), WrEnergyEquivSpeed());
    cols[n++] = 0xFFFFFFFFu;

    _snprintf_s(lines[n], sizeof(lines[0]), _TRUNCATE, "speed   %.0f  (h %.0f)",
                WrEnergySpeed(), WrEnergyHorizontalSpeed());
    cols[n++] = 0xFFCCCCCCu;

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

    static const char *kWidest = "since start  -99999  (-99999)";
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

void WrRenderWorld(void)
{
    g_statSegments = 0;
    g_statPoints = 0;
    g_statBatches = 0;
    g_batchCount = 0;
    g_tagRectCount = 0;
    g_statTags = 0;

    VMatrix m;
    if (!WrWorldToScreen(&m))
        return;
    if (!WrCameraOrigin(&g_cam))
        return;

    // Feeds the Runs tab's "near you" column, which is how you tell which leg of
    // a staged map a run belongs to.
    WrUpdateNearest(g_cam);

    // Anchor to the reference run's first point, once, so that its clock and
    // ours start at the same place -- see wr_timer.h. Only ever taken when
    // nothing has anchored yet, so a manual anchor is never overwritten.
    if (g_energy.anchorToRunStart && WrEnergyAnchorSource() == WR_ANCHOR_NONE)
    {
        const WrRun *ref = WrEnergyReferenceRun();
        if (ref && ref->pointCount >= 2)
            WrEnergyAnchorToFeet(ref->points[0].pos);
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

    int drawn = 0;
    for (int i = 0; i < WrRunCount() && drawn < g_render.maxRunsDrawn; i++)
    {
        WrRun *run = WrRunAt(i);
        if (!run || !run->enabled || run->pointCount < 2)
            continue;
        EmitPath(dl, run->points, run->pointCount, run->breaks, run->breakCount,
                 run->colour, 1.0f, run->eff);
        EmitDips(dl, run);
        EmitMarkers(dl, run);
        if (g_render.drawTags)
        {
            // Queue the Steam lookup for anyone actually on screen, rather than
            // for every run in the store. Forty requests when the map loads
            // would be forty network lookups for lines you may never enable.
            WrSteamWant(run->steamId);
            EmitTag(dl, run);
        }
        drawn++;
    }

    EmitEnergyOverlay(dl);
    EmitVelocityVector(dl);
    EmitEnergyHud(dl);

    if (g_render.drawLive)
    {
        int n = 0;
        const WrPoint *live = WrLivePoints(&n);
        // No break list: WrLiveRecord restarts the buffer on any move over 512
        // units, so the live trail cannot contain a teleport by construction.
        if (n >= 2)
            EmitPath(dl, live, n, NULL, 0, g_render.liveColour, 0.0f, NULL);
    }

    QueryPerformanceCounter(&t1);
    g_statMillis = (float)((double)(t1.QuadPart - t0.QuadPart) * 1000.0 /
                           (double)Qpf());
    g_stageMs[WR_STAGE_EMIT] = g_stageMs[WR_STAGE_EMIT] * 0.95f + g_statMillis * 0.05f;
}
