// test_settings.cpp  --  settings that survive a restart.
//
// The whole feature is a promise about what happens between two runs of the
// game, which is the one thing that cannot be checked by running the game once.
// So it is checked here instead, against the real wr_settings.cpp and the real
// settings structs -- a harness with its own private table would agree with
// itself and say nothing about what ships.
//
// WHAT IS ACTUALLY AT RISK
//
// Not the writing. The failures worth catching are the quiet ones:
//
//   1. A field registered but never written, or written and never read back.
//      One typo in a key name and that setting silently stops persisting, and
//      nothing anywhere says so.
//   2. A file from a different build. An unknown key must be ignored and a
//      missing key must keep its default, in BOTH directions -- otherwise
//      upgrading resets everything, which is the bug this was added to fix.
//   3. A hand-edited file. This is a text file people will edit, so it is an
//      untrusted input: nothing in it may put the tool somewhere its own
//      sliders cannot reach, and a NaN must not reach a float that geometry is
//      computed from.
//   4. A truncated file. Half a write must not lose the half that did land.
//
// Build:  tests\build.bat
// Run:    tests\test_settings.exe

#include "wr_settings.h"
#include "wr_render.h"
#include "wr_energy.h"
#include "wr_start.h"
#include "wr_limit.h"
#include "wr_profile.h"
#include "wr_quick.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// --- stubs -----------------------------------------------------------------
//
// g_energy, g_start, g_limit and the graph settings are the real ones -- their
// files link cleanly. g_render's does not: wr_render.cpp is the draw layer and
// pulls in ImGui, Steam and the engine, none of which belong in a harness.
//
// So the STRUCT is real and only the defaults are stubbed. That keeps what
// matters: every render field in the table is registered against the real
// WrRenderSettings, so a field renamed in the header or a pointer aimed at the
// wrong member still fails to compile here, and the round-trip below still
// exercises the real names and the real clamps.
WrRenderSettings g_render;

void WrRenderDefaults(void)
{
    memset(&g_render, 0, sizeof(g_render));
    g_render.thickness = 2.0f;
    g_render.alpha = 0.85f;
    g_render.maxDrawDistance = 12000.0f;
    g_render.pointBudget = 400000;
    g_render.markerRadius = 10.0f;
    g_render.tagScale = 1.0f;
    g_render.maxTags = 12;
    g_render.effSaturation = 0.6f;
    g_render.effNeutralMix = 0.7f;
    g_render.effNoDataAlpha = 0.35f;
    g_render.pickRadiusPx = 48.0f;
    g_render.pickThickBoost = 1.8f;
    g_render.pickHoldSeconds = 0.35f;
    g_render.rankFullBehind = 25.0f;
    g_render.liveColour = 0xFF66FF66u;
}

// The same arrangement for the quick menu, and for the same reason: wr_quick.cpp
// is a panel and pulls ImGui in behind it. The struct is real, so the three
// quick.* registrations in wr_settings.cpp are still checked against the real
// fields at compile time.
WrQuickSettings g_quick;

void WrQuickDefaults(void)
{
    memset(&g_quick, 0, sizeof(g_quick));
    g_quick.top = WR_QUICK_TOP_DEFAULT;
    g_quick.gamemode = 1;
}

// And the panel's own registration, which lives in wr_ui.cpp for the same
// reason -- a handful of its settings, restated with the same keys.
static int g_hudCycleKey = 0, g_pickToggleKey = 0;
static bool g_gLive = true;
static int g_gMaxSeries = 12;

void WrUiRegisterSettings(void)
{
    WrSettingsInt("key.hudCycleNext", &g_hudCycleKey, 0, 255);
    WrSettingsInt("key.pickToggle", &g_pickToggleKey, 0, 255);
    WrSettingsBool("graph.live", &g_gLive);
    WrSettingsInt("graph.maxSeries", &g_gMaxSeries, 1, 32);
}

// The engine layer, which none of this needs. wr_limit.cpp asks for the game
// window to find the monitor's refresh rate; there is no window here, and it
// handles that.
bool WrCameraForward(Vec3 *out) { if (out) *out = WrVec(1, 0, 0); return true; }
bool WrCameraOrigin(Vec3 *out) { (void)out; return false; }
HWND WrGameWindow(void) { return NULL; }

// --- harness ---------------------------------------------------------------

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-64s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

static void Defaults(void)
{
    WrRenderDefaults();
    WrEnergyDefaults();
    WrStartDefaults();
    WrLimitDefaults();
}

static void WriteFile_(const char *body)
{
    FILE *f = NULL;
    if (fopen_s(&f, WrSettingsPath(), "wb") == 0 && f)
    {
        fputs(body, f);
        fclose(f);
    }
}

static void RemoveFile_(void)
{
    remove(WrSettingsPath());
}

int main(void)
{
    printf("\n=== wrlines: settings that survive a restart ===\n");

    RemoveFile_();
    Defaults();
    WrSettingsInit();

    printf("\nevery registered field round-trips\n");
    {
        Check(WrSettingsFieldCount() > 60,
              "the table has the fields the panel actually shows");

        // Move everything off its default in a way that stays inside its range,
        // so nothing is clamped and a value that comes back changed is a real
        // failure rather than a clamp doing its job.
        g_render.thickness = 4.25f;
        g_render.alpha = 0.42f;
        g_render.lineColour = WR_LINE_EFFICIENCY;
        g_render.pickEnabled = true;
        g_render.liveColour = 0x12345678u;
        g_render.maxTags = 7;
        g_energy.hudMode = WR_HUD_STRAFE;
        g_energy.gaugeSeconds = 3.5f;
        g_energy.showOverlay = true;
        g_energy.hudScale = 1.75f;
        g_start.showZone = true;
        g_start.radiusScale = 1.5f;
        g_limit.enabled = true;
        g_limit.targetFps = 144.0f;
        g_wrProfileLiveSmooth = 0.35f;
        g_wrProfileDespike = false;
        g_hudCycleKey = 34;
        g_gMaxSeries = 5;
        g_gLive = false;

        Check(WrSettingsSave(), "it writes");

        // Wipe everything back to the defaults, which is what a restart does,
        // and read the file over them.
        Defaults();
        g_wrProfileLiveSmooth = 0.0f;
        g_wrProfileDespike = true;
        g_hudCycleKey = 0;
        g_gMaxSeries = 12;
        g_gLive = true;

        Check(WrSettingsLoad(), "and reads back");
        Check(WrSettingsUnknownKeys() == 0,
              "with no key it does not recognise -- so nothing is misspelt");

        Check(fabsf(g_render.thickness - 4.25f) < 1e-6f, "float came back");
        Check(fabsf(g_render.alpha - 0.42f) < 1e-6f, "and so did a second one");
        Check(g_render.lineColour == WR_LINE_EFFICIENCY, "an enum came back");
        Check(g_render.pickEnabled, "a bool came back");
        Check(g_render.liveColour == 0x12345678u,
              "and a colour survived as hex rather than being mangled");
        Check(g_render.maxTags == 7, "an int came back");
        Check(g_energy.hudMode == WR_HUD_STRAFE, "the crosshair mode came back");
        Check(fabsf(g_energy.gaugeSeconds - 3.5f) < 1e-6f,
              "the gauge window came back");
        Check(g_energy.showOverlay, "the corner block came back");
        Check(fabsf(g_energy.hudScale - 1.75f) < 1e-6f, "the text scale came back");
        Check(g_start.showZone, "the start ring came back");
        Check(fabsf(g_limit.targetFps - 144.0f) < 1e-6f, "the frame cap came back");
        Check(fabsf(g_wrProfileLiveSmooth - 0.35f) < 1e-6f,
              "the live smoothing came back");
        Check(!g_wrProfileDespike, "a bool that was turned OFF came back off");
        Check(g_hudCycleKey == 34, "a key binding came back");
        Check(g_gMaxSeries == 5 && !g_gLive,
              "and so did the settings the panel itself owns");
    }

    printf("\na file from another build loads, both ways round\n");
    {
        Defaults();
        WriteFile_("# a file from the future\n"
                   "line.thickness 6.5\n"
                   "wr.somethingWeHaveNeverHeardOf 12\n"
                   "hud.scale 2.5\n"
                   "another.unknown.key hello\n");
        Check(WrSettingsLoad(), "a file with unknown keys still loads");
        Check(WrSettingsUnknownKeys() == 2, "and says how many it skipped");
        Check(fabsf(g_render.thickness - 6.5f) < 1e-6f,
              "the keys it does know are applied");
        Check(fabsf(g_energy.hudScale - 2.5f) < 1e-6f,
              "including ones after the unknown line");

        // The other direction: a key that is simply absent must keep whatever
        // the defaults put there, which is what lets a new build add a setting
        // without resetting everybody's file.
        Defaults();
        float defAlpha = g_render.alpha;
        WriteFile_("line.thickness 3.0\n");
        WrSettingsLoad();
        Check(fabsf(g_render.alpha - defAlpha) < 1e-6f,
              "a missing key keeps its default rather than becoming zero");
    }

    printf("\na hand-edited file cannot reach an impossible state\n");
    {
        Defaults();
        WriteFile_("line.thickness 9999\n"
                   "line.alpha -5\n"
                   "hud.mode 99\n"
                   "graph.maxSeries 100000\n"
                   "line.thickness2 4\n");
        WrSettingsLoad();
        Check(g_render.thickness <= 12.0f && g_render.thickness >= 0.5f,
              "a huge float is clamped to the slider's range");
        Check(g_render.alpha >= 0.05f, "and a negative one to its floor");
        Check(g_energy.hudMode >= 0 && g_energy.hudMode < WR_HUD_MODE_COUNT,
              "an out-of-range mode cannot index past the mode table");
        Check(g_gMaxSeries <= 32, "and an int is clamped too");

        // A NaN fails every comparison, so a range test written the obvious way
        // round lets it through -- and a NaN thickness propagates into the
        // geometry and draws nothing at all, silently.
        Defaults();
        WriteFile_("line.thickness nan\nhud.scale nan\n");
        WrSettingsLoad();
        Check(g_render.thickness == g_render.thickness,
              "a NaN in the file does not reach the line thickness");
        Check(g_energy.hudScale == g_energy.hudScale,
              "nor the text scale");
    }

    printf("\na half-written file keeps the half that landed\n");
    {
        Defaults();
        WriteFile_("line.thickness 5.5\n"
                   "line.alpha 0.33\n"
                   "hud.scal");            // cut off mid-key, no newline
        WrSettingsLoad();
        Check(fabsf(g_render.thickness - 5.5f) < 1e-6f,
              "the complete lines before the truncation are applied");
        Check(fabsf(g_render.alpha - 0.33f) < 1e-6f, "all of them");
    }

    printf("\nreset reaches every field, not just the ones with a Defaults()\n");
    {
        // The snapshot is taken at WrSettingsInit, which ran at the top of this
        // file -- so the defaults it restores are the ones from then, whatever
        // has happened since.
        float defThick = 2.0f;          // what the stubbed WrRenderDefaults sets
        int defKey = 0, defSeries = 12; // and what the panel-owned ones start at

        g_render.thickness = 7.0f;
        g_hudCycleKey = 46;
        g_gMaxSeries = 3;
        g_gLive = false;

        WrSettingsResetAll();
        Check(fabsf(g_render.thickness - defThick) < 1e-6f,
              "a struct field goes back to its default");
        Check(g_hudCycleKey == defKey && g_gMaxSeries == defSeries && g_gLive,
              "and so do the settings the PANEL owns, which have no Defaults()");

        // And what it wrote agrees. Wiping first, so a value that survives is
        // one the file actually carried.
        g_render.thickness = 7.0f;
        g_hudCycleKey = 46;
        g_gMaxSeries = 3;
        WrSettingsLoad();
        Check(fabsf(g_render.thickness - defThick) < 1e-6f &&
              g_hudCycleKey == defKey && g_gMaxSeries == defSeries,
              "the file it wrote says so too");
    }

    printf("\nthe writer waits for a change to settle\n");
    {
        Defaults();
        WrSettingsSave();
        WrSettingsTick(0.016f);
        Check(!WrSettingsPending(), "nothing pending when nothing has changed");

        g_render.thickness = 3.75f;
        WrSettingsTick(0.016f);
        Check(WrSettingsPending(), "a change is noticed");

        // Still moving -- a slider being dragged. Each new value restarts the
        // clock, so nothing is written yet.
        for (int i = 0; i < 200; i++)
        {
            g_render.thickness = 3.75f + (float)i * 0.01f;
            WrSettingsTick(0.016f);
        }
        Check(WrSettingsPending(),
              "and while it keeps moving, it is still not written");

        // Let go.
        float held = g_render.thickness;
        for (int i = 0; i < 200; i++)
            WrSettingsTick(0.016f);
        Check(!WrSettingsPending(), "once it settles, it is written");

        Defaults();
        WrSettingsLoad();
        Check(fabsf(g_render.thickness - held) < 1e-5f,
              "and what was written is the value it settled on");
    }

    RemoveFile_();
    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
