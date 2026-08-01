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

#include "imgui.h"

#include <string.h>

#define TOGGLE_KEY VK_INSERT

static char g_lastMap[72] = {0};

// Called once per frame from the render thread, before the UI is drawn.
// Lives here rather than in wr_imgui.cpp so the per-frame ordering of the whole
// tool is visible in one place.
void WrFrameTick(void)
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
    }

    // Feed the live recorder and the energy sampler from the camera we already
    // solved for rendering. Both want it every frame, not only while the panel
    // is open.
    Vec3 cam;
    if (WrCameraOrigin(&cam))
    {
        WrLiveRecord(cam);
        WrEnergySample(cam, ImGui::GetIO().DeltaTime);
    }

    // Advance a couple of pending Steam avatar lookups.
    WrSteamTick();
}

static DWORD WINAPI HotkeyThread(LPVOID)
{
    bool down = false;
    for (;;)
    {
        bool now = (GetAsyncKeyState(TOGGLE_KEY) & 0x8000) != 0;
        if (now && !down)
            WrSetMenuOpen(!WrMenuOpen());
        down = now;
        Sleep(30);
    }
}

static DWORD WINAPI InitThread(LPVOID)
{
    WrLogInit();
    WrRenderDefaults();
    WrEnergyDefaults();

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
