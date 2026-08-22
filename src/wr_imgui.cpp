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

#include <stdio.h>      // _snprintf_s, for the font search
#include <string.h>

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
static bool g_baked = false;        // the ladder above is real, so nothing is scaled
static bool g_monospace = false;    // and the face is fixed-width, so widths are constant

// TWO flags rather than one, because they used to be the same flag and they are
// not the same fact. "Every size in the ladder exists" is what stops text being
// magnified; "the digits are all the same width" is what stops the corner block
// sliding around. A proportional face baked at the right sizes has the first
// property without the second, and that combination is worth having -- see
// LoadFonts.

// Where the faces come from.
//
// This used to be one hardcoded string, C:\Windows\Fonts\consola.ttf, and on
// Windows that is still the first thing tried and still what gets used. But
// Consolas is a Microsoft font and a Wine prefix does not have one, so every
// Linux user has been falling all the way through to the 13 px bitmap face that
// this whole baked ladder exists to get rid of -- and had no way to know it,
// because blurry is exactly what the tool looked like before the ladder existed.
//
// So ask Windows where its font directory is, which inside a prefix is the
// prefix's own, and work down. The order is deliberate: named monospaced faces
// first, then anything calling itself mono, and only then a proportional face,
// because sharp and proportional still beats blurry and proportional and the
// only thing given up is the fixed-width digits that WrFontIsMonospace() guards.
static const char *kMonoFonts[] = {
    "consola.ttf",                  // Consolas -- every Windows since Vista
    "lucon.ttf",                    // Lucida Console -- older Windows
    "cour.ttf",                     // Courier New, and Wine's substitute for it
    "DejaVuSansMono.ttf",
    "LiberationMono-Regular.ttf",
    "NotoSansMono-Regular.ttf",
    "UbuntuMono-R.ttf",
    "FreeMono.ttf",
};

static const char *kAnyFonts[] = {
    "tahoma.ttf",                   // Wine ships this one itself
    "segoeui.ttf",
    "arial.ttf",
    "LiberationSans-Regular.ttf",
    "DejaVuSans.ttf",
};

#define WR_ARRAY_LEN(a) (int)(sizeof(a) / sizeof((a)[0]))

static bool FontDir(char *out, size_t cap)
{
    char win[MAX_PATH];
    UINT n = GetWindowsDirectoryA(win, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return false;
    return _snprintf_s(out, cap, _TRUNCATE, "%s\\Fonts", win) >= 0;
}

static bool FontExists(const char *dir, const char *name, char *out, size_t cap)
{
    if (_snprintf_s(out, cap, _TRUNCATE, "%s\\%s", dir, name) < 0)
        return false;
    return GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES;
}

// Whatever this prefix actually has. `want` is a lowercase substring the name
// must contain, or NULL for anything at all.
static bool FontScan(const char *dir, const char *want, char *out, size_t cap)
{
    char pattern[MAX_PATH];
    if (_snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*.ttf", dir) < 0)
        return false;

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    bool found = false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (want)
        {
            char low[MAX_PATH];
            strcpy_s(low, sizeof(low), fd.cFileName);
            _strlwr_s(low, sizeof(low));
            if (!strstr(low, want))
                continue;
        }
        found = _snprintf_s(out, cap, _TRUNCATE, "%s\\%s", dir, fd.cFileName) >= 0;
    } while (!found && FindNextFileA(h, &fd));

    FindClose(h);
    return found;
}

// One file, every size in the ladder, all or nothing. Partial success is worse
// than none: WrFontFor picks the NEAREST baked size, so a ladder with holes in
// it would quietly go back to scaling for the sizes that were missing, which is
// the exact bug this file exists to have fixed.
static bool BakeLadder(ImGuiIO &io, const char *path)
{
    ImFontConfig cfg;
    cfg.OversampleH = 2;        // 1 is what made the default face mushy
    cfg.OversampleV = 1;
    cfg.PixelSnapH = false;

    int n = 0;
    for (int i = 0; i < WR_FONT_COUNT; i++)
    {
        ImFont *f = io.Fonts->AddFontFromFileTTF(path, kFontSizes[i], &cfg);
        if (!f)
            break;
        g_fonts[n++] = f;
    }
    if (n == WR_FONT_COUNT)
    {
        g_fontCount = n;
        return true;
    }

    io.Fonts->Clear();
    g_fontCount = 0;
    return false;
}

static void LoadFonts(ImGuiIO &io)
{
    char dir[MAX_PATH] = {0};
    char path[MAX_PATH];

    g_fontCount = 0;
    g_baked = false;
    g_monospace = false;

    if (FontDir(dir, sizeof(dir)))
    {
        for (int i = 0; i < WR_ARRAY_LEN(kMonoFonts); i++)
        {
            if (!FontExists(dir, kMonoFonts[i], path, sizeof(path)))
                continue;
            if (!BakeLadder(io, path))
                continue;
            g_baked = g_monospace = true;
            WrLogf("fonts: %s baked at %d sizes, %.0f-%.0f px (monospaced)",
                   path, g_fontCount, kFontSizes[0], kFontSizes[WR_FONT_COUNT - 1]);
            return;
        }

        if (FontScan(dir, "mono", path, sizeof(path)) && BakeLadder(io, path))
        {
            g_baked = g_monospace = true;
            WrLogf("fonts: %s baked at %d sizes (monospaced, found by name)",
                   path, g_fontCount);
            return;
        }

        // Proportional, but baked, which is the half that matters most: the
        // readout stops being a magnified bitmap. The corner block will change
        // width with the digits in it, and that is what WrFontIsMonospace() is
        // for -- the layout asks, rather than assuming.
        for (int i = 0; i < WR_ARRAY_LEN(kAnyFonts); i++)
        {
            if (!FontExists(dir, kAnyFonts[i], path, sizeof(path)))
                continue;
            if (!BakeLadder(io, path))
                continue;
            g_baked = true;
            WrLogf("[i] fonts: no monospaced face here, so %s is baked at %d "
                   "sizes instead. Text is sharp; the crosshair readout will "
                   "change width as the numbers do.", path, g_fontCount);
            return;
        }

        if (FontScan(dir, NULL, path, sizeof(path)) && BakeLadder(io, path))
        {
            g_baked = true;
            WrLogf("[i] fonts: falling back to %s, baked at %d sizes. Text is "
                   "sharp; the crosshair readout will change width as the "
                   "numbers do.", path, g_fontCount);
            return;
        }
    }

    io.Fonts->Clear();
    g_fontCount = 0;
    g_baked = false;
    g_monospace = false;
    g_fonts[g_fontCount++] = io.Fonts->AddFontDefault();
    WrLogf("[!] fonts: no usable TTF in %s; falling back to the built-in 13 px "
           "face. Large text will be scaled and will look soft.",
           dir[0] ? dir : "the system font directory");
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
    // g_baked, not g_monospace: the question here is whether the size on the
    // ladder is a size that really exists, which is true for any baked face.
    // Whether it is fixed-width is a separate question, and a different caller's.
    if (actual)
        *actual = g_baked ? kFontSizes[best] : wantPixels;
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
        // Edit mode owns the screen until Done. Keeping the settings panel open
        // behind it both covers useful drag space and lets that window affect
        // which handle receives the mouse, so the panel deliberately disappears
        // and returns on the frame after editing finishes.
        if (WrHudEditorOpen())
            WrHudEditorDraw();
        else if (WrPanelOpen(WR_PANEL_MAIN))
            WrUiDraw();
        if (!WrHudEditorOpen() && WrPanelOpen(WR_PANEL_QUICK))
            WrQuickDraw();
        WrStageEnd(WR_STAGE_UI);
    }
    else if (WrHudEditorOpen())
    {
        // Closing the menu is also a natural way to finish editing. Leaving the
        // mode latched would make the four drag windows unexpectedly return the
        // next time Insert is pressed.
        WrHudEditorSetOpen(false);
    }

    WrStageBegin(WR_STAGE_SUBMIT);
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    WrStageEnd(WR_STAGE_SUBMIT);

    if (prev && prev != g_ctx)
        ImGui::SetCurrentContext(prev);
}
