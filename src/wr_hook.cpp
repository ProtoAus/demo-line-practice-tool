// wr_hook.cpp  --  get onto the game's render thread, and take input when asked.
//
// Present is not an exported function, so to hook it we need a real
// IDXGISwapChain vtable. We make a throwaway 1x1 swapchain on a hidden window,
// read slots 8 (Present) and 13 (ResizeBuffers), and release it.
//
// The one subtlety that matters here: D3D11CreateDeviceAndSwapChain is resolved
// out of GetModuleHandleW(L"d3d11.dll") -- the module the game ALREADY loaded --
// never with LoadLibrary. This install has DXVK parked in bin\win64\New folder\
// and it has been used before; if it is ever moved back next to the exe, a
// LoadLibrary would happily give us a second, different d3d11 implementation and
// we would hook function pointers belonging to a swapchain the game never
// touches. Reading the vtable from the already-loaded module means we get the
// right implementation whichever one that is, and the COM layout is identical
// either way. For the same reason nothing here links d3d11.lib or dxgi.lib --
// only dxguid.lib, which is a static GUID blob with no DLL behind it.
//
// We inline-hook the resolved addresses with MinHook rather than writing the
// vtable slots, so the hook survives the game destroying and recreating its
// swapchain, and so we play nicely with anything else already hooking DXGI
// (Steam overlay, RTSS).
//
// WHAT THIS DELIBERATELY DOESN'T DO
//   It never unhooks and the DLL never unloads. Tearing down a Present hook
//   while another thread is inside the trampoline is the classic injected-DLL
//   crash-on-exit, and there is no user-visible benefit to supporting it.

#include "wr_hook.h"
#include "wr_log.h"
#include "wr_imgui.h"
#include "wr_render.h"
#include "wr_limit.h"

// Defined in dllmain.cpp: per-frame bookkeeping that must run even on frames we
// do not draw. Declared here rather than pulled in through a header so this
// translation unit keeps its short include list.
void WrIdleTick(void);

#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <string.h>

// Not windowsx.h: it defines a SubclassWindow() macro that collides with the
// function of the same name below, and all we wanted from it was two accessors
// that are one cast each.
#define WR_LPARAM_X(lp) ((int)(short)LOWORD(lp))
#define WR_LPARAM_Y(lp) ((int)(short)HIWORD(lp))

#include "MinHook.h"

// Slot indices in the IDXGISwapChain vtable. IDXGISwapChain1/2/3 are strict
// supersets, so these are correct no matter which interface the game holds.
#define VT_PRESENT 8
#define VT_RESIZEBUFFERS 13

typedef HRESULT (WINAPI *Present_t)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT (WINAPI *ResizeBuffers_t)(IDXGISwapChain *, UINT, UINT, UINT,
                                          DXGI_FORMAT, UINT);
typedef HRESULT (WINAPI *D3D11CreateDeviceAndSwapChain_t)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
    UINT, UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **,
    ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

static Present_t g_origPresent = NULL;
static ResizeBuffers_t g_origResizeBuffers = NULL;

static ID3D11Device *g_device = NULL;
static ID3D11DeviceContext *g_context = NULL;
static ID3D11RenderTargetView *g_rtv = NULL;
static IDXGISwapChain *g_swapChain = NULL;
static HWND g_window = NULL;
static WNDPROC g_origWndProc = NULL;
static bool g_wndUnicode = true;

static int g_bbWidth = 0, g_bbHeight = 0;
static volatile LONG g_menuOpen = 0;
static float g_curX = 0.0f, g_curY = 0.0f;
static bool g_cursorFollowsOS = false;

// The OS pointer is only a source of truth while it MOVES when the mouse moves.
// Under Wine that is not the same thing as GetCursorInfo reporting CURSOR_SHOWING
// -- see WrCursorUpdate -- so these three track whether it is actually keeping up.
static ULONGLONG g_rawMotionTick = 0;   // when raw mouse motion last arrived
static unsigned long g_rawMotionSeq = 0; // bumped by every raw motion packet
static unsigned long g_rawMotionSeen = 0; // the value at the previous frame's check
static long g_osLastX = -1, g_osLastY = -1;
static int g_osStaleFrames = 0;
static bool g_cursorShowing = false;     // what the last GetCursorInfo said

// WR_OS_CURSOR_STALE_FRAMES lives in wr_hook.h, because Diagnostics shows the
// running count against it and two copies of the number would drift.

// Whether any raw mouse packet has arrived SINCE THE PANEL WAS OPENED, which is
// what arms the WM_MOUSEMOVE fallback in WrWndProc. Reset by WrSetPanelOpen.
//
// This has now been wrong in both directions, so both are written down.
//
// It began as "have we EVER seen raw input", a flag set by the first packet of
// the session and never cleared -- so one packet at any time permanently
// disabled the fallback, including on the setups that only deliver raw input
// while the pointer is grabbed and therefore need it most.
//
// The fix for that was "did raw input arrive in the last second", and that is
// the bug this replaces: idleness is not absence. Source warps the OS cursor
// back to the middle of the window every frame it holds mouselook, and every
// one of those warps posts a WM_MOUSEMOVE carrying the CENTRE. Move the mouse,
// read a tooltip for a second, and the freshness window lapses -- then the next
// warp message is believed and the pointer is thrown to the middle of the
// screen. Reported as "the mouse keeps centering when I stop moving for 1sec,
// annoying for reading tool tips", and it would repeat every second.
//
// Since the panel opened is the test that separates the two cases without
// either failure. A setup with no raw input has seen none since it opened, so
// the fallback is armed. A setup with working raw input has, so a still mouse
// simply leaves the cursor where the user left it -- which is the whole point
// of a cursor. And it re-arms on every open, so nothing carries over from a
// time when the game was in a menu and the pointer behaved differently.
static bool g_rawSeenSinceOpen = false;
static bool g_renderReady = false;
static bool g_isDxvk = false;
static char g_d3d11Path[MAX_PATH] = {0};
static bool g_presentPreHooked = false;
static char g_presentBytes[32] = {0};
static unsigned int g_framesDrawn = 0;
static unsigned int g_framesSkipped = 0;

bool WrMenuOpen(void) { return g_menuOpen != 0; }
bool WrPanelOpen(unsigned int which) { return (g_menuOpen & (LONG)which) != 0; }
HWND WrGameWindow(void) { return g_window; }
ID3D11Device *WrDevice(void) { return g_device; }
ID3D11DeviceContext *WrContext(void) { return g_context; }
bool WrIsDxvk(void) { return g_isDxvk; }
const char *WrD3D11Path(void) { return g_d3d11Path; }
bool WrPresentPreHooked(void) { return g_presentPreHooked; }
const char *WrPresentFirstBytes(void) { return g_presentBytes; }
bool WrWndProcInstalled(void) { return g_origWndProc != NULL; }

void WrFrameCounts(unsigned int *drawn, unsigned int *skipped)
{
    if (drawn) *drawn = g_framesDrawn;
    if (skipped) *skipped = g_framesSkipped;
}

void WrBackbufferSize(int *w, int *h)
{
    if (w) *w = g_bbWidth;
    if (h) *h = g_bbHeight;
}

void WrVirtualCursor(float *x, float *y)
{
    if (x) *x = g_curX;
    if (y) *y = g_curY;
}

bool WrCursorFollowsOS(void) { return g_cursorFollowsOS; }

// Decide once per frame where our cursor should come from. Doing this here
// rather than in the message handler means the answer is consistent for the
// whole frame, which is what stops the pointer jittering between two sources.
//
// THE RULE IS "DOES IT MOVE", NOT "DOES WINDOWS SAY IT IS VISIBLE".
//
// It used to be the second one, and on Windows the two agree: a game holding
// mouselook hides the pointer, GetCursorInfo says so, and we integrate raw
// deltas into a cursor of our own. Under Wine they can disagree -- the pointer
// is reported as showing while the game has it grabbed and yanked back to the
// screen centre every frame. The old code believed the report, set
// g_cursorFollowsOS, and WrImGuiFrame then turned OFF the drawn cursor because
// the real one was supposedly right there. The result was a panel with no
// pointer at all, pinned at the centre, which is exactly what was reported from
// Linux: "insert doesn't bring cursor on its own, so you need to go to any menu
// with cursor available before using insert". Opening a game menu stops the
// recentring, so the two sources agree again and it starts working.
//
// So the OS pointer has to earn it: if the mouse is demonstrably moving (raw
// packets are arriving) and the reported position does not change, it is not a
// cursor, it is a number stuck at 960,540. This never fires on Windows, because
// there the report is already false in that state.
void WrCursorUpdate(void)
{
    g_cursorFollowsOS = false;
    if (!g_window)
        return;
    // Two syscalls whose only consumer is the panel. Nothing reads the result
    // while it is shut, so do not pay for them 300 times a second.
    if (!WrMenuOpen())
        return;

    // Did the mouse move since the last time we looked?
    unsigned long seq = g_rawMotionSeq;
    bool moved = (seq != g_rawMotionSeen);
    g_rawMotionSeen = seq;

    CURSORINFO ci;
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING))
    {
        // Hidden: the game has mouselook, integrate raw deltas. Nothing to be
        // stale about, so the count starts again from here.
        g_cursorShowing = false;
        g_osStaleFrames = 0;
        g_osLastX = g_osLastY = -1;
        return;
    }
    g_cursorShowing = true;

    POINT p = ci.ptScreenPos;
    if (!ScreenToClient(g_window, &p))
        return;

    if (moved && p.x == g_osLastX && p.y == g_osLastY)
    {
        if (g_osStaleFrames < WR_OS_CURSOR_STALE_FRAMES)
            g_osStaleFrames++;
    }
    else
    {
        g_osStaleFrames = 0;
    }
    g_osLastX = p.x;
    g_osLastY = p.y;

    // Self-correcting in both directions: the moment the reported position
    // starts tracking again -- a game menu opening, mouselook released -- the
    // count resets and the OS pointer is trusted again on the next frame.
    if (g_osStaleFrames >= WR_OS_CURSOR_STALE_FRAMES)
        return;

    g_curX = WrClampF((float)p.x, 0.0f, (float)g_bbWidth);
    g_curY = WrClampF((float)p.y, 0.0f, (float)g_bbHeight);
    g_cursorFollowsOS = true;
}

// For the Diagnostics tab. This cannot be reproduced from Windows, so the panel
// has to be able to answer it from the machine where it happens -- one
// screenshot instead of a round trip per guess.
void WrCursorDiag(bool *followsOs, bool *cursorShowing, int *staleFrames,
                  double *rawAgeSeconds, bool *moveFallbackArmed)
{
    if (followsOs)     *followsOs = g_cursorFollowsOS;
    if (cursorShowing) *cursorShowing = g_cursorShowing;
    if (staleFrames)   *staleFrames = g_osStaleFrames;
    // Armed means WM_MOUSEMOVE is allowed to place the cursor. On a machine
    // where the pointer jumps to the middle of the screen, this is the line
    // that says whether that is us believing a warp message.
    if (moveFallbackArmed) *moveFallbackArmed = !g_rawSeenSinceOpen;
    if (rawAgeSeconds)
    {
        *rawAgeSeconds = g_rawMotionTick
            ? (double)(GetTickCount64() - g_rawMotionTick) / 1000.0
            : -1.0;     // never seen at all, which is itself the answer
    }
}

static void SubclassWindow(HWND hwnd);
static void UnsubclassWindow(void);

// The only place the open set changes, and the only place the window procedure
// goes in or comes out.
//
// THE EDGE IS THE WHOLE POINT. Input ownership belongs to "any panel is up", not
// to either panel, so it is taken when the set goes from empty to non-empty and
// given back when it goes the other way. Closing the quick menu while the full
// one is still open used to be expressible only as "unsubclass", which would
// have left the panel still on screen and unable to hear a key.
//
// InterlockedOr and InterlockedAnd hand back the value BEFORE the change, which
// is what makes the edge test a comparison of two numbers this thread owns
// rather than a read-modify-write with a gap in the middle. That gap is not
// hypothetical here: the hotkey thread opens, the window thread closes on ESC,
// and the render thread catches up if the window was not known yet. Three
// writers, one flag, and the last time this file had a race between two of them
// the game hung unkillably -- see SubclassWindow.
void WrSetPanelOpen(unsigned int which, bool open)
{
    const LONG bits = (LONG)which;
    LONG before, after;
    if (open)
    {
        before = InterlockedOr(&g_menuOpen, bits);
        after = before | bits;
    }
    else
    {
        before = InterlockedAnd(&g_menuOpen, ~bits);
        after = before & ~bits;
    }

    if (before == after)
        return;                     // already in that state; say nothing

    if (before == 0)
    {
        // Start the virtual cursor in the middle rather than wherever the OS
        // cursor happens to have been parked by the game's recentring.
        g_curX = g_bbWidth * 0.5f;
        g_curY = g_bbHeight * 0.5f;
        // And start the "is the OS pointer keeping up" test from scratch, so a
        // verdict reached during the last time the panel was open cannot carry
        // into this one -- the game may have been in a menu then and not now.
        g_osStaleFrames = 0;
        g_osLastX = g_osLastY = -1;
        g_rawMotionSeen = g_rawMotionSeq;
        // Arms the WM_MOUSEMOVE fallback for this opening only. A device that
        // delivered raw input last time may not this time, and the other way
        // round -- so the question is asked again rather than remembered.
        g_rawSeenSinceOpen = false;
        // The window procedure goes in only for as long as a panel is up. See
        // SubclassWindow for why.
        SubclassWindow(g_window);
    }
    else if (after == 0)
    {
        UnsubclassWindow();
    }

    WrLogf("panel %s %s (open now: %s%s)",
           (which & WR_PANEL_QUICK) ? "quick" : "main",
           open ? "opened" : "closed",
           (after & WR_PANEL_MAIN) ? "main " : "",
           (after & WR_PANEL_QUICK) ? "quick" : (after ? "" : "none"));
}

void WrSetMenuOpen(bool open) { WrSetPanelOpen(WR_PANEL_MAIN, open); }

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

// imgui_impl_win32.h keeps this prototype inside a #if 0 block so the header
// does not have to drag in <windows.h>, so every host has to declare it by hand.
// Written without IMGUI_IMPL_API on purpose: that macro lives in imgui.h, which
// this translation unit has no other reason to include, and it expands to
// nothing in a normal static build anyway.
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static void AccumulateRawMouse(LPARAM lParam)
{
    UINT size = 0;
    if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &size,
                        sizeof(RAWINPUTHEADER)) != 0)
        return;
    if (size == 0 || size > sizeof(RAWINPUT) * 4)
        return;

    BYTE buf[sizeof(RAWINPUT) * 4];
    if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buf, &size,
                        sizeof(RAWINPUTHEADER)) != size)
        return;

    const RAWINPUT *ri = (const RAWINPUT *)buf;
    if (ri->header.dwType != RIM_TYPEMOUSE)
        return;
    if (ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)
        return;     // absolute devices (tablets) are not worth handling here

    // A sequence number and a timestamp rather than the one-way "we have seen
    // raw input at some point" flag this used to set. The flag was never
    // cleared, so a single packet at any time in the session permanently
    // disabled the WM_MOUSEMOVE fallback below -- the only thing keeping the
    // panel usable when raw input stops arriving.
    g_rawMotionSeq++;
    g_rawMotionTick = GetTickCount64();
    g_rawSeenSinceOpen = true;

    // Only integrate deltas when we are not already tracking the OS cursor;
    // otherwise the two sources would add together and the pointer would run
    // away at double speed.
    if (g_cursorFollowsOS)
        return;

    g_curX = WrClampF(g_curX + (float)ri->data.mouse.lLastX, 0.0f, (float)g_bbWidth);
    g_curY = WrClampF(g_curY + (float)ri->data.mouse.lLastY, 0.0f, (float)g_bbHeight);
}

static LRESULT CALLBACK WrWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (WrMenuOpen() && g_renderReady)
    {
        // ESC closes EVERY panel. Handled before anything else sees it, and not
        // forwarded, so it does not also fall through to the game.
        //
        // All of them rather than the topmost, because ESC here means "give me
        // the game back" and the only way to be sure it does is to close the lot.
        // Closing one and leaving the other would take the key and appear to do
        // nothing, which is the worst of the three available behaviours.
        if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == VK_ESCAPE)
        {
            WrSetPanelOpen(WR_PANEL_ALL, false);
            return 0;
        }

        // Raw input carries mouselook. Consume it without forwarding, or the
        // camera spins while the panel is open. It never goes to the ImGui
        // backend either -- we integrate it into our own cursor instead.
        if (msg == WM_INPUT)
        {
            AccumulateRawMouse(lParam);
            return 0;
        }

        // Everything except mouse MOTION goes to ImGui: buttons, wheel, keys,
        // characters, focus. Motion is deliberately withheld, because the
        // backend would turn it into a position event carrying the OS cursor --
        // the very thing Source keeps yanking back to screen centre. Our
        // position is queued separately in WrImGuiFrame.
        if (msg != WM_MOUSEMOVE)
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);

        switch (msg)
        {
        case WM_MOUSEMOVE:
            // If raw input is not arriving (some setups run without it, and
            // some only deliver it while the pointer is grabbed) fall back to
            // the message's own client coordinates, so the panel is still
            // usable rather than having no pointer at all.
            //
            // "Recently", not "ever". This used to test a flag that was set by
            // the first raw packet of the session and never cleared, which
            // turned the fallback off permanently the moment it was needed
            // least.
            // NOT "has raw input gone quiet", which is what a user reading a
            // tooltip looks like. See g_rawSeenSinceOpen.
            if (!g_rawSeenSinceOpen && !g_cursorFollowsOS)
            {
                g_curX = WrClampF((float)WR_LPARAM_X(lParam), 0.0f, (float)g_bbWidth);
                g_curY = WrClampF((float)WR_LPARAM_Y(lParam), 0.0f, (float)g_bbHeight);
            }
            return 0;

        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_KEYDOWN: case WM_KEYUP:
        case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        case WM_CHAR:
            return 0;

        default:
            break;
        }
    }

    // Read once into a local. The panel can be closed from the hotkey thread
    // while a message is being handled here, and that restores the original
    // procedure and clears the global -- calling through a NULL would be a crash
    // in a code path that is otherwise never exercised.
    //
    // The second test is a hard stop, not a tidy-up: if we ever end up recorded
    // as our own predecessor, calling through it recurses until the window
    // thread dies and the game hangs unkillably. The locking in SubclassWindow
    // is what prevents that; this is here so no future path can bring it back.
    WNDPROC orig = g_origWndProc;
    if (orig == (WNDPROC)WrWndProc)
        orig = NULL;
    if (!orig)
        return g_wndUnicode ? DefWindowProcW(hwnd, msg, wParam, lParam)
                            : DefWindowProcA(hwnd, msg, wParam, lParam);

    return g_wndUnicode ? CallWindowProcW(orig, hwnd, msg, wParam, lParam)
                        : CallWindowProcA(orig, hwnd, msg, wParam, lParam);
}

// Installed when the panel opens, removed when it closes.
//
// It used to go in at the first Present and stay for the whole session, which is
// exactly what other overlays watch for -- SpecialK monitors the window
// procedure and complains when something replaces it. Since the only consumer is
// the panel, and the INSERT hotkey is polled on its own thread rather than
// coming through here, there is no reason to hold it during normal play.
//
// BOTH OF THESE ARE CALLED FROM MORE THAN ONE THREAD, AND THAT MATTERS
//
// The hotkey thread opens the panel; the render thread catches up if the panel
// was opened before the window was known; the window thread itself closes the
// panel on ESC. The first version of this guarded on g_origWndProc being null,
// which is not a guard at all across threads: the hotkey thread set the
// menu-open flag and then subclassed, and in the gap between those two the
// render thread saw "menu open, not subclassed yet" and subclassed as well.
//
// Both calls succeeded, so the second one got OUR OWN procedure back as the
// "previous" one and stored it. Every message then went WrWndProc ->
// CallWindowProc(WrWndProc) -> WrWndProc, forever. The window thread stopped
// pumping messages, so the game hung and could not even be closed.
//
// Hence the lock, and hence reading the current procedure before writing rather
// than trusting what SetWindowLongPtr hands back.
static CRITICAL_SECTION g_wndCs;
static bool g_wndCsReady = false;

static void SubclassWindow(HWND hwnd)
{
    if (!hwnd || !g_wndCsReady)
        return;

    EnterCriticalSection(&g_wndCs);
    if (!g_origWndProc)
    {
        bool unicode = IsWindowUnicode(hwnd) != FALSE;
        WNDPROC current = unicode
            ? (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_WNDPROC)
            : (WNDPROC)GetWindowLongPtrA(hwnd, GWLP_WNDPROC);

        // Already ours with no original recorded? Then something is wrong and
        // installing again would build the self-referential chain described
        // above. Do nothing; the panel keeps working either way.
        if (current == (WNDPROC)WrWndProc)
        {
            WrLogf("[!] window already carries our procedure but no original was "
                   "recorded -- not installing again");
        }
        else
        {
            g_wndUnicode = unicode;
            g_origWndProc = current;
            if (unicode)
                SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)WrWndProc);
            else
                SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)WrWndProc);
            WrLogf("subclassed window %p (%s), original %p", (void *)hwnd,
                   unicode ? "unicode" : "ansi", (void *)current);
        }
    }
    LeaveCriticalSection(&g_wndCs);
}

static void UnsubclassWindow(void)
{
    if (!g_wndCsReady)
        return;

    EnterCriticalSection(&g_wndCs);
    if (g_origWndProc && g_window)
    {
        // Only unhook if we are still the top of the chain. If something else
        // subclassed on top of us in the meantime, restoring the original would
        // cut that other thing out entirely -- a much worse bug than leaving
        // ours in.
        WNDPROC current = g_wndUnicode
            ? (WNDPROC)GetWindowLongPtrW(g_window, GWLP_WNDPROC)
            : (WNDPROC)GetWindowLongPtrA(g_window, GWLP_WNDPROC);
        if (current != (WNDPROC)WrWndProc)
        {
            WrLogf("[!] not restoring the window procedure: something subclassed "
                   "on top of us (now %p). Leaving ours in the chain.",
                   (void *)current);
        }
        else
        {
            if (g_wndUnicode)
                SetWindowLongPtrW(g_window, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);
            else
                SetWindowLongPtrA(g_window, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);
            g_origWndProc = NULL;
            WrLogf("window procedure restored");
        }
    }
    LeaveCriticalSection(&g_wndCs);
}

// ---------------------------------------------------------------------------
// Render target
// ---------------------------------------------------------------------------

static void ReleaseRenderTarget(void)
{
    if (g_rtv)
    {
        g_rtv->Release();
        g_rtv = NULL;
    }
}

static bool CreateRenderTarget(IDXGISwapChain *sc)
{
    ID3D11Texture2D *back = NULL;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back)) || !back)
        return false;

    D3D11_TEXTURE2D_DESC td;
    back->GetDesc(&td);
    g_bbWidth = (int)td.Width;
    g_bbHeight = (int)td.Height;

    HRESULT hr = g_device->CreateRenderTargetView(back, NULL, &g_rtv);
    back->Release();
    return SUCCEEDED(hr) && g_rtv != NULL;
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

static bool AcquireDeviceFrom(IDXGISwapChain *sc)
{
    if (g_device)
        return true;

    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void **)&g_device)) || !g_device)
    {
        WrLogf("[!] swapchain GetDevice failed -- not a D3D11 swapchain?");
        return false;
    }
    g_device->GetImmediateContext(&g_context);

    DXGI_SWAP_CHAIN_DESC sd;
    if (SUCCEEDED(sc->GetDesc(&sd)))
    {
        g_window = sd.OutputWindow;     // far more reliable than FindWindow
        g_bbWidth = (int)sd.BufferDesc.Width;
        g_bbHeight = (int)sd.BufferDesc.Height;
        WrLogf("swapchain: %dx%d fmt=%d buffers=%u windowed=%d hwnd=%p",
               g_bbWidth, g_bbHeight, (int)sd.BufferDesc.Format,
               sd.BufferCount, (int)sd.Windowed, (void *)g_window);
    }

    g_swapChain = sc;
    return g_window != NULL;
}

// Every real present goes through here, so no early exit below can skip it.
//
// The pacing wait itself is NOT here. It happens at the top of HookedPresent,
// before the camera matrix is read and before anything is drawn, and that
// ordering is the point: whatever we wait, we wait it BEFORE sampling the
// matrix, not between sampling it and presenting.
//
// It used to be the other way round -- read the matrix, draw the lines, then
// hold the frame for up to a millisecond, then present. The lines were therefore
// always a whole wait older than the frame they landed on, and the bigger the
// gap between the game's own frame rate and the cap, the further behind they
// sat. With the cap off the wait is zero and the effect vanishes, which is
// exactly the reported behaviour: lines a frame behind, but only when capped.
static inline HRESULT PacedPresent(IDXGISwapChain *sc, UINT interval, UINT flags)
{
    return g_origPresent(sc, interval, flags);
}

static HRESULT WINAPI HookedPresent(IDXGISwapChain *sc, UINT interval, UINT flags)
{
    // DXGI_PRESENT_TEST is a visibility probe, not a real frame: doing work here
    // is wasted and can confuse the flip-model presenter. It must not be paced
    // either -- it is not a frame, and waiting on one would be waiting on
    // nothing.
    if (flags & DXGI_PRESENT_TEST)
        return g_origPresent(sc, interval, flags);

    // Pace here, first, before anything else in the frame.
    //
    // By this point the game has finished rendering, so holding the frame is
    // legitimate wherever it happens -- but everything after this line reads the
    // camera matrix and draws with it, and doing that AFTER the wait is what
    // keeps the lines on the same frame as the world they are drawn over.
    WrLimitTick();

    if (!g_device)
    {
        if (AcquireDeviceFrom(sc))
        {
            if (CreateRenderTarget(sc) && WrImGuiInit(g_window, g_device, g_context))
            {
                g_renderReady = true;
                WrLogf("render path ready");
            }
        }
    }

    if (!g_renderReady)
        return PacedPresent(sc, interval, flags);

    // The panel can be opened before the window is known, in which case
    // WrSetMenuOpen had nothing to subclass. Catch up here.
    if (WrMenuOpen() && !g_origWndProc)
        SubclassWindow(g_window);

    // Bookkeeping that has to keep running whether or not we draw: map changes,
    // the matrix scan, energy sampling. Cheap, and none of it touches the device.
    WrStageBegin(WR_STAGE_IDLE);
    WrIdleTick();
    WrStageEnd(WR_STAGE_IDLE);

    // Nothing to draw? Then touch nothing at all -- no render target binding, no
    // ImGui frame, no draw calls, no device state saved or restored. Present
    // becomes a straight passthrough plus the bookkeeping above.
    //
    // This is not only about saving work. A frame limiter that paces itself
    // inside its own Present hook -- SpecialK's does -- oscillates badly when
    // the cost below it varies from frame to frame, which is what turned a
    // steady 300 fps into 150 bouncing around.
    if (!WrHasAnythingToDraw())
    {
        g_framesSkipped++;
        return PacedPresent(sc, interval, flags);
    }

    if (!g_rtv)
        CreateRenderTarget(sc);

    if (g_rtv)
    {
        // Save what was bound so we can put it back. We used to leave the
        // pipeline pointing at our render target, and since ImGui's own state
        // backup happens *after* this it faithfully restored ours rather than
        // the game's -- so anything drawing later in the same Present, which is
        // exactly what another overlay is, inherited it.
        ID3D11RenderTargetView *prevRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {0};
        ID3D11DepthStencilView *prevDsv = NULL;
        g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                      prevRtv, &prevDsv);

        // Under the flip model the render target is unbound after every
        // Present, so this has to happen every frame, not once at init.
        g_context->OMSetRenderTargets(1, &g_rtv, NULL);
        WrImGuiFrame();
        g_framesDrawn++;

        g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                      prevRtv, prevDsv);
        // OMGetRenderTargets AddRefs everything it hands back.
        for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++)
            if (prevRtv[i])
                prevRtv[i]->Release();
        if (prevDsv)
            prevDsv->Release();
    }

    return PacedPresent(sc, interval, flags);
}

static HRESULT WINAPI HookedResizeBuffers(IDXGISwapChain *sc, UINT bufferCount,
                                          UINT width, UINT height,
                                          DXGI_FORMAT format, UINT flags)
{
    // ResizeBuffers fails with DXGI_ERROR_INVALID_CALL if anything still holds a
    // reference to a back buffer, so our view has to go first and be rebuilt
    // afterwards. This is the number one crash source in overlays.
    WrLogf("ResizeBuffers %ux%u (was %dx%d)", width, height, g_bbWidth, g_bbHeight);
    ReleaseRenderTarget();
    WrImGuiInvalidateDeviceObjects();

    HRESULT hr = g_origResizeBuffers(sc, bufferCount, width, height, format, flags);

    if (g_device)
        CreateRenderTarget(sc);
    WrImGuiCreateDeviceObjects();
    return hr;
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

static HMODULE WaitForD3D11(int timeoutMs)
{
    for (int waited = 0; waited < timeoutMs; waited += 100)
    {
        HMODULE m = GetModuleHandleW(L"d3d11.dll");
        if (m)
            return m;
        Sleep(100);
    }
    return NULL;
}

// Create a throwaway swapchain purely to read the vtable, then release it.
static bool ReadSwapChainVTable(HMODULE d3d11, void **outPresent, void **outResize)
{
    D3D11CreateDeviceAndSwapChain_t create =
        (D3D11CreateDeviceAndSwapChain_t)GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain");
    if (!create)
    {
        WrLogf("[!] d3d11.dll has no D3D11CreateDeviceAndSwapChain");
        return false;
    }

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "WrLinesProbe";
    if (!RegisterClassExA(&wc))
    {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            WrLogf("[!] RegisterClassEx failed (%lu)", GetLastError());
            return false;
        }
    }

    HWND tmp = CreateWindowExA(0, wc.lpszClassName, "WrLines", WS_OVERLAPPEDWINDOW,
                               0, 0, 8, 8, NULL, NULL, wc.hInstance, NULL);
    if (!tmp)
    {
        WrLogf("[!] probe window creation failed (%lu)", GetLastError());
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd;
    memset(&sd, 0, sizeof(sd));
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 8;
    sd.BufferDesc.Height = 8;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = tmp;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    IDXGISwapChain *sc = NULL;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;

    // WARP first, hardware only as a fallback.
    //
    // All we want is the vtable layout, which belongs to the loaded d3d11.dll
    // and not to any adapter, so a software device answers the question exactly
    // as well. It also avoids handing other overlays something confusing to
    // reason about: SpecialK hooks D3D11CreateDeviceAndSwapChain and tracks the
    // swapchains it sees, and a phantom hardware swapchain appearing on a hidden
    // 8x8 window mid-session is not a helpful thing to show it. DXVK may not
    // offer WARP at all, hence the fallback.
    D3D_DRIVER_TYPE types[] = { D3D_DRIVER_TYPE_WARP, D3D_DRIVER_TYPE_HARDWARE };
    HRESULT hr = E_FAIL;
    for (int t = 0; t < 2 && !sc; t++)
    {
        hr = create(NULL, types[t], NULL, 0, want,
                    (UINT)(sizeof(want) / sizeof(want[0])), D3D11_SDK_VERSION,
                    &sd, &sc, &dev, &got, &ctx);
        if (SUCCEEDED(hr) && sc)
            WrLogf("vtable read from a %s device",
                   types[t] == D3D_DRIVER_TYPE_WARP ? "WARP (software)" : "hardware");
    }

    bool ok = false;
    if (SUCCEEDED(hr) && sc)
    {
        void **vt = *(void ***)sc;
        *outPresent = vt[VT_PRESENT];
        *outResize = vt[VT_RESIZEBUFFERS];
        ok = true;

        // If something has already hooked DXGI (Steam overlay, RTSS, SpecialK)
        // the first bytes will be a jump. Worth having in the log -- and in
        // Diagnostics -- before we add ourselves on top of it.
        const unsigned char *p = (const unsigned char *)*outPresent;
        _snprintf_s(g_presentBytes, sizeof(g_presentBytes), _TRUNCATE,
                    "%02X %02X %02X %02X %02X", p[0], p[1], p[2], p[3], p[4]);
        // E9 = rel32 jmp, FF 25 = indirect jmp, 48 B8 = mov rax, imm64 (the
        // usual opening of a 64-bit detour).
        g_presentPreHooked = (p[0] == 0xE9) || (p[0] == 0xFF && p[1] == 0x25) ||
                             (p[0] == 0x48 && p[1] == 0xB8) || (p[0] == 0xEB);
        WrLogf("Present @ %p  first bytes %s%s", *outPresent, g_presentBytes,
               g_presentPreHooked ? "   (already hooked by something else)" : "");
        WrLogf("ResizeBuffers @ %p", *outResize);
    }
    else
    {
        WrLogf("[!] probe device creation failed hr=0x%08lX", (unsigned long)hr);
    }

    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(tmp);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return ok;
}

bool WrHookInit(void)
{
    // Before any hook is live and before the hotkey thread exists, so nothing
    // can be racing for it yet.
    InitializeCriticalSection(&g_wndCs);
    g_wndCsReady = true;

    HMODULE d3d11 = WaitForD3D11(60000);
    if (!d3d11)
    {
        WrLogf("[!] d3d11.dll never appeared after 60s -- giving up");
        return false;
    }

    if (GetModuleFileNameA(d3d11, g_d3d11Path, MAX_PATH))
    {
        // DXVK ships its own d3d11.dll; the game folder path is the tell.
        //
        // Except under Proton, where the path test gets it exactly backwards:
        // Proton installs DXVK's d3d11.dll INTO the prefix's system32, so the
        // path contains \windows\ precisely in the case where DXVK is most
        // certain. Diagnostics has been reporting "DXVK  no" on every Proton
        // install since the line was written. This is display-only -- nothing
        // in the hook path branches on it (see wr_hook.h) -- but a diagnostic
        // that is always wrong on one platform is worse than no diagnostic.
        const char *lower = g_d3d11Path;
        char low[MAX_PATH];
        strcpy_s(low, MAX_PATH, lower);
        _strlwr_s(low, MAX_PATH);
        g_isDxvk = (strstr(low, "\\windows\\") == NULL) || WrIsWine();
        WrLogf("d3d11.dll: %s%s", g_d3d11Path, g_isDxvk ? "   (not system -- DXVK?)" : "");
    }

    void *pPresent = NULL, *pResize = NULL;
    if (!ReadSwapChainVTable(d3d11, &pPresent, &pResize))
        return false;

    if (MH_Initialize() != MH_OK)
    {
        WrLogf("[!] MH_Initialize failed");
        return false;
    }
    if (MH_CreateHook(pPresent, (void *)&HookedPresent, (void **)&g_origPresent) != MH_OK ||
        MH_CreateHook(pResize, (void *)&HookedResizeBuffers, (void **)&g_origResizeBuffers) != MH_OK)
    {
        WrLogf("[!] MH_CreateHook failed");
        return false;
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
    {
        WrLogf("[!] MH_EnableHook failed");
        return false;
    }

    WrLogf("hooks installed; waiting for the first Present");
    return true;
}
