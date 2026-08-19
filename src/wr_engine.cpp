// wr_engine.cpp  --  getting the world->screen matrix out of a closed-source
// engine, and the oracles that decide whether we actually have it.
//
// HOW THIS CHANGED, AND WHY
//
// The original design probed IVEngineClient's vtable for WorldToScreenMatrix,
// with every guard I could think of: code-pointer validation, a shadow `this`,
// distinct scratch arguments, SEH, one call per frame, and a crash-resume
// blacklist. It found GetScreenSize correctly at index 38 -- and then killed the
// game, repeatedly, *without* leaving a breadcrumb. The probe call returned
// normally and the process died a moment later somewhere unrelated.
//
// That is the failure mode none of those guards can catch. There is nothing to
// blacklist, because by the time it crashes the guilty call is long gone. It
// means no probe window is safe, not a narrower one and not a better-predicted
// one, so narrowing the window was never going to fix it.
//
// The matrix is just 16 floats in the game's own writable memory, rewritten
// every frame. So we read memory instead of executing it -- see wr_scan.cpp.
// Scanning cannot corrupt anything because it never writes and never transfers
// control, and the oracle below is strong enough to identify the matrix on sight.
//
// Probing still exists, off by default, reachable from Diagnostics, for the case
// where scanning genuinely finds nothing.
//
// Each method gets an oracle: something we can check the result against that we
// already know independently. A probe is only accepted when its oracle passes,
// so "resolved" means observed-correct rather than assumed-correct.
//
//   GetScreenSize        must write exactly the DXGI backbuffer dimensions,
//                        which we already have. A perfect oracle -- it cannot
//                        pass by accident. Used purely as a canary to prove the
//                        interface pointer is real before anything else runs.
//
//   GetLevelName         must return printable ASCII naming a map that exists,
//                        either "maps/<x>.bsp" or a bare name matching an
//                        installed .bsp.
//
//   WorldToScreenMatrix  closed loop, needs no external data: 16 finite floats,
//                        a w-row of plausible length, a camera origin inside the
//                        world bounds, and -- the real test -- reprojecting a
//                        point 512 units straight ahead of that origin must land
//                        near the centre of the screen. Confirmed over three
//                        consecutive frames so garbage cannot pass once by luck.
//
//   ClientCmd            no return value to check, so a side effect is used:
//                        "host_writeconfig wrlines_probe" must produce
//                        momentum\cfg\wrlines_probe.cfg.
//
// We do not probe IsInGame/IsConnected at all: a bool-returning nullary has no
// unique oracle -- far too many slots return 0 or 1 -- and a resolved
// GetLevelName already answers the same question.

#include "wr_engine.h"
#include "wr_probe.h"
#include "wr_pe.h"
#include "wr_log.h"
#include "wr_hook.h"
#include "wr_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef void *(*CreateInterfaceFn)(const char *name, int *retCode);

// Interface version strings verified by scanning the shipped engine.dll. These
// are exact-match in Source's CreateInterface, so a game update that bumps one
// will show up as a clean "interface not found" in the log rather than as
// undefined behaviour.
#define IFACE_ENGINE_CLIENT "VEngineClient015"

// Starting hypotheses, from the CS:GO-branch SDK that Strata forked. Treated
// strictly as a place to start looking, never as fact.
#define HYP_GETSCREENSIZE 5
#define HYP_CLIENTCMD 7
#define HYP_GETLEVELNAME 26
#define HYP_W2S 36
// Tight, because an unanchored SDK index is barely a guess at all.
#define PROBE_WINDOW 8
// Once anchored the prediction is good, so search around it -- but keep the band
// tight. Every probe is a call into an unknown method, and the crash we already
// hit came from walking blindly up the vtable. Starting AT the prediction and
// expanding outward means the likely answer is reached in the first few frames,
// before the search ever gets near anything dangerous. If this band misses, the
// answer is to type an index in Diagnostics, not to widen it.
#define ANCHORED_WINDOW 16

enum
{
    M_GETSCREENSIZE = 0,
    M_GETLEVELNAME,
    M_W2S,
    M_CLIENTCMD,
    M_COUNT
};

static WrMethodInfo g_methods[M_COUNT];
static bool g_gaveUp[M_COUNT];
static bool g_autoWideTried[M_COUNT];
static int g_hint[M_COUNT];
static bool g_hintTried[M_COUNT];
static bool g_offsetsLoaded = false;
static int g_forcedIndex[M_COUNT];      // -1 = none; set from the UI / ini
static int g_forcedTries[M_COUNT];

static void LoadOffsetHints(void);
static void SaveOffsets(void);
static HMODULE g_engineMod = NULL;
static void *g_engineClient = NULL;
static int g_engineVtSize = 0;
static char g_status[256] = "starting up";
static char g_levelName[128] = {0};
static char g_mapOverride[72] = {0};

static bool g_wantWide = false;
static int g_wideCursor = 0;
static int g_phase = 0;

// Off by default. See the header comment: an unknown vtable call took the game
// down asynchronously, which is not something the probe guards can defend
// against. wr_scan.cpp gets us the same matrix without calling anything.
static bool g_probeEnabled = false;
static WrMatrixSource g_matSource = WR_MAT_NONE;

// W2S confirmation state
static int g_w2sStreak = 0;
static int g_w2sCandidate = -1;

// Latest good matrix, refreshed every frame from the render thread.
static VMatrix g_w2s;
static bool g_w2sValid = false;
static Vec3 g_camOrigin;
static Vec3 g_camForward;
static bool g_camValid = false;

int WrMethodCount(void) { return M_COUNT; }
const WrMethodInfo *WrMethodAt(int i)
{
    return (i >= 0 && i < M_COUNT) ? &g_methods[i] : NULL;
}
const char *WrEngineStatus(void) { return g_status; }
bool WrEngineReady(void) { return g_methods[M_W2S].resolved || WrScanResolved(); }
WrMatrixSource WrMatrixSourceNow(void) { return g_matSource; }
bool WrEngineProbingEnabled(void) { return g_probeEnabled; }

void WrEngineSetProbing(bool on)
{
    if (g_probeEnabled == on)
        return;
    g_probeEnabled = on;
    WrLogf("vtable probing %s", on ? "ENABLED by request -- this can crash the game"
                                   : "disabled");
    if (!on)
    {
        g_wantWide = false;
        for (int i = 0; i < M_COUNT; i++)
            g_forcedIndex[i] = -1;
    }
}
bool WrEngineHasLevelName(void) { return g_methods[M_GETLEVELNAME].resolved; }
bool WrEngineHasClientCmd(void) { return g_methods[M_CLIENTCMD].resolved; }

void WrEngineRequestReprobe(bool wide)
{
    for (int i = 0; i < M_COUNT; i++)
    {
        g_methods[i].resolved = false;
        g_methods[i].index = -1;
        g_methods[i].source = WR_SRC_NONE;
        g_methods[i].note[0] = '\0';
        g_gaveUp[i] = false;
        g_autoWideTried[i] = false;
        g_hintTried[i] = true;      // an explicit re-probe should ignore hints
    }
    g_wantWide = wide;
    g_wideCursor = 0;
    g_phase = 0;
    g_w2sStreak = 0;
    g_w2sCandidate = -1;
    WrEngineSetProbing(true);       // asking for a probe is the opt-in
    WrLogf("re-probe requested (%s)", wide ? "wide sweep" : "windowed");
}

// ---------------------------------------------------------------------------
// Matrix maths
// ---------------------------------------------------------------------------

// The camera origin is the one world point that projects to x = y = w = 0.
// Rows 0, 1 and 3 of the world->clip matrix give three planes through it, so it
// falls out of a 3x3 solve with partial pivoting.
bool WrSolveCameraOrigin(const VMatrix &m, Vec3 *out)
{
    double a[3][4];
    for (int r = 0; r < 3; r++)
    {
        int src = (r == 2) ? 3 : r;     // rows 0, 1, 3
        a[r][0] = m.m[src][0];
        a[r][1] = m.m[src][1];
        a[r][2] = m.m[src][2];
        a[r][3] = -m.m[src][3];
    }

    for (int col = 0; col < 3; col++)
    {
        int pivot = col;
        for (int r = col + 1; r < 3; r++)
            if (fabs(a[r][col]) > fabs(a[pivot][col]))
                pivot = r;
        if (fabs(a[pivot][col]) < 1e-9)
            return false;
        if (pivot != col)
            for (int c = 0; c < 4; c++)
            {
                double t = a[col][c]; a[col][c] = a[pivot][c]; a[pivot][c] = t;
            }
        for (int r = 0; r < 3; r++)
        {
            if (r == col)
                continue;
            double f = a[r][col] / a[col][col];
            for (int c = col; c < 4; c++)
                a[r][c] -= f * a[col][c];
        }
    }

    double x = a[0][3] / a[0][0];
    double y = a[1][3] / a[1][1];
    double z = a[2][3] / a[2][2];
    if (!(x > -1e7 && x < 1e7) || !(y > -1e7 && y < 1e7) || !(z > -1e7 && z < 1e7))
        return false;

    out->x = (float)x;
    out->y = (float)y;
    out->z = (float)z;
    return WrSaneVec(*out);
}

static bool ProjectWith(const VMatrix &m, const Vec3 &p, int sw, int sh,
                        float *sx, float *sy)
{
    float x = m.m[0][0] * p.x + m.m[0][1] * p.y + m.m[0][2] * p.z + m.m[0][3];
    float y = m.m[1][0] * p.x + m.m[1][1] * p.y + m.m[1][2] * p.z + m.m[1][3];
    float w = m.m[3][0] * p.x + m.m[3][1] * p.y + m.m[3][2] * p.z + m.m[3][3];
    if (w < 0.001f)
        return false;
    float inv = 1.0f / w;
    *sx = (sw * 0.5f) + (0.5f * x * inv * sw);
    *sy = (sh * 0.5f) - (0.5f * y * inv * sh);
    return WrSaneFloat(*sx) && WrSaneFloat(*sy);
}

// Row 3 of a world->clip matrix is the view forward vector: w comes out as the
// view-space depth of the point. Which *sign* it carries is a convention, and
// getting it wrong makes the oracle test a point behind the camera and reject a
// perfectly good matrix. Rather than assume, pick the sign that puts the test
// point in front.
static Vec3 ForwardFromMatrix(const VMatrix &m, const Vec3 &origin)
{
    Vec3 f = WrNormalize(WrVec(m.m[3][0], m.m[3][1], m.m[3][2]));
    Vec3 p = WrAdd(origin, WrScale(f, 512.0f));
    float w = m.m[3][0] * p.x + m.m[3][1] * p.y + m.m[3][2] * p.z + m.m[3][3];
    if (w < 0.0f)
        f = WrScale(f, -1.0f);
    return f;
}

// Does this look like the world->clip matrix for the frame we are compositing?
//
// This is the whole basis for trusting 64 bytes found by scanning memory, so it
// has to be strict -- and the first version wasn't. It accepted, and picked, a
// *constant* projection matrix sitting in engine.dll's data. That is worth
// understanding, because it is the obvious trap:
//
//   world->clip  M = P . V,  where V is the view transform and P the projection.
//
// A bare P with no V folded in still has a unit w row, still solves to a camera
// origin (at the world origin, since V is identity), and still reprojects a
// point down the view axis to *exactly* screen centre. Every test based on "does
// this behave like a projection" passes, because it genuinely is one. It just
// isn't the one describing where the player is standing.
//
// What separates M from P is the view part, so the checks have to target that:
//
//   * M's rows 0, 1 and 3 are D00.Vx, D11.Vy and +-Vz, and V's rows are an
//     orthonormal basis. So those three directions must be mutually orthogonal.
//     Exact maths, not a heuristic -- a strong structural filter.
//
//   * |row1| / |row0| = D11/D00 = tan(fovx/2)/tan(fovy/2) = the ASPECT RATIO,
//     which we already know from the DXGI backbuffer. This is the one properly
//     independent oracle available without calling into the game, and it is
//     scale-invariant, so it survives any constant folded into the matrix. It
//     also disposes of every square-aspect matrix in the process -- shadow maps,
//     cubemap faces, render targets -- in one line.
//
//   * The solved camera origin must not be at the world origin, which is where
//     an unfolded projection matrix always puts it.
//
//   * Reprojection error under 1%, not 15%. A real view-projection lands the
//     axis point on screen centre to float precision; 12% was only ever noise
//     that happened to be pointing roughly forwards.
//
// And the final discriminator is temporal, in wr_scan.cpp: the real matrix
// changes every frame. A constant in .data never does. That check alone would
// have caught this one -- "12 hits, 0 updates" was in the log.
bool WrValidateW2S(const VMatrix &m, char *note, int noteLen)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (!WrSaneFloat(m.m[r][c]))
                return false;

    int sw = 0, sh = 0;
    WrBackbufferSize(&sw, &sh);
    if (sw <= 0 || sh <= 0)
        return false;

    Vec3 r0 = WrVec(m.m[0][0], m.m[0][1], m.m[0][2]);
    Vec3 r1 = WrVec(m.m[1][0], m.m[1][1], m.m[1][2]);
    Vec3 r3 = WrVec(m.m[3][0], m.m[3][1], m.m[3][2]);
    float l0 = WrLength(r0), l1 = WrLength(r1), l3 = WrLength(r3);

    // Row 3 is +-Vz, a unit vector, so this should be 1. The window allows for a
    // scale convention we have not seen rather than for noise.
    if (l3 < 0.8f || l3 > 1.25f)
        return false;
    if (l0 < 0.2f || l0 > 8.0f || l1 < 0.2f || l1 > 8.0f)
        return false;

    // Orthonormality of the extracted view basis.
    Vec3 vz = WrScale(r3, 1.0f / l3);
    Vec3 vx = WrScale(r0, 1.0f / l0);
    Vec3 vy = WrScale(r1, 1.0f / l1);
    if (fabsf(WrDot(vx, vz)) > 0.02f) return false;
    if (fabsf(WrDot(vy, vz)) > 0.02f) return false;
    if (fabsf(WrDot(vx, vy)) > 0.02f) return false;

    // The independent oracle: this matrix must be projecting for OUR viewport.
    float aspect = (float)sw / (float)sh;
    float mAspect = l1 / l0;
    if (fabsf(mAspect - aspect) > aspect * 0.10f)
        return false;

    // World bounds. 65536, not the 16384 that every Source reference quotes.
    //
    // Strata's maps are bigger than stock Source's. Measured from the demos
    // themselves, surf_colin_blaster_69000 runs out to -31295 on X, and while
    // the player was out there this test rejected the real matrix every frame:
    // no lines, no energy readout, nothing, on that map only, no matter what was
    // ticked. It read exactly like the tool being broken for one map.
    //
    // Widening it costs almost nothing, because this was never the check doing
    // the work -- the orthonormal basis, the aspect ratio against the actual
    // backbuffer, and a reprojection landing within 1% of screen centre are what
    // identify the matrix. This one only exists to throw out floats that are
    // nowhere near a coordinate.
    Vec3 origin;
    if (!WrSolveCameraOrigin(m, &origin))
        return false;
    if (fabsf(origin.x) > WR_WORLD_LIMIT || fabsf(origin.y) > WR_WORLD_LIMIT ||
        fabsf(origin.z) > WR_WORLD_LIMIT)
        return false;
    // A projection matrix with no view transform folded in solves to exactly
    // (0,0,0). Real players are not standing there.
    if (WrLength(origin) < 4.0f)
        return false;

    // A point straight down the view axis must land on screen centre.
    Vec3 ahead = WrAdd(origin, WrScale(ForwardFromMatrix(m, origin), 512.0f));
    float sx = 0.0f, sy = 0.0f;
    if (!ProjectWith(m, ahead, sw, sh, &sx, &sy))
        return false;

    float dx = fabsf(sx - sw * 0.5f) / (float)sw;
    float dy = fabsf(sy - sh * 0.5f) / (float)sh;
    if (dx > 0.01f || dy > 0.01f)
        return false;

    if (note)
    {
        float fovx = 2.0f * atanf(1.0f / l0) * 57.2957795f;
        _snprintf_s(note, noteLen, _TRUNCATE,
                    "cam (%.0f %.0f %.0f) fov %.0f aspect %.3f err %.2f%%",
                    origin.x, origin.y, origin.z, fovx, mAspect,
                    100.0f * (dx > dy ? dx : dy));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Live accessors
// ---------------------------------------------------------------------------

typedef const VMatrix &(__fastcall *W2SFn_t)(void *);

static bool FetchW2S(VMatrix *out)
{
    if (!g_methods[M_W2S].resolved || !g_engineClient)
        return false;

    void **vt = *(void ***)g_engineClient;
    W2SFn_t fn = (W2SFn_t)vt[g_methods[M_W2S].index];

    __try
    {
        const VMatrix &ref = fn(g_engineClient);
        return WrSafeReadBytes(&ref, out, sizeof(VMatrix));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool WrWorldToScreen(VMatrix *out)
{
    if (!g_w2sValid)
        return false;
    *out = g_w2s;
    return true;
}

bool WrCameraOrigin(Vec3 *out)
{
    if (!g_camValid)
        return false;
    *out = g_camOrigin;
    return true;
}

bool WrCameraForward(Vec3 *out)
{
    if (!g_camValid)
        return false;
    *out = g_camForward;
    return true;
}

// Where you are looking, as Source's own angles.
//
// The forward vector has been sitting here since the matrix scanner shipped and
// nothing ever turned it into an angle. It is worth knowing how good it is: the
// camera basis comes out of the world-to-screen matrix, and WrValidateW2S will
// not accept a matrix whose extracted basis is not orthonormal, whose aspect
// disagrees with the backbuffer, or which fails to reproject a point 512 units
// down the view axis to the screen centre within 1%. So this is not an estimate
// in the way the velocity is an estimate -- it is the game's own view direction,
// read rather than derived, and wr_energy.h already says as much about the turn
// rate built from it: "literally how fast the mouse is moving".
//
// Source's convention: yaw is degrees anticlockwise from +X, and pitch is
// POSITIVE DOWNWARD, which is why the z term is negated.
bool WrCameraYaw(float *out)
{
    if (!g_camValid)
        return false;
    *out = (float)(atan2((double)g_camForward.y, (double)g_camForward.x) *
                   57.29577951308232);
    return true;
}

bool WrCameraPitch(float *out)
{
    if (!g_camValid)
        return false;
    float z = WrClampF(g_camForward.z, -1.0f, 1.0f);
    *out = (float)(-asin((double)z) * 57.29577951308232);
    return true;
}

const char *WrLevelName(void)
{
    return g_mapOverride[0] ? g_mapOverride : g_levelName;
}

typedef void (__fastcall *ClientCmdFn_t)(void *, const char *);

void WrClientCmd(const char *cmd)
{
    if (!g_methods[M_CLIENTCMD].resolved || !g_engineClient || !cmd)
        return;
    void **vt = *(void ***)g_engineClient;
    ClientCmdFn_t fn = (ClientCmdFn_t)vt[g_methods[M_CLIENTCMD].index];
    __try
    {
        fn(g_engineClient, cmd);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        WrLogf("[!] ClientCmd raised on \"%s\"", cmd);
    }
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------

static bool AcquireInterfaces(void)
{
    if (g_engineClient)
        return true;

    g_engineMod = GetModuleHandleA("engine.dll");
    if (!g_engineMod)
        return false;
    WrPeRegister(g_engineMod);

    CreateInterfaceFn ci =
        (CreateInterfaceFn)GetProcAddress(g_engineMod, "CreateInterface");
    if (!ci)
    {
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "engine.dll has no CreateInterface export");
        return false;
    }

    int rc = 0;
    g_engineClient = ci(IFACE_ENGINE_CLIENT, &rc);
    if (!g_engineClient)
    {
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "CreateInterface(\"%s\") returned NULL -- version bumped?",
                    IFACE_ENGINE_CLIENT);
        WrLogf("[!] %s", g_status);
        return false;
    }

    void **vt = NULL;
    if (WrSafeReadBytes(g_engineClient, &vt, sizeof(vt)) && vt)
        g_engineVtSize = WrVTableLength(g_engineMod, vt, 256);

    WrLogf("%s @ %p  vtable size %d", IFACE_ENGINE_CLIENT,
           g_engineClient, g_engineVtSize);
    for (int i = 0; i < M_COUNT; i++)
        g_methods[i].vtableSize = g_engineVtSize;
    LoadOffsetHints();
    return true;
}

// ---------------------------------------------------------------------------
// Remembering what we found
// ---------------------------------------------------------------------------
//
// Sweeping 200 vtable entries is cheap (one probe per frame, so under a second)
// but it is not something to repeat every launch, and every probe is a call into
// an unknown method. So resolved indices are written to wrlines_offsets.ini and
// tried first next time.
//
// A remembered index is still put through its oracle before being used. That is
// the whole point: if a game update moves things, the stale hint simply fails
// validation and we fall back to probing, rather than quietly pointing the
// renderer at the wrong function.

static const char *OffsetsPath(void) { return WrDataPath("wrlines_offsets.ini"); }

static void LoadOffsetHints(void)
{
    if (g_offsetsLoaded)
        return;
    g_offsetsLoaded = true;
    for (int i = 0; i < M_COUNT; i++)
        g_hint[i] = -1;

    FILE *f = NULL;
    if (fopen_s(&f, OffsetsPath(), "r") != 0 || !f)
        return;

    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        if (line[0] == ';' || line[0] == '#' || line[0] == '[')
            continue;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char name[64];
        strcpy_s(name, sizeof(name), line);
        for (int i = (int)strlen(name) - 1; i >= 0 && (name[i] == ' ' || name[i] == '\t'); i--)
            name[i] = '\0';
        int idx = atoi(eq + 1);
        for (int m = 0; m < M_COUNT; m++)
            if (g_methods[m].name && _stricmp(g_methods[m].name, name) == 0)
                g_hint[m] = idx;
    }
    fclose(f);

    for (int m = 0; m < M_COUNT; m++)
        if (g_hint[m] >= 0)
            WrLogf("offsets.ini suggests %s = %d (will be validated)",
                   g_methods[m].name, g_hint[m]);
}

static void SaveOffsets(void)
{
    FILE *f = NULL;
    if (fopen_s(&f, OffsetsPath(), "w") != 0 || !f)
        return;
    fprintf(f, "; WrLines resolved vtable indices for %s\n", IFACE_ENGINE_CLIENT);
    fprintf(f, "; Written automatically. Every value here is re-validated at\n");
    fprintf(f, "; startup, so it is safe to keep after a game update -- a stale\n");
    fprintf(f, "; index just fails its check and gets probed for again.\n");
    fprintf(f, "; vtable size when written: %d\n\n", g_engineVtSize);
    fprintf(f, "[%s]\n", IFACE_ENGINE_CLIENT);
    for (int i = 0; i < M_COUNT; i++)
        if (g_methods[i].resolved)
            fprintf(f, "%-22s = %d\n", g_methods[i].name, g_methods[i].index);
    fclose(f);
}

static void Accept(int m, int index, WrResolveSource src, const char *note)
{
    g_methods[m].index = index;
    g_methods[m].resolved = true;
    g_methods[m].source = src;
    if (note)
        strcpy_s(g_methods[m].note, sizeof(g_methods[m].note), note);
    WrLogf("resolved %-20s index %-3d  (%s)", g_methods[m].name, index,
           note ? note : "");
    SaveOffsets();
}

// Try one index for one method. Returns true if it was accepted.
static bool TryIndex(int m, int index)
{
    if (index < 0 || (g_engineVtSize > 0 && index >= g_engineVtSize))
        return false;
    if (WrProbeIsBlacklisted(IFACE_ENGINE_CLIENT, index))
        return false;

    char note[128] = {0};

    if (m == M_W2S)
    {
        // Special-cased: this one has a real signature we can call directly, and
        // its oracle is strong enough that a probe-shaped call is unnecessary.
        void **vt = NULL;
        if (!WrSafeReadBytes(g_engineClient, &vt, sizeof(vt)) || !vt)
            return false;
        void *fn = NULL;
        if (!WrSafeReadBytes(&vt[index], &fn, sizeof(fn)) || !fn)
            return false;
        if (!WrIsCodeIn(g_engineMod, fn))
            return false;

        // Sweeping a 200-entry vtable means calling a lot of methods that are
        // not this one. Run each candidate through the shadow-`this` probe
        // first: anything that faults gets filtered out there, against a copy of
        // the object, instead of against the engine's live one. Only survivors
        // are then called for real -- which they must be, because the matrix we
        // want is reached through the true `this`.
        void *dummy = NULL;
        if (!WrProbeCall(g_engineMod, g_engineClient, index, &dummy))
            return false;

        WrProbeBegin(IFACE_ENGINE_CLIENT, index);
        VMatrix candidate;
        bool got = false;
        __try
        {
            const VMatrix &ref = ((W2SFn_t)fn)(g_engineClient);
            got = WrSafeReadBytes(&ref, &candidate, sizeof(candidate));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            got = false;
        }
        WrProbeEnd();

        if (!got || !WrValidateW2S(candidate, note, sizeof(note)))
        {
            g_w2sStreak = (g_w2sCandidate == index) ? g_w2sStreak : 0;
            return false;
        }

        // Require it to hold across consecutive frames: a one-off pass could be
        // luck, three in a row from a moving camera cannot be.
        if (g_w2sCandidate == index)
            g_w2sStreak++;
        else
        {
            g_w2sCandidate = index;
            g_w2sStreak = 1;
        }
        if (g_w2sStreak < 3)
            return false;

        Accept(M_W2S, index, g_wantWide ? WR_SRC_WIDE : WR_SRC_HYPOTHESIS, note);
        return true;
    }

    WrProbeBegin(IFACE_ENGINE_CLIENT, index);
    void *ret = NULL;
    bool called = WrProbeCall(g_engineMod, g_engineClient, index, &ret);
    WrProbeEnd();
    if (!called)
        return false;

    if (m == M_GETSCREENSIZE)
    {
        // Expect it to have written width and height through two out-params.
        int bw = 0, bh = 0;
        WrBackbufferSize(&bw, &bh);
        if (bw <= 0 || bh <= 0)
            return false;
        int *a = (int *)WrScratch(1);
        int *b = (int *)WrScratch(2);
        if (!a || !b)
            return false;
        if (*a == bw && *b == bh)
        {
            _snprintf_s(note, sizeof(note), _TRUNCATE,
                        "wrote %dx%d, matches DXGI", *a, *b);
            Accept(M_GETSCREENSIZE, index,
                   g_wantWide ? WR_SRC_WIDE : WR_SRC_HYPOTHESIS, note);
            return true;
        }
        return false;
    }

    if (m == M_GETLEVELNAME)
    {
        char buf[160];
        if (WrSafeReadString(ret, buf, sizeof(buf)) <= 0)
            return false;

        const char *bare = buf;
        if (_strnicmp(bare, "maps/", 5) == 0 || _strnicmp(bare, "maps\\", 5) == 0)
            bare += 5;

        char name[128];
        strcpy_s(name, sizeof(name), bare);
        size_t len = strlen(name);
        if (len > 4 && _stricmp(name + len - 4, ".bsp") == 0)
            name[len - 4] = '\0';
        if (!name[0])
            return false;

        // Confirm against the installed maps rather than trusting the shape.
        char bsp[MAX_PATH];
        _snprintf_s(bsp, sizeof(bsp), _TRUNCATE, "%s\\momentum\\maps\\%s.bsp",
                    WrGameDir(), name);
        if (GetFileAttributesA(bsp) == INVALID_FILE_ATTRIBUTES)
            return false;

        strcpy_s(g_levelName, sizeof(g_levelName), name);
        _snprintf_s(note, sizeof(note), _TRUNCATE, "\"%s\" (.bsp exists)", buf);
        Accept(M_GETLEVELNAME, index,
               g_wantWide ? WR_SRC_WIDE : WR_SRC_HYPOTHESIS, note);
        return true;
    }

    return false;
}

// Walk outward from a hypothesis: h, h-1, h+1, h-2, h+2, ...
static int WindowIndex(int hypothesis, int step)
{
    int off = (step + 1) / 2;
    return (step & 1) ? hypothesis - off : hypothesis + off;
}

// Where we actually expect a method to live, given what we have already found.
//
// This interface has 200 methods where the public SDK has under 40, so raw SDK
// indices are useless on their own. But once ONE method is located, the shift
// between the SDK layout and this one is measurable, and everything after it can
// be predicted rather than hunted for.
//
// GetScreenSize is the anchor because its oracle is exact -- it has to write the
// backbuffer dimensions we already know, so it cannot be accepted by accident.
// It landed at 38 against an SDK index of 5, i.e. everything is displaced by
// about +33, which puts WorldToScreenMatrix (SDK 36) near 69 rather than 36.
//
// This matters for safety, not just speed: every probe is a call into an unknown
// method, and sweeping up from index 0 means calling ServerCmd, SetViewAngles,
// LoadModel and friends before ever reaching a harmless getter. Starting from a
// good prediction keeps the number of unknown calls in single digits.
#define SDK_GETSCREENSIZE 5
#define SDK_W2S 36

static int ProjectedIndex(int m)
{
    int raw = (m == M_W2S) ? SDK_W2S
            : (m == M_GETSCREENSIZE) ? SDK_GETSCREENSIZE
            : HYP_CLIENTCMD;

    if (m != M_GETSCREENSIZE && g_methods[M_GETSCREENSIZE].resolved)
    {
        int shift = g_methods[M_GETSCREENSIZE].index - SDK_GETSCREENSIZE;
        int guess = raw + shift;
        if (guess > 0 && (g_engineVtSize <= 0 || guess < g_engineVtSize))
            return guess;
    }
    return raw;
}

// ---------------------------------------------------------------------------
// Current map, without touching the engine at all
// ---------------------------------------------------------------------------
//
// The engine rewrites momentum\demoheader.tmp on every map load. It is a
// CSVCMsg_ServerInfo protobuf, and field 16 (tag byte 0x82 0x01, length-
// delimited) is the map name -- "surf_demise" sits about 220 bytes in, right
// after field 15 which holds the game directory.
//
// This is strictly better than probing IVEngineClient::GetLevelName for it:
// zero unknown vtable calls, zero risk, and no dependence on an index that
// Strata is free to move. The name is validated against an actually-installed
// .bsp before it is believed, so a garbled read cannot poison the run store.

static long long g_headerMtime = 0;

static const char *DemoHeaderPath(void)
{
    static char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\momentum\\demoheader.tmp",
                WrGameDir());
    return path;
}

static bool ParseMapFromServerInfo(const unsigned char *buf, int len, char *out,
                                   int outLen)
{
    // Scan a bounded window for the field-16 tag, then validate what follows.
    int limit = len < 4096 ? len : 4096;
    for (int i = 0; i + 2 < limit; i++)
    {
        if (buf[i] != 0x82 || buf[i + 1] != 0x01)
            continue;
        int n = buf[i + 2];                 // names are far below the 128-byte
        if (n <= 0 || n > 63)               // varint continuation boundary
            continue;
        if (i + 3 + n > limit)
            continue;

        char name[72];
        bool printable = true;
        for (int k = 0; k < n; k++)
        {
            unsigned char c = buf[i + 3 + k];
            if (c < 0x20 || c > 0x7E)
            {
                printable = false;
                break;
            }
            name[k] = (char)c;
        }
        if (!printable)
            continue;
        name[n] = '\0';

        // The oracle: it has to name a map that is actually installed.
        char bsp[MAX_PATH];
        _snprintf_s(bsp, sizeof(bsp), _TRUNCATE, "%s\\momentum\\maps\\%s.bsp",
                    WrGameDir(), name);
        if (GetFileAttributesA(bsp) == INVALID_FILE_ATTRIBUTES)
            continue;

        strcpy_s(out, outLen, name);
        return true;
    }
    return false;
}

// Throttle: a map change does not need three-millisecond resolution, and the
// stat below is a filesystem syscall sitting in the Present path.
#define MAP_POLL_MS 250

static void RefreshMapFromDemoHeader(void)
{
    static DWORD lastPoll = 0;
    DWORD now = GetTickCount();
    if (lastPoll != 0 && (now - lastPoll) < MAP_POLL_MS)
        return;
    lastPoll = now;

    const char *path = DemoHeaderPath();

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
        return;
    long long mtime = ((long long)fad.ftLastWriteTime.dwHighDateTime << 32) |
                      fad.ftLastWriteTime.dwLowDateTime;
    if (mtime == g_headerMtime)
        return;                 // unchanged since we last looked
    g_headerMtime = mtime;

    // Shared read: the engine may still have it open.
    HANDLE h = CreateFileA(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;

    unsigned char buf[4096];
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf), &got, NULL);
    CloseHandle(h);
    if (!ok || got < 32)
        return;

    char name[72];
    if (!ParseMapFromServerInfo(buf, (int)got, name, sizeof(name)))
    {
        WrLogf("[!] demoheader.tmp changed but no installed map name in it");
        return;
    }
    if (strcmp(name, g_levelName) != 0)
    {
        strcpy_s(g_levelName, sizeof(g_levelName), name);
        WrLogf("map is now \"%s\" (from demoheader.tmp)", g_levelName);
        // The matrix almost certainly moved, and anything we were watching from
        // the previous level is stale.
        WrScanOnMapChanged();
    }
}

void WrSetMapOverride(const char *map)
{
    if (map && *map)
    {
        strcpy_s(g_mapOverride, sizeof(g_mapOverride), map);
        WrLogf("map override set to \"%s\"", g_mapOverride);
    }
    else
    {
        g_mapOverride[0] = '\0';
        WrLogf("map override cleared");
    }
}

const char *WrMapOverride(void) { return g_mapOverride; }

static void RefreshLevelName(void)
{
    if (!g_methods[M_GETLEVELNAME].resolved)
        return;
    void **vt = *(void ***)g_engineClient;
    typedef const char *(__fastcall *LevelFn_t)(void *);
    LevelFn_t fn = (LevelFn_t)vt[g_methods[M_GETLEVELNAME].index];

    const char *s = NULL;
    __try { s = fn(g_engineClient); }
    __except (EXCEPTION_EXECUTE_HANDLER) { s = NULL; }
    if (!s)
        return;

    char buf[160];
    if (WrSafeReadString(s, buf, sizeof(buf)) <= 0)
    {
        g_levelName[0] = '\0';
        return;
    }
    const char *bare = buf;
    if (_strnicmp(bare, "maps/", 5) == 0 || _strnicmp(bare, "maps\\", 5) == 0)
        bare += 5;
    char name[128];
    strcpy_s(name, sizeof(name), bare);
    size_t len = strlen(name);
    if (len > 4 && _stricmp(name + len - 4, ".bsp") == 0)
        name[len - 4] = '\0';
    if (strcmp(name, g_levelName) != 0)
    {
        strcpy_s(g_levelName, sizeof(g_levelName), name);
        WrLogf("map changed: \"%s\"", g_levelName);
    }
}

void WrEngineTryIndexFor(const char *method, int index)
{
    if (!method || index < 0)
        return;
    for (int m = 0; m < M_COUNT; m++)
    {
        if (!g_methods[m].name || _stricmp(g_methods[m].name, method) != 0)
            continue;
        WrLogf("manual index %d requested for %s", index, method);
        g_methods[m].resolved = false;
        g_gaveUp[m] = false;
        g_hintTried[m] = true;
        g_hint[m] = -1;
        g_phase = 0;
        g_wantWide = false;
        // Point the ordinary probe machinery at exactly this index. W2S still
        // needs its three confirming frames, which the tick loop will supply.
        g_w2sCandidate = -1;
        g_w2sStreak = 0;
        g_forcedIndex[m] = index;
        g_probeEnabled = true;      // typing an index is the opt-in
        return;
    }
}

void WrEngineTick(void)
{
    // Map detection is independent of everything below and must keep working
    // even if nothing else ever resolves.
    RefreshMapFromDemoHeader();

    // The safe path, and the default one: find the matrix by reading memory.
    // Needs no interface, no vtable and no calls, so it runs before anything
    // that could fail and regardless of whether engine.dll is even loaded yet.
    WrScanStart();
    WrScanTick();

    // Keep the matrix fresh. Reading it here, inside Present, means it is the
    // matrix that was used for the frame we are drawing over -- read it anywhere
    // else and the lines lag the camera by a frame.
    //
    // A probed vtable method wins if the user went out of their way to resolve
    // one, since that is unambiguously the engine's own accessor; otherwise the
    // scanned address is just as good and cost nothing to get.
    VMatrix m;
    bool got = false;
    WrMatrixSource src = WR_MAT_NONE;

    if (g_methods[M_W2S].resolved && FetchW2S(&m))
    {
        got = true;
        src = WR_MAT_VTABLE;
    }
    else if (WrScanMatrix(&m))
    {
        got = true;
        src = WR_MAT_SCAN;
    }

    g_matSource = src;
    if (got)
    {
        g_w2s = m;
        g_w2sValid = true;
        Vec3 o;
        if (WrSolveCameraOrigin(m, &o))
        {
            g_camOrigin = o;
            g_camForward = ForwardFromMatrix(m, o);
            g_camValid = true;
        }
    }
    else
    {
        g_w2sValid = false;
        g_camValid = false;
    }

    if (!AcquireInterfaces())
        return;

    RefreshLevelName();

    // Everything past here calls unknown methods on the live engine object, and
    // that is exactly what was crashing the game. It only runs when the user has
    // deliberately turned probing on in Diagnostics.
    if (!g_probeEnabled)
    {
        if (WrScanResolved())
            _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                        "matrix from memory scan; map \"%s\"",
                        g_levelName[0] ? g_levelName : "(none)");
        else
            _snprintf_s(g_status, sizeof(g_status), _TRUNCATE, "%s",
                        WrScanStatus());
        return;
    }

    // One probe per frame, in priority order. A method that has exhausted its
    // window is marked given-up and skipped, so a failure on the optional
    // GetScreenSize canary can never stop us from ever probing the one method
    // the whole feature actually needs.
    int target = -1;
    static const int kOrder[] = { M_GETSCREENSIZE, M_W2S };
    for (int i = 0; i < (int)(sizeof(kOrder) / sizeof(kOrder[0])); i++)
    {
        int m = kOrder[i];
        if (!g_methods[m].resolved && !g_gaveUp[m])
        {
            target = m;
            break;
        }
    }
    if (target < 0)
    {
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "%s; map \"%s\"",
                    g_methods[M_W2S].resolved ? "resolved"
                                              : "NOT resolved -- try Wide probe",
                    g_levelName[0] ? g_levelName : "(none)");
        return;
    }

    int hyp = ProjectedIndex(target);
    // Once anchored, search a wide band around the prediction rather than the
    // tight window used for the very first, unanchored guess.
    int window = (target != M_GETSCREENSIZE && g_methods[M_GETSCREENSIZE].resolved)
               ? ANCHORED_WINDOW : PROBE_WINDOW;

    int index;
    if (g_forcedIndex[target] >= 0)
    {
        // A number the user supplied. Keep trying just this one -- W2S needs
        // three consecutive confirming frames -- and stop if it does not pass.
        index = g_forcedIndex[target];
        if (!(target == M_W2S && g_w2sCandidate == index && g_w2sStreak > 0))
        {
            if (g_forcedTries[target]++ > 6)
            {
                WrLogf("[!] forced index %d did not validate for %s",
                       index, g_methods[target].name);
                g_forcedIndex[target] = -1;
                g_forcedTries[target] = 0;
                g_gaveUp[target] = true;
                return;
            }
        }
    }
    else if (!g_hintTried[target] && g_hint[target] >= 0)
    {
        // Try what we remembered from last session first. It still has to pass
        // the oracle; this only saves the sweep in the common case.
        index = g_hint[target];
        if (!(target == M_W2S && g_w2sCandidate == index && g_w2sStreak > 0))
            g_hintTried[target] = true;
    }
    else if (g_wantWide)
    {
        // WorldToScreenMatrix only accepts an index that validates on three
        // consecutive frames. Advancing the cursor every frame means the same
        // index is never tried twice in a row, so the streak could never reach
        // three and a wide sweep could never resolve it. Hold the cursor while a
        // candidate is mid-streak.
        if (target == M_W2S && g_w2sCandidate >= 0 && g_w2sStreak > 0)
        {
            index = g_w2sCandidate;
        }
        else if (g_wideCursor >= (g_engineVtSize > 0 ? g_engineVtSize : 128))
        {
            g_wantWide = false;
            g_wideCursor = 0;
            g_gaveUp[target] = true;
            WrLogf("[!] wide sweep covered the whole vtable without resolving %s",
                   g_methods[target].name);
            _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                        "wide sweep finished without resolving %s",
                        g_methods[target].name);
            return;
        }
        else
        {
            index = g_wideCursor++;
        }
    }
    else
    {
        if (g_phase > window * 2)
        {
            // Deliberately NOT auto-escalating to a full 0..200 sweep. That is
            // exactly what crashes: it calls every command-ish method in the
            // interface on the way up. If the anchored band missed, the honest
            // move is to stop and say so, and let the user decide whether to run
            // the explicit Wide probe or just paste an index into the ini.
            g_gaveUp[target] = true;
            g_phase = 0;
            WrLogf("[!] %s not found within +-%d of predicted index %d",
                   g_methods[target].name, window, hyp);
            WrLogf("    Set it by hand in wrlines_data\\wrlines_offsets.ini, or "
                   "use Diagnostics -> Wide probe (which may crash).");
            return;
        }
        index = WindowIndex(hyp, g_phase);
        // W2S needs several frames on the same index to confirm; don't advance
        // while a candidate is still accumulating its streak.
        if (!(target == M_W2S && g_w2sCandidate == index && g_w2sStreak > 0))
            g_phase++;
        if (index < 0 || (g_engineVtSize > 0 && index >= g_engineVtSize))
            return;     // outside the vtable; skip without burning a probe
    }

    _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                "probing %s at index %d%s", g_methods[target].name, index,
                g_wantWide ? " (wide)" : "");

    if (TryIndex(target, index))
        g_phase = 0;
}

// Static initialiser for the method table.
struct WrEngineStaticInit
{
    WrEngineStaticInit()
    {
        g_methods[M_GETSCREENSIZE].name = "GetScreenSize";
        g_methods[M_GETLEVELNAME].name = "GetLevelName";
        g_methods[M_W2S].name = "WorldToScreenMatrix";
        g_methods[M_CLIENTCMD].name = "ClientCmd";
        for (int i = 0; i < M_COUNT; i++)
        {
            g_methods[i].index = -1;
            g_hint[i] = -1;
            g_forcedIndex[i] = -1;
        }
        // Neither of these is probed, and both blank rows are intentional.
        //
        // ClientCmd exists for the in-engine capture pipeline, which is not
        // built yet. GetLevelName was dropped entirely: the map name comes from
        // demoheader.tmp instead, which costs no unknown vtable call at all.
        // Guessing an index for it was the one thing that actually broke in
        // testing -- index 26 is IsInGame in this interface, not GetLevelName.
        g_gaveUp[M_CLIENTCMD] = true;
        strcpy_s(g_methods[M_CLIENTCMD].note,
                 sizeof(g_methods[M_CLIENTCMD].note),
                 "not probed - only needed for in-engine capture");
        g_gaveUp[M_GETLEVELNAME] = true;
        strcpy_s(g_methods[M_GETLEVELNAME].note,
                 sizeof(g_methods[M_GETLEVELNAME].note),
                 "not probed - map comes from demoheader.tmp");
    }
};
static WrEngineStaticInit g_engineStaticInit;
