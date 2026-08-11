// wr_hook.h  --  D3D11 swapchain hooking and window input for WrLines.

#ifndef WR_HOOK_H
#define WR_HOOK_H

#include "wr_common.h"

struct ID3D11Device;
struct ID3D11DeviceContext;

// Blocks until the game's d3d11.dll is loaded, reads the swapchain vtable and
// installs the Present / ResizeBuffers hooks. Returns false if it gave up.
bool WrHookInit(void);

// --- panels, and the one thing they share -----------------------------------
//
// There are two now: the full panel on Insert and the quick one on Delete. They
// are independent as far as being ON SCREEN goes, and they are not independent
// at all as far as INPUT goes -- there is one window procedure, one virtual
// cursor and one matrix-scan hold between them, and the game gets its input back
// exactly when the last of them closes.
//
// So "open" became a bitmask and WrMenuOpen kept its meaning: ANY panel is up.
// Its eight consumers -- the cursor, the draw decision, the message handler, the
// scan hold -- all wanted that question and not "is the main panel up", so none
// of them changed. What changed is that subclassing the window happens on the
// 0 <-> non-zero EDGE, so closing one panel while the other is still open no
// longer blinds the one left behind.
//
// A bitmask rather than a count because the same panel can be told to open twice
// -- the hotkey thread and the X button both go through here -- and a count
// would then need one close per open, which nothing tracks.
enum WrPanel
{
    WR_PANEL_MAIN  = 1 << 0,        // Insert
    WR_PANEL_QUICK = 1 << 1         // Delete
};
#define WR_PANEL_ALL (WR_PANEL_MAIN | WR_PANEL_QUICK)

bool WrMenuOpen(void);              // any of them
bool WrPanelOpen(unsigned int which);
void WrSetPanelOpen(unsigned int which, bool open);

// WR_PANEL_MAIN, spelled the way it was before there were two. Kept because the
// X button and every settings site say "the menu" and mean this one.
void WrSetMenuOpen(bool open);

HWND WrGameWindow(void);
ID3D11Device *WrDevice(void);
ID3D11DeviceContext *WrContext(void);
void WrBackbufferSize(int *w, int *h);

// Where our cursor currently is, in client pixels.
//
// Two sources, chosen per frame by WrCursorUpdate:
//   * The game is showing the OS cursor (a game menu is open) -> we follow it
//     exactly, so there is no offset and nothing to fight.
//   * The game has hidden and captured it (normal play) -> we integrate raw
//     input deltas ourselves, because the OS cursor is pinned to screen centre
//     and useless.
void WrVirtualCursor(float *x, float *y);
void WrCursorUpdate(void);
bool WrCursorFollowsOS(void);

// True when the loaded d3d11.dll is DXVK rather than the system one. Shown in
// Diagnostics; nothing in the hook path depends on it.
bool WrIsDxvk(void);
const char *WrD3D11Path(void);

// --- coexisting with other overlays -----------------------------------------
//
// Provided by wr_render.cpp. When this is false the whole draw path is skipped
// inside Present: no render target is bound, no ImGui frame is built, nothing is
// submitted and no device state is touched. That matters beyond saving work --
// a frame limiter that paces itself inside its own Present hook (SpecialK does)
// oscillates when the cost below it varies frame to frame.
bool WrHasAnythingToDraw(void);

// True if something had already hooked Present before we got there.
bool WrPresentPreHooked(void);
const char *WrPresentFirstBytes(void);

// Is our window procedure currently installed? It goes in when the panel opens
// and comes out when it closes, so during normal play the game's window is
// untouched.
bool WrWndProcInstalled(void);

// Frames Present ran with nothing for us to do, and frames we actually drew.
void WrFrameCounts(unsigned int *drawn, unsigned int *skipped);

#endif // WR_HOOK_H
