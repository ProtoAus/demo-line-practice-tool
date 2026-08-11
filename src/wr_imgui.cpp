// wr_imgui.cpp  --  see wr_imgui.h.

#include "wr_imgui.h"
#include "wr_hook.h"
#include "wr_log.h"
#include "wr_ui.h"
#include "wr_quick.h"
#include "wr_render.h"

#include <d3d11.h>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

static ImGuiContext *g_ctx = NULL;
static bool g_backendsReady = false;

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------
//
// Nothing loaded a font before, so ImGui fell back to AddFontDefault(): the
// embedded ProggyClean bitmap face, baked once at 13 pixels with no
// oversampling. The crosshair readout then asked for 13 * 1.7 * hudScale, i.e.
// up to 66 pixels, and ImDrawList::AddText happily scaled the 13-pixel glyphs
// through a bilinear sampler. That is the reported blur, and no amount of
// filtering fixes it -- there simply are no pixels there to magnify.
//
// So the sizes are BAKED. Text is only ever drawn at a size that exists in the
// atlas, never scaled to it.
//
// Consolas rather than the built-in face for a second reason that matters just
// as much: it is monospaced. The readout is numbers that change every frame, and
// with a proportional face the measured width of the block changes with them --
// which is why the corner block visibly slid around. Fixed-width digits make the
// layout constant by construction.

static const float kFontSizes[] = {
    13.0f, 16.0f, 20.0f, 24.0f, 30.0f, 38.0f, 48.0f, 60.0f
};
#define WR_FONT_COUNT (int)(sizeof(kFontSizes) / sizeof(kFontSizes[0]))

static ImFont *g_fonts[WR_FONT_COUNT];
static int g_fontCount = 0;
static bool g_monospace = false;

static void LoadFonts(ImGuiIO &io)
{
    // The system face. Present on every Windows since Vista; if it is somehow
    // missing we fall through to the built-in one rather than fail to start.
    static const char *kPath = "C:\\Windows\\Fonts\\consola.ttf";

    ImFontConfig cfg;
    cfg.OversampleH = 2;        // 1 is what made the default face mushy
    cfg.OversampleV = 1;
    cfg.PixelSnapH = false;

    for (int i = 0; i < WR_FONT_COUNT; i++)
    {
        ImFont *f = io.Fonts->AddFontFromFileTTF(kPath, kFontSizes[i], &cfg);
        if (!f)
            break;
        g_fonts[g_fontCount++] = f;
    }

    if (g_fontCount == WR_FONT_COUNT)
    {
        g_monospace = true;
        WrLogf("fonts: Consolas baked at %d sizes, %.0f-%.0f px", g_fontCount,
               kFontSizes[0], kFontSizes[WR_FONT_COUNT - 1]);
        return;
    }

    // Partial success is worse than none -- the size ladder would have holes.
    io.Fonts->Clear();
    g_fontCount = 0;
    g_monospace = false;
    g_fonts[g_fontCount++] = io.Fonts->AddFontDefault();
    WrLogf("[!] fonts: could not load %s; falling back to the built-in 13 px "
           "face. Large text will be scaled and will look soft.", kPath);
}

ImFont *WrFontFor(float wantPixels, float *actual)
{
    if (g_fontCount <= 0)
    {
        if (actual) *actual = ImGui::GetFontSize();
        return ImGui::GetFont();
    }

    // Nearest baked size, never an interpolation between two of them.
    int best = 0;
    float bestErr = 1e30f;
    for (int i = 0; i < g_fontCount; i++)
    {
        float err = kFontSizes[i] - wantPixels;
        if (err < 0.0f) err = -err;
        if (err < bestErr) { bestErr = err; best = i; }
    }
    if (actual)
        *actual = g_monospace ? kFontSizes[best] : wantPixels;
    return g_fonts[best];
}

bool WrFontIsMonospace(void) { return g_monospace; }

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

    // OUR ini, in OUR folder. It was NULL, which kept ImGui away from the game's
    // momentum\cfg\imgui.ini -- ImGui's default is a bare "imgui.ini" relative
    // to the working directory, which for an injected DLL is the game's, and
    // that file is the game's own devui's. Pointing it at an absolute path under
    // wrlines_data keeps that promise and also stops the panel forgetting where
    // it was every time the game restarts.
    //
    // Static, because ImGui keeps the POINTER rather than copying the string and
    // WrDataPath returns a rotating internal buffer.
    static char iniPath[MAX_PATH];
    strcpy_s(iniPath, sizeof(iniPath), WrDataPath("imgui.ini"));
    io.IniFilename = iniPath;
    io.LogFilename = NULL;

    // Draw our own cursor. The OS cursor is useless to us: Source hides it and
    // yanks it back to the screen centre every frame while mouselook is active,
    // so ImGui's normal GetCursorPos-based position would be pinned to the
    // middle of the screen and nothing would be draggable.
    io.MouseDrawCursor = true;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    LoadFonts(io);

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

    // Both panels are drawn from the same stage timer. They are two windows in
    // one ImGui frame, not two frames, so the cost of the pair is the number
    // Diagnostics should be showing.
    if (WrMenuOpen())
    {
        WrStageBegin(WR_STAGE_UI);
        if (WrPanelOpen(WR_PANEL_MAIN))
            WrUiDraw();
        if (WrPanelOpen(WR_PANEL_QUICK))
            WrQuickDraw();
        WrStageEnd(WR_STAGE_UI);
    }

    WrStageBegin(WR_STAGE_SUBMIT);
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    WrStageEnd(WR_STAGE_SUBMIT);

    if (prev && prev != g_ctx)
        ImGui::SetCurrentContext(prev);
}
