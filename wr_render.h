// wr_render.h  --  project run paths and draw them over the world.

#ifndef WR_RENDER_H
#define WR_RENDER_H

#include "wr_common.h"

struct WrRun;

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

    // Numbers printed on the line itself. Both sites take any combination of
    // WR_LABEL_*, because which of them is useful depends entirely on what you
    // are practising.
    bool drawDipSpeeds;         // the toggle for ramp-bottom labels as a whole
    int maxDipsPerRun;
    unsigned int dipLabel;      // WR_LABEL_* mask
    bool drawPeaks;             // the same, at the tops
    int maxPeaksPerRun;
    unsigned int peakLabel;     // WR_LABEL_* mask
    unsigned int markerLabel;   // WR_LABEL_* mask
    int maxMarkersPerRun;
    int maxLabelsPerFrame;      // across every run; labels crowd faster than lines

    // Where you will be in a quarter of a second, drawn from your midsection.
    bool drawVelocity;

    // Colour lines by how much of the physically available energy the strafing
    // actually captured. See wr_stress.h -- this is NOT a turn-rate metric, and
    // the reason why is measured.
    bool colourByEfficiency;
    float effSaturation;        // |eta| that reaches full colour, both ways
    float effNeutralBand;       // |eta| under this keeps the run's own colour
    float effNeutralMix;        // how far neutral is pulled toward grey
    float effNoDataAlpha;       // multiplier where there is no reading at all
    bool effColourblind;        // blue/orange instead of red/green
    bool effLegend;             // draw the key on screen while the mode is on

    // Colour each whole run by where it placed on its own leg. Per RUN, unlike
    // the two above -- it replaces the palette colour a run was given, so the
    // line, its name tag, its ramp numbers and its checkpoints all agree.
    int rankColour;             // WrRankColour
    float rankFullBehind;       // WR_RANK_BY_TIME: +% off the best that reads red
    bool rankLegend;            // its rows go in the same key the efficiency uses
};

enum WrRankColour
{
    WR_RANK_OFF = 0,
    WR_RANK_BY_PLACING,     // even green->red across the field
    WR_RANK_BY_TIME,        // shaded by how far off the best each run is
    WR_RANK_MODE_COUNT
};

// The podium, in ImGui's ABGR. Deliberately not part of the ramp: a medal is a
// place, not a measurement, and blending them would make second look like a
// slightly worse first rather than second.
#define WR_COL_GOLD   0xFF00D7FFu
#define WR_COL_SILVER 0xFFC0C0C0u
#define WR_COL_BRONZE 0xFF327FCDu

// What a run should be drawn in, given the mode. Every site that used to read
// run->colour goes through this, or the line changes colour and its labels do
// not.
unsigned int WrRunColour(const WrRun *run);

// What a label on the line may say. Any combination; empty draws nothing.
#define WR_LABEL_SPEED  (1u << 0)
#define WR_LABEL_ENERGY (1u << 1)
#define WR_LABEL_TIME   (1u << 2)
#define WR_LABEL_DELTA  (1u << 3)   // yours minus theirs, once you have passed

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

// Would this frame put anything on screen? Asked inside Present, before any
// device state is touched; see HookedPresent in wr_hook.cpp.
bool WrHasAnythingToDraw(void);

// --- where the frame goes ----------------------------------------------------
//
// Four stage timers, smoothed, in milliseconds. Added because the last
// performance question was answered by reading code and reasoning rather than by
// measuring, which is one lucky guess away from being wrong.
enum WrStage
{
    WR_STAGE_IDLE = 0,      // map poll + matrix scan + energy sampling
    WR_STAGE_EMIT,          // projecting and batching the lines
    WR_STAGE_UI,            // building the panel
    WR_STAGE_SUBMIT,        // ImGui::Render + RenderDrawData
    WR_STAGE_COUNT
};

void WrStageBegin(WrStage s);
void WrStageEnd(WrStage s);
float WrStageMillis(WrStage s);
const char *WrStageName(WrStage s);

// The run the energy readout is comparing you against -- the fastest enabled
// one whose line is currently within g_energy.compareRadius -- or NULL when
// nothing enabled is near enough for the comparison to mean anything.
struct WrRun;
const WrRun *WrEnergyReferenceRun(void);

#endif // WR_RENDER_H
