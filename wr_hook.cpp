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
static bool g_sawRawInput = false;
static bool g_renderReady = false;
static bool g_isDxvk = false;
static char g_d3d11Path[MAX_PATH] = {0};

bool WrMenuOpen(void) { return g_menuOpen != 0; }
HWND WrGameWindow(void) { return g_window; }
ID3D11Device *WrDevice(void) { return g_device; }
ID3D11DeviceContext *WrContext(void) { return g_context; }
bool WrIsDxvk(void) { return g_isDxvk; }
const char *WrD3D11Path(void) { return g_d3d11Path; }

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
void WrCursorUpdate(void)
{
    g_cursorFollowsOS = false;
    if (!g_window)
        return;

    CURSORINFO ci;
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING))
        return;     // hidden: the game has mouselook, integrate raw deltas

    POINT p = ci.ptScreenPos;
    if (!ScreenToClient(g_window, &p))
        return;

    g_curX = WrClampF((float)p.x, 0.0f, (float)g_bbWidth);
    g_curY = WrClampF((float)p.y, 0.0f, (float)g_bbHeight);
    g_cursorFollowsOS = true;
}

void WrSetMenuOpen(bool open)
{
    InterlockedExchange(&g_menuOpen, open ? 1 : 0);
    if (open)
    {
        // Start the virtual cursor in the middle rather than wherever the OS
        // cursor happens to have been parked by the game's recentring.
        g_curX = g_bbWidth * 0.5f;
        g_curY = g_bbHeight * 0.5f;
    }
    WrLogf("menu %s", open ? "opened" : "closed");
}

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

    g_sawRawInput = true;

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
        // ESC closes the panel. Handled before anything else sees it, and not
        // forwarded, so it does not also fall through to the game.
        if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == VK_ESCAPE)
        {
            WrSetMenuOpen(false);
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
            // If raw input never arrives (some setups run without it) fall back
            // to using the message's own client coordinates, so the panel is
            // still usable rather than having no pointer at all.
            if (!g_sawRawInput && !g_cursorFollowsOS)
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

    return g_wndUnicode ? CallWindowProcW(g_origWndProc, hwnd, msg, wParam, lParam)
                        : CallWindowProcA(g_origWndProc, hwnd, msg, wParam, lParam);
}

static void SubclassWindow(HWND hwnd)
{
    if (g_origWndProc || !hwnd)
        return;
    g_wndUnicode = IsWindowUnicode(hwnd) != FALSE;
    g_origWndProc = g_wndUnicode
        ? (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)WrWndProc)
        : (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)WrWndProc);
    WrLogf("subclassed window %p (%s)", (void *)hwnd, g_wndUnicode ? "unicode" : "ansi");
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

static HRESULT WINAPI HookedPresent(IDXGISwapChain *sc, UINT interval, UINT flags)
{
    // DXGI_PRESENT_TEST is a visibility probe, not a real frame: doing work here
    // is wasted and can confuse the flip-model presenter.
    if (flags & DXGI_PRESENT_TEST)
        return g_origPresent(sc, interval, flags);

    if (!g_device)
    {
        if (AcquireDeviceFrom(sc))
        {
            SubclassWindow(g_window);
            if (CreateRenderTarget(sc) && WrImGuiInit(g_window, g_device, g_context))
            {
                g_renderReady = true;
                WrLogf("render path ready");
            }
        }
    }

    if (g_renderReady)
    {
        if (!g_rtv)
            CreateRenderTarget(sc);

        if (g_rtv)
        {
            // Under the flip model the render target is unbound after every
            // Present, so this has to happen every frame, not once at init.
            g_context->OMSetRenderTargets(1, &g_rtv, NULL);
            WrImGuiFrame();
        }
    }

    return g_origPresent(sc, interval, flags);
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

    HRESULT hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want,
                        (UINT)(sizeof(want) / sizeof(want[0])), D3D11_SDK_VERSION,
                        &sd, &sc, &dev, &got, &ctx);
    bool ok = false;
    if (SUCCEEDED(hr) && sc)
    {
        void **vt = *(void ***)sc;
        *outPresent = vt[VT_PRESENT];
        *outResize = vt[VT_RESIZEBUFFERS];
        ok = true;

        // If something has already hooked DXGI (Steam overlay, RTSS, SpecialK)
        // the first bytes will be a jump. Worth having in the log before we add
        // ourselves on top of it.
        const unsigned char *p = (const unsigned char *)*outPresent;
        WrLogf("Present @ %p  first bytes %02X %02X %02X %02X %02X",
               *outPresent, p[0], p[1], p[2], p[3], p[4]);
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
    HMODULE d3d11 = WaitForD3D11(60000);
    if (!d3d11)
    {
        WrLogf("[!] d3d11.dll never appeared after 60s -- giving up");
        return false;
    }

    if (GetModuleFileNameA(d3d11, g_d3d11Path, MAX_PATH))
    {
        // DXVK ships its own d3d11.dll; the game folder path is the tell.
        const char *lower = g_d3d11Path;
        char low[MAX_PATH];
        strcpy_s(low, MAX_PATH, lower);
        _strlwr_s(low, MAX_PATH);
        g_isDxvk = (strstr(low, "\\windows\\") == NULL);
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
