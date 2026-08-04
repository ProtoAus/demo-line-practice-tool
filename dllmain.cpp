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
        WrLiveClear();
        WrEnergyReset();
        WrTimerReset();
        WrExtractOnMapChanged(map);
    }

    // Feeds the run store a few files per frame, so a 125-run map does not stall
    // the render thread on load.
    WrPathLoadTick();

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

        WrTimerTick(cam, dt);

        // Not while held: a paused demo would otherwise stamp the same point
        // over and over with a clock that is not advancing. WrLiveRecord already
        // ignores moves under two units, so this is belt and braces -- but it
        // also keeps the recorder from being handed a velocity that is being
        // deliberately frozen.
        if (!WrEnergyHeld())
        {
            Vec3 vel;
            if (!WrEnergyVelocity(&vel))
                vel = WrVec(0.0f, 0.0f, 0.0f);
            WrLiveRecord(feet, vel, WrTimerElapsed());
        }
        WrSavelocRefresh(map);
        WrSavelocTick(cam, WrTimerElapsed(), WrTimerRunning());
    }

    // Advance a couple of pending Steam avatar lookups. Does nothing unless a
    // tag asked for one, which only happens while drawing.
    WrSteamTick();

    // The extractor writes .wrpath files from another process, so once it has
    // finished the store on this side is out of date.
    if (WrExtractTakeFinished())
    {
        const char *m = WrPathLoadedMap();
        if (m && *m)
        {
            char keep[72];
            strcpy_s(keep, sizeof(keep), m);
            WrPathLoadMap(keep);
            WrExtractOnMapChanged(keep);
        }
    }
}

static DWORD WINAPI HotkeyThread(LPVOID)
{
    bool down = false;
    bool cycleDown = false;
    for (;;)
    {
        bool now = (GetAsyncKeyState(TOGGLE_KEY) & 0x8000) != 0;
        if (now && !down)
            WrSetMenuOpen(!WrMenuOpen());
        down = now;

        // Cycling the crosshair readout has to work WITHOUT opening the panel,
        // because the whole point of it is to be changed mid-run. The key is
        // settable (and clearable) from the Energy tab, since we cannot know
        // what the player has bound: this reads the key, it does not swallow it,
        // so a collision means the game acts on it too rather than anything
        // being broken.
        int cycleKey = WrUiHudCycleKey();
        bool cycNow = cycleKey && (GetAsyncKeyState(cycleKey) & 0x8000) != 0;
        if (cycNow && !cycleDown)
            WrEnergyCycleHudMode();
        cycleDown = cycNow;

        Sleep(30);
    }
}

static DWORD WINAPI InitThread(LPVOID)
{
    WrLogInit();
    WrRenderDefaults();
    WrEnergyDefaults();
    WrLimitDefaults();

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
