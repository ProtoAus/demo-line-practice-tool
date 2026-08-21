// wr_bspload.cpp  --  the worker, the handoff and the generation counter.
//
// See wr_bspload.h for why the handoff needs no lock and why the generation
// counter has to exist. The short version of both: a WrBspMap is immutable once
// built, so only freeing is dangerous and only the render thread frees; and a
// load can take 124 ms, which is long enough for the player to be somewhere
// else by the time it lands.

#include "wr_bspload.h"
#include "wr_engine.h"
#include "wr_log.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

WrBspLoadSettings g_bspLoad;

void WrBspLoadDefaults(void)
{
    g_bspLoad.read          = true;
    g_bspLoad.drawSurf      = false;
    g_bspLoad.drawRadius    = 1024.0f;
    g_bspLoad.drawAlpha     = 0.55f;
    g_bspLoad.maxDrawPolys  = 96;
    g_bspLoad.drawFill      = false;
    g_bspLoad.drawClip      = true;
    g_bspLoad.showAhead     = false;
    g_bspLoad.aheadDistance = 2048.0f;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static CRITICAL_SECTION g_cs;
static bool g_csReady = false;

static void EnsureCs(void)
{
    if (!g_csReady)
    {
        InitializeCriticalSection(&g_cs);
        g_csReady = true;
    }
}

// Guarded by g_cs: the name the worker should load, and the status the panel
// reads. Small, copied in and out, never held across anything slow.
static char  g_wanted[72] = { 0 };
static char  g_loaded[72] = { 0 };
static char  g_err[192] = { 0 };
static float g_millis = 0.0f;
static int   g_state = WR_BSPLOAD_IDLE;

// Bumped by the render thread every time the wanted map changes, and read by
// the worker when it finishes. See the header: this is what stops one level's
// geometry being published into another level.
static volatile LONG g_generation = 0;

// One worker at a time. A second map change while one is in flight does not
// start a second thread; it bumps the generation, the running one throws its
// result away, and WrBspLoadTick starts a fresh one the frame after it exits.
static volatile LONG g_busy = 0;
static HANDLE g_thread = NULL;

// What crosses between the two threads. The generation TRAVELS WITH THE MAP,
// and that is not tidiness -- it is the fix for a race the worker's own check
// cannot close.
//
// The worker tests the generation and then publishes, and those are two
// separate operations. A map change landing between them passes the test and
// then publishes into the new level anyway, and the render thread has no way to
// tell that pointer from a good one. Carrying the generation across means the
// SIDE THAT INSTALLS gets to decide, and that side is the only one that ever
// bumps the counter -- so its comparison cannot go stale while it is being made.
//
// The worker's own check stays. It is now an optimisation rather than the
// guarantee: it saves building a 2 MB result into a slot that would only be
// freed again, which is the common case when somebody is cycling maps.
struct WrBspPending
{
    WrBspMap map;
    LONG     gen;
};

// The handoff slot. Written only by the worker, taken only by the render
// thread, both through InterlockedExchangePointer.
static void * volatile g_next = NULL;

// Owned exclusively by the render thread. Never touched by the worker, which is
// the entire reason WrBspLoadCurrent can be called from a draw path with no
// lock and no reference count. Still a WrBspPending rather than a bare map so
// that the allocation the worker made is the allocation that gets freed.
static WrBspPending *g_current = NULL;

// Whether g_bspLoad.read was on last tick, so the transition can be noticed.
static bool g_wasReading = true;

// ---------------------------------------------------------------------------
// The worker
// ---------------------------------------------------------------------------

static DWORD WINAPI LoadThread(LPVOID)
{
    // Same reasoning as wr_extract.cpp's CountThread: this runs on a map
    // change, which is exactly when the game is loading, streaming and
    // spawning. Get out of its way -- nothing here is wanted this instant.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    LONG gen = g_generation;

    char map[72];
    EnterCriticalSection(&g_cs);
    strcpy_s(map, sizeof(map), g_wanted);
    LeaveCriticalSection(&g_cs);

    char err[192] = { 0 };
    int state = WR_BSPLOAD_MISSING;
    WrBspPending *built = NULL;
    float ms = 0.0f;

    if (map[0] && WrGameDir()[0])
    {
        char path[MAX_PATH];
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\momentum\\maps\\%s.bsp",
                    WrGameDir(), map);

        LARGE_INTEGER freq, t0, t1;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);

        WrBspRaw raw;
        if (!WrBspReadRaw(path, &raw, err, (int)sizeof(err)))
        {
            // "there is no such file" is a different thing from "this reader
            // will not read it", and the panel says different things about
            // them. A level whose .bsp was never in momentum\maps is ordinary
            // and gets a flat sentence; a file that is there and refused is
            // worth reporting, and its refusal names the lump.
            //
            // This used to test for "no map file named", which WrBspReadRaw only
            // ever says when it is handed an EMPTY PATH -- and the path here is
            // built from two non-empty strings, so it never was. A missing map
            // took the other branch and was reported in warning orange as
            // "Would not read it: could not open the map file", i.e. as a bug in
            // this reader rather than as a map you do not have. The whole
            // WR_BSPLOAD_MISSING arm was unreachable.
            state = (strstr(err, "could not open the map file") != NULL ||
                     strstr(err, "no map file named") != NULL)
                        ? WR_BSPLOAD_MISSING : WR_BSPLOAD_REFUSED;
        }
        else
        {
            WrBspMap m;
            if (!WrBspBuild(&raw, &m, err, (int)sizeof(err)))
            {
                state = WR_BSPLOAD_REFUSED;
            }
            else
            {
                built = (WrBspPending *)malloc(sizeof(WrBspPending));
                if (built)
                {
                    built->map = m;
                    built->gen = gen;
                    state = WR_BSPLOAD_READY;
                }
                else
                {
                    // 200 bytes. If this fails the machine is in no state to
                    // draw anything anyway, but the map still has to be freed
                    // or the 2 MB behind it leaks on every map change.
                    WrBspFreeMap(&m);
                    strcpy_s(err, sizeof(err), "out of memory");
                    state = WR_BSPLOAD_REFUSED;
                }
            }
            WrBspFreeRaw(&raw);
        }

        QueryPerformanceCounter(&t1);
        ms = (float)(1000.0 * (double)(t1.QuadPart - t0.QuadPart)
                     / (double)freq.QuadPart);
    }

    // The early out. If the level changed while this was parsing, everything
    // above describes somewhere the player is not, and there is no point
    // handing it over. This is NOT what makes the handoff safe -- a map change
    // landing between this test and the publish below would slip past it. What
    // makes it safe is that built->gen travels with the map and the render
    // thread checks it before installing. See WrBspPending.
    if (g_generation != gen)
    {
        if (built)
        {
            WrBspFreeMap(&built->map);
            free(built);
        }
        InterlockedExchange(&g_busy, 0);
        return 0;
    }

    EnterCriticalSection(&g_cs);
    strcpy_s(g_loaded, sizeof(g_loaded), map);
    strcpy_s(g_err, sizeof(g_err), err);
    g_millis = ms;
    g_state = state;
    LeaveCriticalSection(&g_cs);

    if (built)
    {
        // Anything already sitting in the slot is a result the render thread
        // never got to -- which can only happen if two loads completed between
        // two frames. Free it here rather than leaking it; this thread put it
        // there and no one else has seen it.
        void *stale = InterlockedExchangePointer(&g_next, built);
        if (stale)
        {
            WrBspFreeMap(&((WrBspPending *)stale)->map);
            free(stale);
        }

        WrLogf("bsp: %s -- %d polys, %d in the surf band, %.2f MB, %.0f ms",
               map, built->map.polyCount, built->map.surfPolys,
               (double)built->map.bytes / 1048576.0, ms);
    }
    else if (state == WR_BSPLOAD_REFUSED)
    {
        WrLogf("bsp: %s -- %s", map, err);
    }

    InterlockedExchange(&g_busy, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

// Frees whatever the render thread is holding. Render thread only.
static void DropCurrent(void)
{
    if (g_current)
    {
        WrBspFreeMap(&g_current->map);
        free(g_current);
        g_current = NULL;
    }
}

// Frees anything a worker published that has not been taken yet. Render thread
// only, and only where a result is known to be unwanted.
static void DropPending(void)
{
    void *stale = InterlockedExchangePointer(&g_next, NULL);
    if (stale)
    {
        WrBspFreeMap(&((WrBspPending *)stale)->map);
        free(stale);
    }
}

void WrBspLoadOnMapChanged(const char *map)
{
    EnsureCs();

    EnterCriticalSection(&g_cs);
    strcpy_s(g_wanted, sizeof(g_wanted), map ? map : "");
    g_loaded[0] = 0;
    g_err[0] = 0;
    g_millis = 0.0f;
    g_state = g_bspLoad.read ? WR_BSPLOAD_WORKING : WR_BSPLOAD_OFF;
    LeaveCriticalSection(&g_cs);

    // Bumped AFTER the name is stored, so a worker that reads the generation
    // and then the name cannot see the new generation with the old name.
    InterlockedIncrement(&g_generation);

    // Immediately, not when the replacement lands. This is called from
    // WrIdleTick on the render thread, so it is safe here -- and it has to
    // happen here, because the alternative is up to 124 ms of drawing the
    // previous level's ramps into this one.
    DropCurrent();
    DropPending();
}

void WrBspLoadTick(void)
{
    EnsureCs();

    if (!g_bspLoad.read)
    {
        if (g_wasReading)
        {
            // Turning it off is a real off: the resident map goes now rather
            // than at the next map change, and any worker in flight is told
            // its result is unwanted by the same mechanism a map change uses.
            InterlockedIncrement(&g_generation);
            DropCurrent();
            DropPending();
            EnterCriticalSection(&g_cs);
            g_loaded[0] = 0;
            g_err[0] = 0;
            g_state = WR_BSPLOAD_OFF;
            LeaveCriticalSection(&g_cs);
            g_wasReading = false;
        }
        return;
    }
    if (!g_wasReading)
    {
        // Turning it back on re-asks for whatever map is current. The state
        // goes to WORKING here rather than in the branch below so that a frame
        // where the worker has not started yet does not read as IDLE.
        g_wasReading = true;
        EnterCriticalSection(&g_cs);
        if (g_wanted[0])
            g_state = WR_BSPLOAD_WORKING;
        LeaveCriticalSection(&g_cs);
    }

    // Take a finished load, if there is one -- and check that it is for the
    // level currently on screen before installing it.
    //
    // THIS IS THE CHECK THAT MAKES THE HANDOFF SAFE, not the one in the worker.
    // This thread is the only one that ever bumps the generation, so nothing
    // can move it while this comparison is being made. The worker's version of
    // the same test can be overtaken between testing and publishing; this one
    // cannot be overtaken by anything.
    void *fresh = InterlockedExchangePointer(&g_next, NULL);
    if (fresh)
    {
        WrBspPending *p = (WrBspPending *)fresh;
        if (p->gen == g_generation)
        {
            DropCurrent();
            g_current = p;
        }
        else
        {
            WrBspFreeMap(&p->map);
            free(p);
        }
    }

    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0)
        return;                 // one already running

    // Is one wanted? Only when there is a map name and nothing loaded for it.
    bool start = false;
    EnterCriticalSection(&g_cs);
    if (g_wanted[0] && strcmp(g_wanted, g_loaded) != 0)
        start = true;
    LeaveCriticalSection(&g_cs);

    if (!start)
    {
        InterlockedExchange(&g_busy, 0);
        return;
    }

    if (g_thread)
    {
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    g_thread = CreateThread(NULL, 0, LoadThread, NULL, 0, NULL);
    if (!g_thread)
    {
        InterlockedExchange(&g_busy, 0);
        EnterCriticalSection(&g_cs);
        strcpy_s(g_err, sizeof(g_err), "could not start the reader thread");
        g_state = WR_BSPLOAD_REFUSED;
        // So it is not retried every frame for ever.
        strcpy_s(g_loaded, sizeof(g_loaded), g_wanted);
        LeaveCriticalSection(&g_cs);
    }
}

const WrBspMap *WrBspLoadCurrent(void)
{
    return g_current ? &g_current->map : NULL;
}

int WrBspLoadStateNow(void)
{
    if (!g_bspLoad.read)
        return WR_BSPLOAD_OFF;
    if (g_current)
        return WR_BSPLOAD_READY;
    if (!g_csReady)
        return WR_BSPLOAD_IDLE;
    EnterCriticalSection(&g_cs);
    int s = g_state;
    // READY with nothing resident happens for one frame between the worker
    // publishing and the tick taking it. Reporting READY there would have the
    // panel print a coverage line for a map it cannot see.
    if (s == WR_BSPLOAD_READY)
        s = WR_BSPLOAD_WORKING;
    LeaveCriticalSection(&g_cs);
    return s;
}

const char *WrBspLoadError(void)
{
    static char copy[192];
    if (!g_csReady)
        return "";
    EnterCriticalSection(&g_cs);
    strcpy_s(copy, sizeof(copy), g_err);
    LeaveCriticalSection(&g_cs);
    return copy;
}

float WrBspLoadMillis(void)
{
    if (!g_csReady)
        return 0.0f;
    EnterCriticalSection(&g_cs);
    float ms = g_millis;
    LeaveCriticalSection(&g_cs);
    return ms;
}

const char *WrBspLoadMapName(void)
{
    static char copy[72];
    if (!g_csReady)
        return "";
    EnterCriticalSection(&g_cs);
    strcpy_s(copy, sizeof(copy), g_loaded);
    LeaveCriticalSection(&g_cs);
    return copy;
}

// ---------------------------------------------------------------------------
// Coverage
// ---------------------------------------------------------------------------

// WR_BSP_THIN_OWNED and both predicates now live in wr_bsp.h, taking a WrBspMap
// rather than reaching for the loaded one. They are properties of a map, and the
// switch this reader's whole live behaviour turns on had no test coverage in any
// harness while it was trapped in here behind a loader lookup. These two remain
// because every caller in the panel wants the current map and should not have to
// say so.
bool WrBspLoadCoverageThin(void)
{
    return WrBspCoverageThin(WrBspLoadCurrent());
}

bool WrBspLoadCoverage(char *line, int cap)
{
    if (!line || cap <= 0)
        return false;
    line[0] = 0;

    const WrBspMap *m = WrBspLoadCurrent();
    if (!m)
    {
        switch (WrBspLoadStateNow())
        {
        case WR_BSPLOAD_OFF:
            strcpy_s(line, cap, "Not reading the map file.");
            break;
        case WR_BSPLOAD_WORKING:
            strcpy_s(line, cap, "Reading the map file...");
            break;
        case WR_BSPLOAD_MISSING:
            strcpy_s(line, cap, "No .bsp for this level in momentum\\maps.");
            break;
        case WR_BSPLOAD_REFUSED:
            _snprintf_s(line, cap, _TRUNCATE, "Would not read it: %s",
                        WrBspLoadError());
            break;
        default:
            strcpy_s(line, cap, "No level loaded.");
            break;
        }
        return false;
    }

    const int haveBrushes = m->brushWorld + m->entBrushes;
    float pct = m->brushTotal ? 100.0f * (float)haveBrushes
                              / (float)m->brushTotal : 0.0f;

    // The clip share is spelled out rather than folded in. It used to be folded
    // in, and on a map like surf_ethereal -- where two thirds of the world
    // brushes are clip-only -- that meant the sentence said several hundred were
    // in the surf band while the drawing query was refusing almost all of them.
    char clip[64];
    clip[0] = '\0';
    if (m->surfClipPolys > 0)
        _snprintf_s(clip, sizeof(clip), _TRUNCATE,
                    " (%d of those are clip brushes)", m->surfClipPolys);

    // "4 displacements skipped" is what this used to say on surf_kvas, and it
    // reads as 99.995% complete while meaning the exact opposite: those 4 were
    // the switch that turned the live map query off for the whole level. Say how
    // many of how many, and say that what is missing is missing from every
    // number in this same sentence.
    char disp[160];
    disp[0] = '\0';
    if (m->dispPolys > 0 || m->dispDropped > 0)
        _snprintf_s(disp, sizeof(disp), _TRUNCATE,
                    " %d displacement triangles; %d of %d displacements were "
                    "not built at all and are missing from every number above.",
                    m->dispPolys, m->dispDropped, m->dispTotal);

    _snprintf_s(line, cap, _TRUNCATE,
                "%d of %d brushes read (%.0f%%): %d the world's and %d from %d "
                "solid entities, %d of them solid; %d faces, %d in the surf "
                "band%s.%s",
                haveBrushes, m->brushTotal, pct, m->brushWorld, m->entBrushes,
                m->entModels, m->brushSolid, m->polyCount, m->surfPolys,
                clip, disp);
    return true;
}

float WrBspLoadNearestEx(const Vec3 &feet, float radius, float *outNormal,
                         float *outRampPlane, float *outRampDist)
{
    if (outRampDist) *outRampDist = -1.0f;

    const WrBspMap *m = WrBspLoadCurrent();
    if (!m || m->polyCount <= 0)
        return -1.0f;
    if (!(radius > 0.0f))
        radius = WR_BSP_TOUCH_RADIUS;

    const float p[3] = { feet.x, feet.y, feet.z };
    int poly = -1, ramp = -1;
    float dist = 0.0f, rampDist = -1.0f;
    if (!WrBspNearestFaceEx(m, p, radius, &poly, &dist, &ramp, &rampDist))
        return -1.0f;

    if (outNormal && poly >= 0 && poly < m->polyCount)
    {
        const float *pl = m->polys[poly].plane;
        outNormal[0] = pl[0];
        outNormal[1] = pl[1];
        outNormal[2] = pl[2];
    }

    // All four floats of the ramp's plane, not three. The fourth is what lets
    // the board solve WHEN the surface was crossed instead of guessing an
    // instant -- see WrEnergyTickBoards. It is free here and unavailable later.
    if (outRampPlane && ramp >= 0 && ramp < m->polyCount)
    {
        const float *pl = m->polys[ramp].plane;
        outRampPlane[0] = pl[0];
        outRampPlane[1] = pl[1];
        outRampPlane[2] = pl[2];
        outRampPlane[3] = pl[3];
    }
    if (outRampDist && ramp >= 0)
        *outRampDist = rampDist;

    return dist;
}

float WrBspLoadNearest(const Vec3 &feet, float radius, float *outNormal)
{
    return WrBspLoadNearestEx(feet, radius, outNormal, 0, 0);
}

bool WrBspLoadGeometryComplete(void)
{
    return WrBspGeometryComplete(WrBspLoadCurrent());
}

// Does this map use displacements, which are not read at all?
//
// Beside WrBspLoadCoverageThin rather than inside the sentence above, because it
// is a different KIND of gap and the panel should be able to colour it. Every
// number in that sentence is about brushes that were counted; a displacement was
// never counted, so no percentage there can move when a map is full of them.
//
// Over half the maps in the library have some and 51 are built almost entirely
// out of them, where this reader draws nearly nothing -- which on screen is
// indistinguishable from a map with no ramps in it. That has to be said rather
// than discovered. It is one directory entry's length: WrBspRaw::dispInfoBytes.
bool WrBspLoadHasDisplacements(void)
{
    const WrBspMap *m = WrBspLoadCurrent();
    return m && m->hasDisplacements;
}

bool WrBspLoadDisplacementsMissing(void)
{
    // Having displacements is no longer a problem; having ones that could not
    // be built still is. Both causes land here: a displacement lump whose
    // version has no row in the stride table -- which no longer includes v25,
    // and is now a statement about the next bump rather than about any map in
    // this library -- and a map dense enough to hit the build budget.
    const WrBspMap *m = WrBspLoadCurrent();
    if (!m || !m->hasDisplacements)
        return false;
    return m->dispPolys <= 0 || m->dispDropped > 0;
}

// ---------------------------------------------------------------------------
// The queries, wrapped
// ---------------------------------------------------------------------------

bool WrBspLoadAhead(const Vec3 &from, const Vec3 &dir, float maxDist,
                    float *angleDeg, float *distOut, bool *inBandOut)
{
    const WrBspMap *m = WrBspLoadCurrent();
    if (!m || m->polyCount <= 0)
        return false;

    const float s[3] = { from.x, from.y, from.z };
    const float d[3] = { dir.x, dir.y, dir.z };

    int poly = -1;
    float t = 0.0f;
    if (!WrBspTraceRay(m, s, d, maxDist, &poly, &t))
        return false;
    if (poly < 0 || poly >= m->polyCount)
        return false;

    const float *p = m->polys[poly].plane;
    if (angleDeg)  *angleDeg = WrBspSurfaceAngle(p);
    if (distOut)   *distOut = t;
    if (inBandOut) *inBandOut = WrBspIsSurfBand(p[2]);
    return true;
}

