// wr_render.h  --  project run paths and draw them over the world.

#ifndef WR_RENDER_H
#define WR_RENDER_H

#include "wr_common.h"

struct WrRun;

// Three caps that used to be sliders in the Display tab, and are constants now
// because none of them was ever a matter of taste.
//
// WR_MAX_RUNS_DRAWN -- how many enabled runs are drawn at all. Measured cost is
// in the WrRenderDefaults comment: 8 runs 0.24 ms/frame, 256 runs 8.1 ms, 1000
// runs 32.5 ms against a 16.7 ms frame at 60 Hz. Not redundant with the per-run
// checkboxes: the "All" button can enable up to WR_MAX_RUNS = 1000 in one press,
// and this is applied in the renderer afterwards. It must be read in BOTH the
// draw loop and the pick loop or the crosshair readout names a line that is not
// on screen.
//
// WR_MAX_LABELS_PER_FRAME -- numbers on the line, across every run. Not just a
// clutter limit: labels and name tags share the one WR_MAX_TAG_RECTS pool and
// numbers register first, so without a cap two or three runs' numbers can take
// every slot and silently starve every later run's NAME TAG. It also buys about
// half a millisecond a frame at 256 runs as an early-out.
//
// The per-run checkpoint cap is gone entirely rather than made a constant: its
// slider stopped at 64, WR_MAX_MARKERS is 64, and the loader clamps to it, so
// the maximum setting already was "no limit" and the setting could not express
// anything the storage did not already enforce.
#define WR_MAX_RUNS_DRAWN 256
#define WR_MAX_LABELS_PER_FRAME 40

struct WrRenderSettings
{
    float thickness;
    float alpha;
    float maxDrawDistance;
    float fadeStartFraction;    // fraction of maxDrawDistance where fade begins
    float pixelTolerance;       // screen-space decimation
    int pointBudget;            // per run, per frame; 0 disables the cap

    // What varies the colour ALONG a line. One of WrLineColour, because these
    // were three independent booleans with an unwritten precedence -- efficiency
    // beat speed beat flat -- and the only place that said so was a sentence in
    // the panel. Turning on a second one silently did nothing, which is a poor
    // way for a checkbox to behave.
    int lineColour;             // WrLineColour

    // Take the ends of the ramp from the runs actually on screen.
    //
    // The fixed pair below was 250..3500, and on a board's slow end that is
    // mostly wasted: a learner's run lives between 400 and 1200, which is a
    // fifth of the ramp, so every line comes out the same colour and the mode
    // says nothing. Scaled to what is enabled on the leg being looked at, the
    // ramp covers what those runs did.
    //
    // The user's own numbers are NOT overwritten. speedMin/speedMax and the two
    // energy pairs stay exactly as the sliders left them, the derived values go
    // in the use* fields below, and turning this off puts the sliders back by
    // doing nothing at all.
    bool autoScale;

    float speedMin, speedMax;
    float energyMin, energyMax;
    // WR_LINE_ENERGY_REL's own range. A separate pair, because the two modes
    // measure the same quantity on scales two orders of magnitude apart: an
    // absolute energy is a map coordinate in the thousands, and a relative one
    // sits either side of zero.
    float energyRelMin, energyRelMax;

    // WHAT THE COLOUR PATH ACTUALLY READS. Not settings: derived, not persisted,
    // and rewritten by WrRenderRefreshScales. Equal to the three pairs above
    // whenever autoScale is off.
    //
    // A separate set rather than writing the scaled numbers back over the
    // sliders, because a slider that moves on its own is a setting you can no
    // longer hold: drag speedMax to 2000, tick another run, and the number you
    // chose is gone with nothing to put it back.
    float useSpeedMin, useSpeedMax;
    float useEnergyMin, useEnergyMax;
    float useEnergyRelMin, useEnergyRelMax;

    // Skip the pre-roll: start each line where the run starts rather than where
    // the recording does. See startIndex in wr_path.h -- there is roughly three
    // quarters of a second of walking into the start zone in front of every
    // extracted demo, and it is what makes a replay appear to begin in an odd
    // place. Runs whose start could not be recovered draw in full either way.
    bool hidePreRoll;

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

    // Where you will be in a quarter of a second, drawn from your midsection.
    bool drawVelocity;

    // Settings for WR_LINE_EFFICIENCY: how much of the physically available
    // energy the strafing actually captured. See wr_stress.h -- this is NOT a
    // turn-rate metric, and the reason why is measured.
    float effSaturation;        // |eta| that reaches full colour, both ways
    float effNeutralBand;       // |eta| under this keeps the run's own colour
    float effNeutralMix;        // how far neutral is pulled toward grey
    float effNoDataAlpha;       // multiplier where there is no reading at all
    bool effColourblind;        // blue/orange instead of red/green
    bool lineKey;               // draw the key on screen for whichever mode is on

    // Aim at a line and be told whose it is.
    //
    // The crosshair is screen centre, not the mouse: wr_imgui.cpp clears ImGui's
    // mouse position whenever the panel is shut, which is exactly when you are
    // playing. So this runs off the world-to-screen matrix and nothing else.
    bool pickEnabled;
    float pickRadiusPx;         // how near the crosshair a line has to come
    float pickDepthBias;        // how much a far line is penalised, 0 = none
    float pickThickBoost;       // the picked line is drawn this much thicker
    float pickHoldSeconds;      // how long the readout survives looking away
    float pickOffsetPx;         // how far the plate sits off the picked point
    unsigned int pickLabel;     // WR_LABEL_* mask for the plate
    bool pickRing;

    // Colour each whole run by where it placed on its own leg. Per RUN, unlike
    // the two above -- it replaces the palette colour a run was given, so the
    // line, its name tag, its ramp numbers and its checkpoints all agree.
    int rankColour;             // WrRankColour
    float rankFullBehind;       // WR_RANK_BY_TIME: +% off the best that reads red
    bool rankLegend;            // its rows go in the same key the efficiency uses
};

// What varies along a line. Mutually exclusive by construction: exactly one
// quantity can be mapped onto a single line's colour, and pretending otherwise
// is how the old pair of booleans ended up with a precedence nobody could see.
//
// Rank colour is separate and composes with these: it sets what a run's BASE
// colour is, and WR_LINE_EFFICIENCY modulates from that base.
enum WrLineColour
{
    WR_LINE_FLAT = 0,       // one colour per run
    WR_LINE_SPEED,          // speedMin..speedMax
    WR_LINE_ENERGY,         // energyMin..energyMax, z + |v|^2/2g, absolute
    WR_LINE_ENERGY_REL,     // the same, less each run's own energy at its start
    WR_LINE_EFFICIENCY,     // dE/dt against what air accel could have added
    WR_LINE_MODE_COUNT
};

enum WrRankColour
{
    WR_RANK_OFF = 0,
    WR_RANK_BY_PLACING,     // even green->red across the field
    WR_RANK_BY_TIME,        // shaded by how far off the best each run is
    WR_RANK_MODE_COUNT
};

// The winner, in ImGui's ABGR. Deliberately not part of the ramp: first place is
// a place, not a measurement, and blending it in would make the best line look
// like a slightly greener version of the second-best.
//
// It was a full gold/silver/bronze podium, and on screen that failed for the
// reason a medal table does not have to work at: the ramp it sits in already
// runs green -> amber -> red, and gold, silver and bronze are all warm, mid
// brightness colours inside that range. Second and third simply vanished into
// the field, and gold was hard to pick out of the fast end.
//
// Violet is the one hue the ramp never reaches. The ramp holds blue at or below
// 0.25 everywhere along its length, so a colour that is mostly blue cannot be
// confused with any position in the field -- which is the entire job of this
// value. Only first place gets it; second and third are ordinary members of the
// ramp, where their colour at least tells you how close they were.
#define WR_COL_FIRST 0xFFFF5ABEu    // rgb(190, 90, 255)

// What a run should be drawn in, given the mode. Every site that used to read
// run->colour goes through this, or the line changes colour and its labels do
// not.
unsigned int WrRunColour(const WrRun *run);

// Recompute the use* ranges and every run's shownRank. Called once per frame
// from WrRenderWorld; returns immediately unless something it depends on has
// changed.
//
// A DIRTY STAMP, NOT A PER-FRAME PASS, and the reason is written into
// wr_path.h: the renderer asks for a run's colour once for the line, again for
// its name tag, its ramp numbers, its checkpoints and its comparison ring, and
// a scan of the store per call was a real performance bug at a thousand runs.
// The energy ranges need a pass over POINTS, which is heavier still. So this
// watches the store generation, the enabled set, the colour mode and gravity,
// and does nothing at all when none of them moved.
void WrRenderRefreshScales(void);

// Which colour range is live, for the on-screen key. Writes the pair the
// current lineColour mode is actually using.
void WrRenderColourRange(float *lo, float *hi, bool *scaled);

// What a label on the line may say. Any combination; empty draws nothing.
#define WR_LABEL_SPEED  (1u << 0)
#define WR_LABEL_ENERGY (1u << 1)
#define WR_LABEL_TIME   (1u << 2)
#define WR_LABEL_DELTA  (1u << 3)   // yours minus theirs, once you have passed

extern WrRenderSettings g_render;

void WrRenderDefaults(void);

// The run currently under the crosshair, or NULL. `pointIndex` is which point of
// it, `screenPx` how far off the crosshair it was, `tied` how many other runs
// were close enough that the choice between them was near-arbitrary -- shown so
// a coin toss is visible rather than trusted.
const WrRun *WrPickedRun(int *pointIndex, float *screenPx, int *tied);
void WrRenderPickReset(void);

// For Diagnostics: what the pick pass actually cost this frame.
void WrPickStats(int *chunksTested, int *pointsTested, float *millis);

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
