// wr_imgui.cpp  --  see wr_imgui.h.

#include "wr_imgui.h"
#include "wr_hook.h"
#include "wr_log.h"
#include "wr_ui.h"
#include "wr_render.h"

#include <d3d11.h>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

static ImGuiContext *g_ctx = NULL;
static bool g_backendsReady = false;

// Defined in dllmain.cpp. Called from Present before the draw decision, not from
// here -- it has to run on frames where nothing is drawn at all.
void WrIdleTick(void);

bool WrImGuiInit(HWND hwnd, ID3D11Device *device, ID3D11DeviceContext *context)
{
    if (g_ctx)
        return true;

    IMGUI_CHECKVERSION();
    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);

    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;      // never touch the game's momentum\cfg\imgui.ini
    io.LogFilename = NULL;

    // Draw our own cursor. The OS cursor is useless to us: Source hides it and
    // yanks it back to the screen centre every frame while mouselook is active,
    // so ImGui's normal GetCursorPos-based position would be pinned to the
    // middle of the screen and nothing would be draggable.
    io.MouseDrawCursor = true;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.WindowBorderSize = 1.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.94f;

    if (!ImGui_ImplWin32_Init(hwnd))
    {
        WrLogf("[!] ImGui_ImplWin32_Init failed");
        return false;
    }
    if (!ImGui_ImplDX11_Init(device, context))
    {
        WrLogf("[!] ImGui_ImplDX11_Init failed");
        return false;
    }
    g_backendsReady = true;

    WrLogf("ImGui %s initialised (our own context @ %p)", IMGUI_VERSION, (void *)g_ctx);
    return true;
}

void WrImGuiInvalidateDeviceObjects(void)
{
    if (g_backendsReady)
        ImGui_ImplDX11_InvalidateDeviceObjects();
}

void WrImGuiCreateDeviceObjects(void)
{
    if (g_backendsReady)
        ImGui_ImplDX11_CreateDeviceObjects();
}

void WrImGuiFrame(void)
{
    if (!g_ctx || !g_backendsReady)
        return;

    // The game has its own ImGui context; make sure every call below lands on
    // ours, and hand the previous one back afterwards.
    ImGuiContext *prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_ctx);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();

    ImGuiIO &io = ImGui::GetIO();
    int bw = 0, bh = 0;
    WrBackbufferSize(&bw, &bh);
    if (bw > 0 && bh > 0)
        io.DisplaySize = ImVec2((float)bw, (float)bh);

    // Mouse position has to go in through the EVENT QUEUE, after the Win32
    // backend has queued its own, and before ImGui::NewFrame() drains it.
    //
    // Assigning io.MousePos directly here does not work: NewFrame() replays the
    // queued events and the backend's position -- read from the OS cursor, which
    // Source drags back to the screen centre every frame -- lands last and wins.
    // That was the "cursor fights you every frame" bug. Queueing ours last means
    // ours is the one that survives.
    WrCursorUpdate();
    if (WrMenuOpen())
    {
        float cx = 0.0f, cy = 0.0f;
        WrVirtualCursor(&cx, &cy);
        io.AddMousePosEvent(cx, cy);
        // When the game is showing its own cursor we track it exactly, so
        // drawing a second one on top just looks like a doubled pointer.
        io.MouseDrawCursor = !WrCursorFollowsOS();
    }
    else
    {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        io.MouseDrawCursor = false;
    }

    ImGui::NewFrame();

    // World lines go into the background draw list so they composite beneath
    // the panel without any window or z-order juggling. Map detection, the
    // matrix scan and energy sampling already ran in WrIdleTick, before Present
    // decided whether this frame draws anything at all.
    WrRenderWorld();

    if (WrMenuOpen())
    {
        WrStageBegin(WR_STAGE_UI);
        WrUiDraw();
        WrStageEnd(WR_STAGE_UI);
    }

    WrStageBegin(WR_STAGE_SUBMIT);
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    WrStageEnd(WR_STAGE_SUBMIT);

    if (prev && prev != g_ctx)
        ImGui::SetCurrentContext(prev);
}
