// wr_engine.h  --  resolved engine access, and what it costs.
//
// The entire visible feature needs exactly ONE guessed vtable index:
// IVEngineClient::WorldToScreenMatrix. Everything else is either free or
// optional:
//
//   * screen size          -> DXGI_SWAP_CHAIN_DESC, not GetScreenSize
//   * camera position      -> solved out of the world->clip matrix itself
//   * current map name     -> GetLevelName, needed only to match runs to a map
//   * console commands     -> ClientCmd, needed only for in-engine capture
//
// That is why this is feasible against a closed-source engine at all, and it is
// worth keeping true: every additional index is another thing that can silently
// move under a game update.
//
// Every accessor returns a bool "did this work" rather than trusting the engine,
// so each caller degrades instead of crashing.

#ifndef WR_ENGINE_H
#define WR_ENGINE_H

#include "wr_common.h"

enum WrResolveSource
{
    WR_SRC_NONE = 0,
    WR_SRC_HYPOTHESIS,      // found by probing around the expected index
    WR_SRC_INI,             // supplied by wrlines_offsets.ini
    WR_SRC_WIDE,            // found by a full-vtable sweep
};

struct WrMethodInfo
{
    const char *name;
    int index;
    int vtableSize;
    WrResolveSource source;
    bool resolved;
    char note[128];         // what the oracle saw when it accepted
};

// Finds engine.dll / client.dll, gets the interfaces, and resolves the methods.
// Safe to call every frame; it does at most one probe call per invocation so a
// hang is always attributable to a single slot.
void WrEngineTick(void);

bool WrEngineReady(void);           // WorldToScreenMatrix resolved
bool WrEngineHasLevelName(void);
bool WrEngineHasClientCmd(void);

// The world->clip matrix for the frame currently being composited. Only valid
// inside the Present hook.
bool WrWorldToScreen(VMatrix *out);

// Camera position, solved from the world->clip matrix. No entity list, no extra
// vtable risk. See wr_engine.cpp for the derivation.
bool WrCameraOrigin(Vec3 *out);
bool WrCameraForward(Vec3 *out);

// The same direction as Source's own angles: yaw anticlockwise from +X, pitch
// positive downward. Exact -- the matrix oracle will not accept a basis that is
// not orthonormal or that fails to reproject, so this is the game's view
// direction read rather than anything estimated.
bool WrCameraYaw(float *out);
bool WrCameraPitch(float *out);

// Bare map name, e.g. "surf_demise" (no "maps/" prefix, no ".bsp").
//
// Read out of momentum\demoheader.tmp, which the engine rewrites on every map
// load, and validated against an installed .bsp before it is believed. No
// vtable call is involved, so this keeps working even if nothing else resolves.
const char *WrLevelName(void);

// Manual override, for when auto-detection is wrong or you want to look at a
// map you are not currently standing in. Empty string clears it.
void WrSetMapOverride(const char *map);
const char *WrMapOverride(void);

void WrClientCmd(const char *cmd);

// Where the matrix we are drawing with came from.
enum WrMatrixSource
{
    WR_MAT_NONE = 0,
    WR_MAT_SCAN,        // found by reading memory (the default, and safe)
    WR_MAT_VTABLE,      // found by probing IVEngineClient (opt-in, can crash)
};
WrMatrixSource WrMatrixSourceNow(void);

// The oracle, shared with wr_scan.cpp. Closed-loop and self-contained: 16 finite
// floats, a plausible w-row, a camera origin inside world bounds, and a point
// 512 units down the view axis landing near screen centre.
bool WrValidateW2S(const VMatrix &m, char *note, int noteLen);
bool WrSolveCameraOrigin(const VMatrix &m, Vec3 *out);

// Vtable probing is OFF by default and has to be turned on explicitly.
//
// It is not merely risky, it is unfixably risky: calling an unknown method on
// the live engine object corrupted state and killed the game a moment *after*
// the call returned cleanly, which no amount of SEH, shadow-`this` or crash
// blacklisting can catch. Reading memory (wr_scan) gets the same matrix without
// executing anything, so probing now exists only as a manual fallback.
void WrEngineSetProbing(bool on);
bool WrEngineProbingEnabled(void);

// Diagnostics
int WrMethodCount(void);
const WrMethodInfo *WrMethodAt(int i);
const char *WrEngineStatus(void);
void WrEngineRequestReprobe(bool wide);

// Force a specific vtable index for WorldToScreenMatrix. It is still put through
// the same oracle before being used, so a wrong number is rejected rather than
// pointing the renderer at whatever happens to live there.
void WrEngineTryIndexFor(const char *method, int index);

#endif // WR_ENGINE_H
