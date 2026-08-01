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

#endif // WR_IMGUI_H
