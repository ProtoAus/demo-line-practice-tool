// wr_render.h  --  project run paths and draw them over the world.

#ifndef WR_RENDER_H
#define WR_RENDER_H

#include "wr_common.h"

struct WrRenderSettings
{
    float thickness;
    float alpha;
    float maxDrawDistance;
    float fadeStartFraction;    // fraction of maxDrawDistance where fade begins
    float pixelTolerance;       // screen-space decimation
    int pointBudget;            // per run, per frame; 0 disables the cap
    int maxRunsDrawn;
    bool colourBySpeed;
    float speedMin, speedMax;
    bool drawMarkers;
    float markerRadius;
    bool drawLive;
    unsigned int liveColour;

    // Who owns which line. With forty runs enabled a colour is not an answer.
    bool drawTags;
    bool tagAvatars;
    float tagScale;
    int maxTags;

    // Horizontal speed at the bottom of each ramp.
    bool drawDipSpeeds;
    int maxDipsPerRun;
};

extern WrRenderSettings g_render;

void WrRenderDefaults(void);

// Called once per frame from inside the ImGui frame, before the panel is drawn.
// Emits into ImGui's background draw list so lines composite beneath the UI.
void WrRenderWorld(void);

// Stats for the Diagnostics tab. `batches` is the number of AddPolyline calls,
// which is the thing that actually costs -- a path whose distance fade keeps
// crossing bucket boundaries can flush hundreds of two-point polylines.
void WrRenderStats(int *segments, int *pointsConsidered, int *batches,
                   float *millis);

// The run the energy readout is comparing you against -- the fastest enabled
// one whose line is currently within g_energy.compareRadius -- or NULL when
// nothing enabled is near enough for the comparison to mean anything.
struct WrRun;
const WrRun *WrEnergyReferenceRun(void);

#endif // WR_RENDER_H
