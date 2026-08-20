// wr_bspload.h  --  when the map file is read, on which thread, and who owns it.
//
// wr_bsp.cpp is a pure reader: hand it a path, get back polygons or a refusal,
// and it knows nothing about maps changing or threads or settings. That is
// deliberate and it is what lets tests\test_bsp.exe and tests\bsp_sweep.exe
// link it with nothing else attached. This file is the other half -- the part
// that knows there is a game running.
//
// WHY A WORKER THREAD AND NOT THE JOB POOL
//
// WrJobsRunAll BLOCKS THE CALLING THREAD until its work is done. That is right
// for extraction, which is started from a button and is allowed to take a
// second. It is exactly wrong here: the only caller with a map name in its hand
// is WrIdleTick, which runs inside Present, and blocking there stops the game's
// frame. So this is one plain CreateThread at below-normal priority, the same
// shape as wr_extract.cpp's CountThread and for the same reason -- it fires on
// a map change, which is the busiest moment the game has.
//
// WHAT IT COSTS, MEASURED
//
// One load is read + decompress + walk + clip + grid. Over all 1,304 maps
// (tests\bsp_sweep.exe --closure prints it):
//
//     p50 16 ms   p90 41 ms   p99 52 ms   worst 124 ms (surf_ispy)
//
// on a background thread, once, when the map changes. The resident result is
// p50 2.25 MB and 10.50 MB at the very worst. Both are small enough that this
// does not need to be clever, and the honest reason the loading is asynchronous
// at all is not the 16 ms -- it is the 124. At 200 fps that is twenty-five
// frames, and a hitch on a map change is exactly where a player would blame
// the game.
//
// THE HANDOFF, AND WHY IT NEEDS NO LOCK
//
// A WrBspMap is IMMUTABLE once WrBspBuild returns. Nothing in this file or any
// other ever writes into one after that point, so the only dangerous operation
// is freeing it, and freeing happens on exactly one thread.
//
//     the worker    builds into a heap block, then publishes the pointer with
//                   InterlockedExchangePointer into g_next. It never reads
//                   g_current and never frees anything it published.
//     the renderer  takes g_next in WrBspLoadTick, frees whatever it was
//                   holding, and installs the new one. It is the only reader of
//                   g_current and the only caller of WrBspFreeMap.
//
// So there is one writer of the handoff slot, one reader, and a single
// interlocked pointer exchange between them, which is a full barrier on both
// sides. WrBspLoadCurrent() may therefore be called from the draw path every
// frame with no lock at all -- but only from the render thread, and only for
// the duration of that frame. That restriction is the whole safety argument and
// it is repeated on the function.
//
// THE GENERATION COUNTER EXISTS BECAUSE A LOAD CAN FINISH INTO THE WRONG MAP
//
// Loads take up to 124 ms and a player can change map, spawn, and change again
// inside that. Without a check the worker would publish surf_a's geometry while
// surf_b is on screen -- and the failure mode is the bad one for this feature:
// not an error, but ramps drawn in midair somewhere plausible.
//
// THE CHECK IS ON THE TAKING SIDE, and that detail is the whole of it. Having
// the worker test the generation before publishing is not enough: the test and
// the publish are two operations, and a map change landing between them sails
// straight through. So the generation travels WITH the map, and the render
// thread -- which is the only thread that ever bumps the counter, so nothing
// can move it mid-comparison -- decides whether to install or to free. The
// worker keeps its own test as an optimisation, to avoid handing over a result
// it can already see is unwanted.
//
// NOTHING IS SHUT DOWN, ON PURPOSE
//
// This DLL has no unload path -- see dllmain.cpp. A worker still parsing when
// the process exits is terminated by ExitProcess with its allocations still
// held, and that is fine: the process is going away. There is deliberately no
// wait-for-worker here, because the only place it could run is
// DLL_PROCESS_DETACH, under the loader lock, after every other thread is
// already dead.

#ifndef WR_BSPLOAD_H
#define WR_BSPLOAD_H

#include "wr_common.h"
#include "wr_bsp.h"

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

struct WrBspLoadSettings
{
    // Read the map file at all. On by default, and it is worth saying why,
    // because this tool's rule is that everything reaching outside itself is
    // opt-in: this reaches for a file in the game's own install, read-only,
    // shared, in the same way and from the same directory the demo reader
    // already does. It makes no call into the game, sets no cvar and runs no
    // command. What it costs is one background thread for a few tens of
    // milliseconds and a couple of megabytes, once per map.
    //
    // Turning it off frees the resident map immediately and makes every query
    // below answer "nothing", which is the same answer they give on a map that
    // could not be read. Both say so in the panel rather than looking empty.
    bool read;

    // Draw the surf-band faces near you. OFF by default, like the pick plate
    // and the corner block, because it puts something on screen that was not
    // there before and the first thing anybody wants to know is what it is.
    bool drawSurf;

    // How far from the camera to look, and how far from that the faces fade
    // out. The default 1024 is about a second and a half of travel at surf
    // speed, which is roughly as far ahead as it is useful to be told about.
    float drawRadius;
    float drawAlpha;
    int   maxDrawPolys;

    // Outline only, or a translucent fill as well. Outline is the default: a
    // filled ramp face hides the demo line lying on it, which is the thing
    // the whole tool exists to show.
    bool drawFill;

    // The "what is ahead" readout: trace where the camera is looking and say
    // what angle the first surface is at. A row on the corner block, so it
    // follows that block's own toggle.
    bool showAhead;
    float aheadDistance;
};

extern WrBspLoadSettings g_bspLoad;

void WrBspLoadDefaults(void);

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------

// Called from WrIdleTick's map-change block. Cheap: it records the name and
// bumps the generation. The thread is started by WrBspLoadTick.
void WrBspLoadOnMapChanged(const char *map);

// Called once per frame from WrIdleTick, on the render thread. Picks up a
// finished load, frees the previous map, and starts the next worker if one is
// wanted and none is running.
void WrBspLoadTick(void);

// The map for the level currently on screen, or NULL.
//
// RENDER THREAD ONLY, AND ONLY FOR THIS FRAME. The pointer is stable until the
// next WrBspLoadTick and there is no reference counting: holding it across a
// frame boundary is a use-after-free the moment the player changes level. Every
// caller in this codebase asks for it, uses it, and drops it inside one
// function.
const WrBspMap *WrBspLoadCurrent(void);

enum WrBspLoadState
{
    WR_BSPLOAD_OFF = 0,     // the setting is off
    WR_BSPLOAD_IDLE,        // nothing asked for yet -- no map
    WR_BSPLOAD_WORKING,     // a worker is parsing
    WR_BSPLOAD_READY,       // WrBspLoadCurrent() has something
    WR_BSPLOAD_MISSING,     // no such file, which is normal for a workshop map
    WR_BSPLOAD_REFUSED      // the file was there and this reader would not read it
};

int WrBspLoadStateNow(void);

// The refusal, verbatim from wr_bsp.cpp, or "" when there was none. These are
// written to be read by a person -- "LEAFS lump version 3 on a v25 map is not
// one this reads" is a thing somebody can report; "failed" is not.
const char *WrBspLoadError(void);

// What the last load cost, in milliseconds, and which map it was for.
float WrBspLoadMillis(void);
const char *WrBspLoadMapName(void);

// ---------------------------------------------------------------------------
// Coverage -- the half of the answer that is about what was NOT read
// ---------------------------------------------------------------------------
//
// Measured over the library, model 0 owns 84.1% of brushes; on bhop_slope_v2 it
// owns 3.6%, because that map is built almost entirely out of entities. And 51
// maps are displacement-dominated, where this reads almost nothing at all.
//
// On any of those the geometry layer is not wrong, it is ABSENT -- and absent
// looks exactly like "there is no ramp ahead of you", which is the one thing it
// must never be mistaken for. So the panel gets a sentence and it comes from
// here rather than from the caller doing arithmetic on WrBspMap's counters.
//
// Fills `line` with something like "read 1,204 of 1,392 brushes (86%)" plus a
// warning clause when world ownership is low. Returns false when there is
// nothing loaded, in which case `line` explains that instead.
bool WrBspLoadCoverage(char *line, int cap);

// True when what was read is thin enough that the panel should say so in
// warning colour rather than in passing.
bool WrBspLoadCoverageThin(void);

// ---------------------------------------------------------------------------
// The queries, wrapped
// ---------------------------------------------------------------------------
//
// Thin wrappers that answer "no" instead of crashing when nothing is loaded,
// so the draw path does not have to null-check before every question.

// What the camera is looking at: the angle of the first surface along `dir`,
// and how far away it is. False when nothing is loaded or nothing is ahead.
bool WrBspLoadAhead(const Vec3 &from, const Vec3 &dir, float maxDist,
                    float *angleDeg, float *distOut, bool *inBandOut);

#endif // WR_BSPLOAD_H
