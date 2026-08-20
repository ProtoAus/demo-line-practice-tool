// wrlines.dll  --  entry point and the bits of glue that tie the rest together.
//
// DllMain does nothing but spawn a thread. Everything real happens off the
// loader lock: waiting for d3d11.dll, reading the swapchain vtable, installing
// hooks. Doing any of that inside DllMain is a deadlock waiting to happen.
//
// WHAT THIS DELIBERATELY DOESN'T DO
//   There is no unload path. No MH_Uninitialize, no WndProc restore, no
//   FreeLibrary. Tearing down a Present hook while another thread is executing
//   inside the trampoline is the classic injected-DLL crash-on-exit, and since
//   the only reason to unload would be to reload a rebuilt DLL -- which a game
//   restart does more reliably anyway -- there is nothing to gain. The injector
//   refuses to inject twice for the same reason.
//
//   That matters more since extraction became a worker pool inside this
//   process, so it is worth being explicit: WrExtractShutdown is NOT called
//   from here. DLL_PROCESS_DETACH on process exit runs after ExitProcess has
//   already terminated every other thread, so waiting for a worker there would
//   be waiting, under the loader lock, for a thread that will never run again.
//   Nothing is lost by not waiting: every file a worker writes goes through a
//   temp name and a rename, so the worst a killed worker leaves behind is a
//   stray .tmp.

#include "wr_common.h"
#include "wr_log.h"
#include "wr_hook.h"
#include "wr_probe.h"
#include "wr_engine.h"
#include "wr_render.h"
#include "wr_path.h"
#include "wr_ui.h"
#include "wr_steam.h"
#include "wr_energy.h"
#include "wr_limit.h"
#include "wr_extract.h"
#include "wr_timer.h"
#include "wr_savelocs.h"
#include "wr_start.h"
#include "wr_intogame.h"
#include "wr_settings.h"
#include "wr_quick.h"
#include "wr_update.h"
#include "wr_bspload.h"

#include "imgui.h"

#include <string.h>

#define TOGGLE_KEY VK_INSERT

static char g_lastMap[72] = {0};

// Called once per frame from inside Present, BEFORE we decide whether to draw.
//
// Everything here has to keep working on frames where nothing is on screen --
// the map has to be noticed changing, the matrix scan has to keep tracking, the
// energy history has to stay continuous. None of it touches the device or ImGui,
// which is what lets the whole draw path be skipped on an idle frame.
//
// Lives here rather than in wr_imgui.cpp so the per-frame ordering of the whole
// tool is visible in one place.
void WrIdleTick(void)
{
    WrEngineTick();

    // Reload the run store whenever the map changes.
    const char *map = WrLevelName();
    if (map && strcmp(map, g_lastMap) != 0)
    {
        strcpy_s(g_lastMap, sizeof(g_lastMap), map);
        WrPathLoadMap(map);
        WrUiOnMapChanged(map);
        WrQuickOnMapChanged(map);
        WrLiveClear();
        WrEnergyReset();
        WrTimerReset();
        WrStartReset();
        WrRenderPickReset();
        // The prune alone. Adoption needs the numeric map id, which lives in the
        // map index the panel owns, so it happens there.
        WrIntoGameRefresh(NULL, 0);
        WrExtractOnMapChanged(map);

        // Drops the previous level's geometry NOW, and asks for this one's.
        // The drop has to happen on this thread and at this moment: a load
        // takes up to 124 ms, and until the replacement lands anything drawn
        // from the old map is ramps from somewhere else, at plausible angles,
        // in plausible places. See wr_bspload.h.
        WrBspLoadOnMapChanged(map);
    }

    // Feeds the run store a few files per frame, so a 125-run map does not stall
    // the render thread on load.
    WrPathLoadTick();

    // Picks up a finished map parse and frees the one it replaces. This is the
    // ONLY place a WrBspMap is freed, which is what lets the draw path hold the
    // pointer for a frame without a lock or a reference count.
    WrBspLoadTick();

    // Our own frame time. ImGui's io.DeltaTime is not available here: on an idle
    // frame there is no ImGui frame at all, and using it would make the energy
    // sampler's velocity baseline silently wrong whenever the panel was shut.
    static LARGE_INTEGER freq = {0};
    static LARGE_INTEGER prev = {0};
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = 0.0f;
    if (prev.QuadPart != 0 && freq.QuadPart != 0)
        dt = (float)((double)(now.QuadPart - prev.QuadPart) / (double)freq.QuadPart);
    prev = now;

    // Feed the live recorder and the energy sampler from the camera we already
    // solved for rendering. Both want it every frame, not only while the panel
    // is open.
    Vec3 cam;
    if (WrCameraOrigin(&cam))
    {
        // The live trail is recorded at the FEET, not the eye. Demo runs store
        // the player origin, so recording the camera put our own line about 64
        // units above a demo line of the identical trajectory -- close enough to
        // look deliberate and wrong enough to be useless for comparison.
        Vec3 feet = cam;
        feet.z -= g_energy.eyeHeight;

        // The energy sampler keeps the raw camera. Its anchor is a camera height
        // too, so the offset cancels there and must not be applied twice.
        //
        // It runs BEFORE the recorder now, because the recorder wants the
        // velocity this frame's sample produces. A frame of lag here would put
        // the wrong speed on the point it is attached to.
        WrEnergySample(cam, dt);
        WrEnergyTickArrow(dt);

        // Save-locs BEFORE the clock, and that ordering is a fix rather than a
        // preference. The other way round, loading an untimed save-loc restored
        // nothing and then stamped that save-loc with the PRE-load clock,
        // permanently -- which is how entries reading 0.000 got onto disk, after
        // which they restored as 0.000 for ever. It is handed the clock as it
        // stands now, which is what a save-loc created this frame deserves.
        WrSavelocRefresh(map, WrTimerElapsed(), WrTimerRunning());
        WrTimerTick(cam, dt);

        // Not while held: a paused demo would otherwise stamp the same point
        // over and over with a clock that is not advancing. WrLiveRecord already
        // ignores moves under two units, so this is belt and braces -- but it
        // also keeps the recorder from being handed a velocity that is being
        // deliberately frozen.
        if (!WrEnergyHeld())
        {
            // The sampler's OWN pair, not this frame's feet and the smoothed
            // velocity readout. Those describe two instants about 80 ms apart,
            // and every energy computed from a live point -- the Graphs tab's
            // live curve, any comparison against your own line -- was wrong by
            // whatever the trajectory did in between. Measured: with a
            // perfectly noiseless camera the resulting efficiency error was
            // still larger than the signal. See WrEnergySampleAt.
            Vec3 lvPos, lvVel;
            if (WrEnergySampleAt(&lvPos, &lvVel))
                WrLiveRecord(lvPos, lvVel, WrTimerElapsed());
        }
    }

    // Notice a changed setting and write it once it has settled. Here rather
    // than in the draw path, because settings are changed from the panel and the
    // panel being shut is exactly when a write is safe and wanted.
    WrSettingsTick(dt);

    // Advance a couple of pending Steam avatar lookups. Does nothing unless a
    // tag asked for one, which only happens while drawing.
    WrSteamTick();

    // The extractor writes .wrpath files, so once it has finished the store on
    // this side is out of date.
    //
    // Only for an extraction, and only for a fetch that brought new demos in.
    // The slot is shared by four jobs and this reloads every .wrpath for the map
    // -- 273 of them on surf_utopia -- which is a visible hitch to pay for
    // having looked at a leaderboard.
    if (WrExtractTakeFinished())
    {
        WrJobKind kind = WrExtractLastKind();
        const char *m = WrPathLoadedMap();
        if ((kind == WR_JOB_EXTRACT || kind == WR_JOB_FETCH) && m && *m)
        {
            char keep[72];
            strcpy_s(keep, sizeof(keep), m);
            WrPathLoadMap(keep);
            WrExtractOnMapChanged(keep);
        }
    }

    // The quick menu's chain, and it runs WHETHER OR NOT THAT PANEL IS OPEN --
    // which is the whole of "it downloads and extracts in the background". It is
    // also what re-applies the ticks after the reload just above, since a reload
    // turns every run off. Last, so it sees this frame's finished job.
    WrQuickTick();
}

// One edge latch. Every key here is a thing you want to change mid-run, which is
// exactly when a panel is the wrong answer -- and all of them are settable (and
// clearable) from the panel, since we cannot know what the player has bound.
// This READS the key, it does not swallow it, so a collision means the game acts
// on it too rather than anything being broken.
static bool Pressed(int vk, bool *wasDown)
{
    bool now = vk && (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = now && !*wasDown;
    *wasDown = now;
    return edge;
}

static DWORD WINAPI HotkeyThread(LPVOID)
{
    bool down = false;
    bool cycleDown = false, cycleBackDown = false;
    bool pickDown = false, overlayDown = false, quickDown = false;
    for (;;)
    {
        if (Pressed(TOGGLE_KEY, &down))
            WrSetMenuOpen(!WrMenuOpen());

        // The quick menu. Bindable, unlike Insert above -- Delete is already in
        // the panel's list of bindable keys and was already offered for the four
        // HUD toggles, so leaving this one a fixed #define would have let a
        // player bind Delete to the corner block and then wonder why it also
        // opened a window.
        if (Pressed(WrUiQuickKey(), &quickDown))
            WrSetPanelOpen(WR_PANEL_QUICK, !WrPanelOpen(WR_PANEL_QUICK));

        // Page Down and Page Up step the centre box's mode.
        if (Pressed(WrUiHudCycleKey(), &cycleDown))
            WrEnergyCycleHudMode(+1);
        if (Pressed(WrUiHudCycleBackKey(), &cycleBackDown))
            WrEnergyCycleHudMode(-1);

        // Home: the "whose line is this" plate. It sits over the thing it is
        // naming, so being able to get rid of it without opening the panel is
        // most of what makes it usable -- and it is off by default now, so this
        // is also how you get it in the first place.
        if (Pressed(WrUiPickToggleKey(), &pickDown))
            g_render.pickEnabled = !g_render.pickEnabled;

        // End: the corner block, off by default for the same reason.
        if (Pressed(WrUiOverlayToggleKey(), &overlayDown))
            g_energy.showOverlay = !g_energy.showOverlay;

        Sleep(30);
    }
}

static DWORD WINAPI InitThread(LPVOID)
{
    WrLogInit();
    WrRenderDefaults();
    WrEnergyDefaults();
    WrStartDefaults();
    WrLimitDefaults();
    WrQuickDefaults();
    WrBspLoadDefaults();

    // AFTER the defaults, never before: the file is read OVER them, so a key it
    // does not carry keeps whatever the default put there. That is what lets an
    // old settings file load into a new build without resetting the settings the
    // build has added since.
    WrSettingsInit();

    // Deletes the pair a previous install renamed aside. It happens here and
    // not there because the file that would have deleted them was the one still
    // mapped at that path -- see wr_update.h. This reads two directory entries
    // and reaches no network: the update CHECK is a button and only a button.
    WrUpdateSweepOld();

    // Said once, at startup, before anything can fail quietly because of it.
    // See WrPathIsAscii: this build cannot open a path with a byte >= 0x80 in
    // it, and until v0.7.0 the Python extractor could, so a library on such a
    // path used to half-work. Two lines in the log now beat a bug report that
    // says only "the button does nothing".
    if (!WrPathIsAscii(WrGameDir()) || !WrPathIsAscii(WrModuleDir()))
    {
        WrLogf("[!] a path here is not ASCII, and this build reads files with "
               "the -A Windows calls, which cannot name it:");
        WrLogf("[!]   game   %s", WrGameDir());
        WrLogf("[!]   module %s", WrModuleDir());
        WrLogf("[!] demos will not be found and extraction will refuse. Moving "
               "the install, or this folder, to an ASCII path is the fix.");
    }

    if (!WrProbeInit())
        WrLogf("[!] probe layer unavailable -- engine access will not be attempted");

    if (!WrHookInit())
    {
        WrLogf("[!] hook setup failed; WrLines is inert. Nothing was changed.");
        return 1;
    }

    CreateThread(NULL, 0, HotkeyThread, NULL, 0, NULL);
    WrLogf("ready -- press INSERT in game");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_wrSelf = module;
        DisableThreadLibraryCalls(module);
        CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
    }
    return TRUE;
}
