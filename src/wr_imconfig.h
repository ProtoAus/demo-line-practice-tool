// wr_imconfig.h  --  Dear ImGui build configuration for WrLines.
//
// Pulled in via /DIMGUI_USER_CONFIG="wr_imconfig.h" rather than by editing
// imgui/imconfig.h, so the vendored ImGui tree stays exactly as cloned and can
// be re-pulled or diffed against upstream without losing anything.
//
// The one setting here that is not cosmetic is ImDrawIdx. A single AddPolyline
// call for a full run path can emit well over 65536 vertices, and while
// ImGuiBackendFlags_RendererHasVtxOffset covers most large-buffer cases it does
// not help when one draw command alone exceeds the 16-bit index range. The DX11
// backend picks DXGI_FORMAT_R32_UINT off sizeof(ImDrawIdx) automatically.

#pragma once

#define ImDrawIdx unsigned int

// Not linked, not needed, and it keeps the DLL smaller.
#define IMGUI_DISABLE_DEMO_WINDOWS

// Note: we do NOT define IMGUI_DISABLE_DEFAULT_FILE_FUNCTIONS. That flag expects
// the host to supply its own ImFileOpen/Read/Write/Close, and we have no reason
// to. Keeping ImGui away from the game's momentum\cfg\imgui.ini is handled where
// it actually matters -- io.IniFilename and io.LogFilename are set to NULL in
// wr_imgui.cpp, so no file is ever opened in the first place.
