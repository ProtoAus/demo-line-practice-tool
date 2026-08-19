// wr_settings.cpp  --  see wr_settings.h.

#include "wr_settings.h"
#include "wr_render.h"
#include "wr_energy.h"
#include "wr_start.h"
#include "wr_limit.h"
#include "wr_profile.h"
#include "wr_quick.h"
#include "wr_board.h"           // WR_GAMEMODE_COUNT, for the quick menu's range
#include "wr_extract.h"         // the per-demo timeout, which had a slider and
                                // no line in this table until v0.9.4
#include "wr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WR_SET_MAX 256

enum WrSetType { WR_SET_BOOL = 0, WR_SET_INT, WR_SET_UINT, WR_SET_FLOAT };

struct WrSetField
{
    const char *name;
    int type;
    void *p;
    float lo, hi;       // ints use these too; a float holds every int we store
};

static WrSetField g_f[WR_SET_MAX];
static int g_n = 0;
static bool g_registered = false;

// What every field held the instant after registration, which is the instant
// after the *Defaults() functions ran and before the file was read. That is the
// definition of "default" for every field uniformly.
//
// Snapshotted rather than reset by calling the *Defaults() functions again,
// because those only cover the four settings structs: the key bindings and the
// Graphs tab's own toggles are owned by the panel and have no Defaults() to
// call, so "reset everything" left them exactly as they were while claiming
// otherwise. A snapshot cannot drift from the table, because it IS the table.
static unsigned int g_default[WR_SET_MAX];

// Debounce state. `g_dirtyFor` counts up only while the values differ from what
// was last written, and is reset to zero every time they change again -- so a
// slider being dragged writes once, after it is let go of.
#define SETTLE_SECONDS 2.0f
static unsigned int g_savedHash = 0;
static float g_dirtyFor = -1.0f;    // negative: nothing pending
static float g_sinceSave = -1.0f;
static int g_unknownKeys = 0;

// ---------------------------------------------------------------------------

static void Add(const char *name, int type, void *p, float lo, float hi)
{
    if (g_n >= WR_SET_MAX || !name || !p)
    {
        // Silent truncation here would look exactly like a setting that does not
        // persist, which is the one failure this file exists to prevent.
        WrLogf("[!] settings: no room for \"%s\" -- raise WR_SET_MAX",
               name ? name : "?");
        return;
    }
    g_f[g_n].name = name;
    g_f[g_n].type = type;
    g_f[g_n].p = p;
    g_f[g_n].lo = lo;
    g_f[g_n].hi = hi;
    g_n++;
}

void WrSettingsBool(const char *name, bool *p)
{
    Add(name, WR_SET_BOOL, p, 0.0f, 1.0f);
}
void WrSettingsInt(const char *name, int *p, int lo, int hi)
{
    Add(name, WR_SET_INT, p, (float)lo, (float)hi);
}
void WrSettingsUInt(const char *name, unsigned int *p)
{
    Add(name, WR_SET_UINT, p, 0.0f, 0.0f);
}
void WrSettingsFloat(const char *name, float *p, float lo, float hi)
{
    Add(name, WR_SET_FLOAT, p, lo, hi);
}

// ---------------------------------------------------------------------------
// The registration
// ---------------------------------------------------------------------------
//
// Ranges match the sliders that set these, because that is what makes a
// hand-edited file safe: the widest thing the file can say is the widest thing
// the panel could have said.

static void RegisterRender(void)
{
    WrSettingsFloat("line.thickness", &g_render.thickness, 0.5f, 12.0f);
    WrSettingsFloat("line.alpha", &g_render.alpha, 0.05f, 1.0f);
    WrSettingsFloat("line.maxDrawDistance", &g_render.maxDrawDistance,
                    0.0f, 100000.0f);
    WrSettingsFloat("line.fadeStartFraction", &g_render.fadeStartFraction,
                    0.0f, 1.0f);
    WrSettingsFloat("line.pixelTolerance", &g_render.pixelTolerance, 0.0f, 8.0f);
    WrSettingsInt("line.pointBudget", &g_render.pointBudget, 1000, 4000000);
    WrSettingsInt("line.colourMode", &g_render.lineColour, 0,
                  WR_LINE_MODE_COUNT - 1);
    // Missing until v0.8.3, which meant ticking "scale to what is on" bought you
    // one session of it and no warning that it had gone. The derived use* fields
    // beside it are NOT here and must not be: they are recomputed from this and
    // the enabled set, and a persisted copy of a derived value is a stale value
    // waiting to happen.
    WrSettingsBool("line.autoScale", &g_render.autoScale);
    WrSettingsBool("line.hidePreRoll", &g_render.hidePreRoll);

    WrSettingsBool("markers.draw", &g_render.drawMarkers);
    WrSettingsFloat("markers.radius", &g_render.markerRadius, 1.0f, 64.0f);
    WrSettingsUInt("markers.label", &g_render.markerLabel);

    WrSettingsBool("live.draw", &g_render.drawLive);
    WrSettingsUInt("live.colour", &g_render.liveColour);

    WrSettingsBool("tags.draw", &g_render.drawTags);
    WrSettingsBool("tags.avatars", &g_render.tagAvatars);
    WrSettingsFloat("tags.scale", &g_render.tagScale, 0.5f, 3.0f);
    WrSettingsInt("tags.max", &g_render.maxTags, 0, 64);

    WrSettingsBool("dips.draw", &g_render.drawDipSpeeds);
    WrSettingsInt("dips.perRun", &g_render.maxDipsPerRun, 0, 64);
    WrSettingsUInt("dips.label", &g_render.dipLabel);
    WrSettingsBool("peaks.draw", &g_render.drawPeaks);
    WrSettingsInt("peaks.perRun", &g_render.maxPeaksPerRun, 0, 64);
    WrSettingsUInt("peaks.label", &g_render.peakLabel);

    WrSettingsBool("boards.draw", &g_render.drawBoards);
    WrSettingsInt("boards.perRun", &g_render.maxBoardsPerRun, 0, 64);
    WrSettingsBool("boards.labels", &g_render.boardLabels);
    WrSettingsBool("boards.detail", &g_render.boardLabelDetail);

    WrSettingsBool("velocity.draw", &g_render.drawVelocity);

    WrSettingsFloat("eff.saturation", &g_render.effSaturation, 0.2f, 1.0f);
    WrSettingsFloat("eff.neutralBand", &g_render.effNeutralBand, 0.0f, 0.5f);
    WrSettingsFloat("eff.neutralMix", &g_render.effNeutralMix, 0.0f, 1.0f);
    WrSettingsFloat("eff.noDataAlpha", &g_render.effNoDataAlpha, 0.0f, 1.0f);
    WrSettingsBool("eff.colourblind", &g_render.effColourblind);
    WrSettingsBool("eff.key", &g_render.lineKey);

    WrSettingsBool("pick.enabled", &g_render.pickEnabled);
    WrSettingsFloat("pick.radiusPx", &g_render.pickRadiusPx, 8.0f, 160.0f);
    WrSettingsFloat("pick.depthBias", &g_render.pickDepthBias, 0.0f, 1.0f);
    WrSettingsFloat("pick.thickBoost", &g_render.pickThickBoost, 1.0f, 4.0f);
    WrSettingsFloat("pick.holdSeconds", &g_render.pickHoldSeconds, 0.0f, 5.0f);
    WrSettingsFloat("pick.offsetPx", &g_render.pickOffsetPx, -200.0f, 200.0f);
    WrSettingsUInt("pick.label", &g_render.pickLabel);
    WrSettingsBool("pick.ring", &g_render.pickRing);

    WrSettingsInt("rank.colour", &g_render.rankColour, 0, 8);
    WrSettingsFloat("rank.fullBehind", &g_render.rankFullBehind, 0.0f, 100.0f);
    WrSettingsBool("rank.legend", &g_render.rankLegend);
}

static void RegisterEnergy(void)
{
    WrSettingsFloat("phys.gravity", &g_energy.gravity, 200.0f, 1600.0f);
    WrSettingsFloat("phys.airAccelerate", &g_energy.airAccelerate, 5.0f, 1000.0f);
    WrSettingsFloat("phys.maxSpeed", &g_energy.maxSpeed, 100.0f, 500.0f);
    WrSettingsFloat("phys.eyeHeight", &g_energy.eyeHeight, 0.0f, 96.0f);

    WrSettingsBool("hud.show", &g_energy.showHud);
    WrSettingsFloat("hud.offsetX", &g_energy.hudOffsetX, -2000.0f, 2000.0f);
    WrSettingsFloat("hud.offsetY", &g_energy.hudOffsetY, -2000.0f, 2000.0f);
    WrSettingsInt("hud.alignX", &g_energy.hudAlignX, 0, 2);
    WrSettingsInt("hud.anchorY", &g_energy.hudAnchorY, 0, 2);
    WrSettingsFloat("hud.scale", &g_energy.hudScale, 0.6f, 3.0f);
    WrSettingsBool("hud.backing", &g_energy.hudBacking);
    WrSettingsBool("hud.clock", &g_energy.showHudClock);
    WrSettingsBool("hud.speedLine", &g_energy.showHudSpeed);
    WrSettingsFloat("hud.width", &g_energy.hudWidth, 0.0f, 400.0f);
    WrSettingsInt("hud.mode", &g_energy.hudMode, 0, WR_HUD_MODE_COUNT - 1);

    WrSettingsBool("overlay.show", &g_energy.showOverlay);
    WrSettingsInt("overlay.corner", &g_energy.overlayCorner, 0, 3);
    WrSettingsFloat("overlay.margin", &g_energy.overlayMargin, 0.0f, 200.0f);
    WrSettingsFloat("overlay.scale", &g_energy.overlayScale, 0.5f, 3.0f);

    WrSettingsBool("compare.toRun", &g_energy.compareToRun);
    WrSettingsFloat("compare.radius", &g_energy.compareRadius, 64.0f, 4096.0f);
    WrSettingsBool("compare.showPoint", &g_energy.showComparePoint);
    WrSettingsBool("compare.leader", &g_energy.comparePointLeader);
    WrSettingsBool("compare.anchorToRunStart", &g_energy.anchorToRunStart);

    WrSettingsFloat("filter.smoothSeconds", &g_energy.smoothSeconds, 0.0f, 2.0f);
    WrSettingsFloat("filter.trendSeconds", &g_energy.trendSeconds, 0.05f, 3.0f);
    WrSettingsFloat("filter.quantiseStep", &g_energy.quantiseStep, 0.0f, 100.0f);
    WrSettingsFloat("filter.velWindow", &g_energy.velWindowSeconds, 0.005f, 0.5f);
    WrSettingsFloat("filter.velTau", &g_energy.velTau, 0.0f, 1.0f);
    WrSettingsFloat("filter.speedTau", &g_energy.speedTau, 0.0f, 1.0f);
    WrSettingsFloat("filter.powerSeconds", &g_energy.powerSeconds, 0.05f, 1.5f);
    WrSettingsFloat("filter.gaugeSeconds", &g_energy.gaugeSeconds, 0.5f, 5.0f);
    WrSettingsFloat("filter.arrowBand", &g_energy.arrowBand, 0.0f, 200.0f);

    WrSettingsBool("bar.show", &g_energy.showBar);
    WrSettingsInt("bar.mode", &g_energy.barMode, 0, WR_BAR_MODE_COUNT - 1);
    WrSettingsFloat("bar.maxEnergy", &g_energy.barMaxEnergy, 10.0f, 5000.0f);
    WrSettingsFloat("bar.maxSpeed", &g_energy.barMaxSpeed, 10.0f, 3000.0f);
    WrSettingsFloat("bar.height", &g_energy.barHeight, 1.0f, 40.0f);
}

static void RegisterStart(void)
{
    WrSettingsBool("start.enabled", &g_start.enabled);
    WrSettingsBool("start.autoAnchor", &g_start.autoAnchor);
    WrSettingsBool("start.autoZeroClock", &g_start.autoZeroClock);
    WrSettingsBool("start.showZone", &g_start.showZone);
    WrSettingsFloat("start.radiusScale", &g_start.radiusScale, 0.25f, 4.0f);
    WrSettingsFloat("start.leaveSpeed", &g_start.leaveSpeed, 0.0f, 1000.0f);
    WrSettingsFloat("start.stillSpeed", &g_start.stillSpeed, 0.0f, 1000.0f);
}

static void RegisterLimit(void)
{
    WrSettingsBool("cap.enabled", &g_limit.enabled);
    WrSettingsBool("cap.autoTarget", &g_limit.autoTarget);
    WrSettingsFloat("cap.targetFps", &g_limit.targetFps, 10.0f, 1000.0f);
    WrSettingsFloat("cap.headroomHz", &g_limit.headroomHz, 0.0f, 60.0f);
    WrSettingsFloat("cap.spinMs", &g_limit.spinMs, 0.0f, 10.0f);
}

static void RegisterQuick(void)
{
    WrSettingsBool("quick.network", &g_quick.network);
    WrSettingsInt("quick.top", &g_quick.top, 1, WR_QUICK_TOP_MAX);
    WrSettingsInt("quick.gamemode", &g_quick.gamemode, 1, WR_GAMEMODE_COUNT);

    // Not the quick menu's, strictly -- the slider is in the full panel's Runs
    // tab -- but it is here because it is the quick menu that made the omission
    // matter. It had a slider and no line in this table, so anybody who raised
    // it got it back at 30 on the next launch, which is exactly the shape of
    // the line.autoScale bug and was found the same way: by looking.
    //
    // 0 is "no limit" and has to stay reachable, so the range starts there
    // rather than at 1. The upper end matches the slider.
    WrSettingsInt("extract.timeout", WrExtractTimeoutPtr(), 0, 300);
}

static void RegisterProfile(void)
{
    WrSettingsInt("graph.buckets", &g_wrProfileBuckets, 16, WR_PROFILE_BUCKETS);
    WrSettingsBool("graph.despike", &g_wrProfileDespike);
    WrSettingsFloat("graph.liveSmooth", &g_wrProfileLiveSmooth, 0.0f, 0.5f);
}

// ---------------------------------------------------------------------------
// Snapshot and hash
// ---------------------------------------------------------------------------
//
// Every value is four bytes, so the whole table is one FNV-1a pass over it. This
// is a CHANGE detector and never a checksum of anything on disk -- collisions
// only ever cost one write that was not needed, or delay one that was.

// Every registered value is four bytes, so both of these are one memcpy each.
static unsigned int RawOf(const WrSetField *fd)
{
    unsigned int v = 0;
    if (fd->type == WR_SET_BOOL)
        v = *(bool *)fd->p ? 1u : 0u;
    else
        memcpy(&v, fd->p, 4);
    return v;
}

static void RawTo(const WrSetField *fd, unsigned int v)
{
    if (fd->type == WR_SET_BOOL)
        *(bool *)fd->p = (v != 0);
    else
        memcpy(fd->p, &v, 4);
}

static unsigned int HashNow(void)
{
    unsigned int h = 2166136261u;
    for (int i = 0; i < g_n; i++)
    {
        unsigned int v = RawOf(&g_f[i]);
        for (int b = 0; b < 4; b++)
        {
            h ^= (v >> (b * 8)) & 0xFFu;
            h *= 16777619u;
        }
    }
    return h;
}

// ---------------------------------------------------------------------------

const char *WrSettingsPath(void)
{
    return WrDataPath("settings.cfg");
}

int WrSettingsFieldCount(void) { return g_n; }
float WrSettingsSinceSave(void) { return g_sinceSave; }
bool WrSettingsPending(void) { return g_dirtyFor >= 0.0f; }
int WrSettingsUnknownKeys(void) { return g_unknownKeys; }

bool WrSettingsSave(void)
{
    char path[MAX_PATH];
    strcpy_s(path, sizeof(path), WrSettingsPath());

    char tmp[MAX_PATH];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s.tmp", path);

    FILE *f = NULL;
    if (fopen_s(&f, tmp, "wb") != 0 || !f)
    {
        WrLogf("[!] settings: could not write %s", tmp);
        return false;
    }

    fprintf(f,
        "# WrLines settings. Written automatically; safe to edit or delete.\n"
        "#\n"
        "# Deleting this file puts everything back to its defaults. A key this\n"
        "# build does not know is ignored, and a key that is missing keeps its\n"
        "# default -- so an old file and a new build always agree, in both\n"
        "# directions, and there is no version to get wrong.\n"
        "#\n"
        "# Every value is clamped to the range its own slider has when it is\n"
        "# read, so nothing in here can put the tool somewhere the panel could\n"
        "# not have put it.\n"
        "#\n"
        "# DISPLAY SETTINGS ONLY. No names, no SteamIDs, no map or run data, no\n"
        "# paths, no record of what was watched. Safe to paste into a bug\n"
        "# report.\n"
        "\n");

    for (int i = 0; i < g_n; i++)
    {
        switch (g_f[i].type)
        {
        case WR_SET_BOOL:
            fprintf(f, "%s %d\n", g_f[i].name, *(bool *)g_f[i].p ? 1 : 0);
            break;
        case WR_SET_INT:
            fprintf(f, "%s %d\n", g_f[i].name, *(int *)g_f[i].p);
            break;
        case WR_SET_UINT:
            // Hex, because every one of these is a colour or a bit mask and
            // decimal 4283782502 is not something anybody can edit.
            fprintf(f, "%s 0x%08X\n", g_f[i].name, *(unsigned int *)g_f[i].p);
            break;
        default:
            // %.9g round-trips a float exactly, which matters only so that a
            // save immediately after a load is not seen as a change.
            fprintf(f, "%s %.9g\n", g_f[i].name, *(float *)g_f[i].p);
            break;
        }
    }

    fclose(f);

    // Replace atomically, so an interrupted write cannot leave a half file where
    // the settings used to be.
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
    {
        WrLogf("[!] settings: could not replace %s", path);
        DeleteFileA(tmp);
        return false;
    }

    g_savedHash = HashNow();
    g_dirtyFor = -1.0f;
    g_sinceSave = 0.0f;
    return true;
}

static void Apply(WrSetField *fd, const char *value)
{
    switch (fd->type)
    {
    case WR_SET_BOOL:
        *(bool *)fd->p = (atoi(value) != 0);
        break;
    case WR_SET_INT:
    {
        int v = atoi(value);
        if (v < (int)fd->lo) v = (int)fd->lo;
        if (v > (int)fd->hi) v = (int)fd->hi;
        *(int *)fd->p = v;
        break;
    }
    case WR_SET_UINT:
        *(unsigned int *)fd->p = (unsigned int)strtoul(value, NULL, 0);
        break;
    default:
    {
        float v = (float)atof(value);
        // NaN fails every comparison, so test for the range it IS in rather
        // than the one it is not: a NaN written into a slider's target
        // propagates into the geometry and draws nothing, silently.
        if (!(v >= fd->lo && v <= fd->hi))
            v = (v > fd->hi) ? fd->hi : fd->lo;
        *(float *)fd->p = v;
        break;
    }
    }
}

bool WrSettingsLoad(void)
{
    g_unknownKeys = 0;

    FILE *f = NULL;
    if (fopen_s(&f, WrSettingsPath(), "rb") != 0 || !f)
        return false;           // no file yet is not an error

    char line[256];
    int applied = 0;
    while (fgets(line, sizeof(line), f))
    {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\r' || *p == '\n' || !*p)
            continue;

        char *name = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (!*p)
            continue;           // a key with no value; ignore rather than guess
        *p++ = '\0';
        while (*p == ' ' || *p == '\t') p++;

        char *end = p;
        while (*end && *end != '\r' && *end != '\n') end++;
        *end = '\0';
        if (!*p)
            continue;

        bool found = false;
        for (int i = 0; i < g_n; i++)
        {
            if (strcmp(g_f[i].name, name) != 0)
                continue;
            Apply(&g_f[i], p);
            applied++;
            found = true;
            break;
        }
        if (!found)
            g_unknownKeys++;
    }
    fclose(f);

    g_savedHash = HashNow();
    g_dirtyFor = -1.0f;
    WrLogf("settings: %d of %d read from settings.cfg%s", applied, g_n,
           g_unknownKeys ? " (some keys were not recognised)" : "");
    return true;
}

void WrSettingsInit(void)
{
    if (!g_registered)
    {
        RegisterRender();
        RegisterEnergy();
        RegisterStart();
        RegisterLimit();
        RegisterProfile();
        RegisterQuick();
        WrUiRegisterSettings();
        g_registered = true;

        // Right here, before the file is read: this is what "default" means.
        for (int i = 0; i < g_n; i++)
            g_default[i] = RawOf(&g_f[i]);
    }

    // Defaults are already in place when this runs -- dllmain calls the four
    // *Defaults() functions first -- so a key the file does not carry simply
    // keeps what it had.
    if (!WrSettingsLoad())
    {
        g_savedHash = HashNow();
        g_dirtyFor = -1.0f;
        WrLogf("settings: no settings.cfg yet; %d fields at their defaults", g_n);
    }
}

void WrSettingsResetAll(void)
{
    // From the snapshot, so it reaches EVERY registered field and not only the
    // ones that happen to live in a struct with a Defaults() function.
    for (int i = 0; i < g_n; i++)
        RawTo(&g_f[i], g_default[i]);
    WrSettingsSave();
}

void WrSettingsTick(float dt)
{
    if (!g_registered || !(dt >= 0.0f) || dt > 1.0f)
        return;
    if (g_sinceSave >= 0.0f)
        g_sinceSave += dt;

    unsigned int now = HashNow();
    if (now == g_savedHash)
    {
        g_dirtyFor = -1.0f;
        return;
    }

    // Changed. Start the clock, and restart it on every further change, so a
    // slider being dragged writes once when it is let go of rather than on
    // every frame of the drag.
    static unsigned int lastSeen = 0;
    if (g_dirtyFor < 0.0f || now != lastSeen)
    {
        lastSeen = now;
        g_dirtyFor = 0.0f;
        return;
    }

    g_dirtyFor += dt;
    if (g_dirtyFor >= SETTLE_SECONDS)
        WrSettingsSave();
}
