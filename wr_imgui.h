// wr_imgui.h  --  our own Dear ImGui context, living alongside the game's.
//
// Momentum ships Dear ImGui 1.92.5 inside devui.dll with a Source materialsystem
// backend. We do NOT reuse it: its IDevUISystem vtable is undocumented, some of
// its windows are sv_cheats-gated, and binding to it would tie us to a private
// engine ABI. Instead we build our own context with the stock Win32+DX11
// backends. Two ImGui contexts coexisting in one process is fine -- GImGui is a
// per-module global and wrlines.dll exports nothing, so the two can never bind
// to each other's symbols.

#ifndef WR_IMGUI_H
#define WR_IMGUI_H

#include "wr_common.h"

struct ID3D11Device;
struct ID3D11DeviceContext;

bool WrImGuiInit(HWND hwnd, ID3D11Device *device, ID3D11DeviceContext *context);
void WrImGuiFrame(void);
void WrImGuiInvalidateDeviceObjects(void);
void WrImGuiCreateDeviceObjects(void);

// The baked font nearest `wantPixels`, and the size it should actually be drawn
// at. Text must be drawn at a size the atlas holds: asking AddText for 66 pixels
// from a 13-pixel atlas is what made the readout blurry, and no sampler setting
// fixes glyphs that were never rasterised at that size.
struct ImFont;
ImFont *WrFontFor(float wantPixels, float *actual);

// True when the real (monospaced) face loaded. Layout that depends on digits
// being fixed-width has to know, because the fallback face is proportional.
bool WrFontIsMonospace(void);

#endif // WR_IMGUI_H
