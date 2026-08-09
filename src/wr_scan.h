// wr_scan.h  --  finding the world->screen matrix without calling anything.
//
// The vtable probe approach worked, right up until it didn't. Calling unknown
// methods on a live engine object crashed the game asynchronously -- the call
// returned fine and the process died a moment later somewhere unrelated. That is
// the one failure mode the crash-resume blacklist cannot catch, because there is
// nothing to blame by the time it happens. No probe window is safe against it.
//
// So: don't call anything. The matrix we want is a specific arrangement of 16
// floats sitting in the game's own writable memory, updated every frame. Read
// memory instead of executing it.
//
//   * Scanning is read-only. It cannot corrupt engine state, because it never
//     writes and never transfers control. Worst case is finding nothing.
//   * Every read goes through ReadProcessMemory on our own process, which
//     returns false on an unmapped page rather than raising, so a region being
//     freed underneath the scan is a non-event.
//   * The oracle is the same closed-loop test the probe used, and it is strong:
//     a camera origin inside world bounds AND a point 512 units down the view
//     axis landing near screen centre AND holding up over consecutive frames.
//     Random floats do not do that.
//
// Anything that passes is a real world->clip matrix, wherever it lives -- the
// engine's renderer, the client's view setup, a material-system copy. They are
// all equally usable, so we take whichever one updates most reliably.

#ifndef WR_SCAN_H
#define WR_SCAN_H

#include "wr_common.h"

// Kick off the background scan. Safe to call every frame; only the first call
// does anything. Needs the backbuffer size to be known, so call it from inside
// the Present hook rather than at DllMain time.
void WrScanStart(void);

// Per-frame, on the render thread: re-validate the candidates and keep the
// winner's matrix fresh. Must run inside Present so the matrix we read is the
// one the frame we are drawing over was rendered with.
void WrScanTick(void);

// Throw away everything and scan again. Used when the resolved address goes
// stale (a level change can move a heap-resident matrix).
//
// Never blocks: it asks the scan thread to stop and the next WrScanTick starts
// the new one once the old has actually exited. It used to WaitForSingleObject
// for up to three seconds, from inside Present, which is a three-second freeze
// on the render thread at the worst possible moment.
void WrScanRestart(void);

// A level load invalidates most of what we were watching. Call on map change.
void WrScanOnMapChanged(void);

bool WrScanResolved(void);
bool WrScanMatrix(VMatrix *out);     // last validated matrix

// Diagnostics
const char *WrScanStatus(void);
const char *WrScanNote(void);        // what the oracle saw when it accepted
const void *WrScanAddress(void);
bool WrScanTransposed(void);
bool WrScanBusy(void);
int WrScanCandidateCount(void);
int WrScanLiveCandidateCount(void);
int WrScanUpdatingCount(void);   // candidates seen changing frame to frame
float WrScanTravel(void);        // units the chosen camera has moved
double WrScanMegabytes(void);

// How long the chosen matrix has been byte-identical, and the threshold past
// which we conclude the world is not being rendered at all -- disconnecting to
// the main menu leaves the last in-game camera sitting in memory, valid and
// motionless, and drawing a route through it puts the whole map over the menu.
float WrScanFrozenSeconds(void);
float *WrScanFrozenLimit(void);  // settable from the UI

// True while the freeze cutoff is being deliberately ignored because the panel
// is open -- otherwise opening the panel to tick runs on and off would make the
// lines you are ticking vanish, since using the panel means standing still.
bool WrScanHoldingForPanel(void);

// ---------------------------------------------------------------------------
// Choosing between candidates by hand
// ---------------------------------------------------------------------------
//
// More than one matrix in a Source frame is a genuine world->clip matrix for
// this viewport. The main view is one; any pass that renders from the player's
// own eye with a different field of view is another, and it passes every test
// here -- orthonormal basis, right aspect ratio, camera inside the world,
// reprojects to screen centre, changes every frame, follows the player. The
// only difference is the FOV, and the symptom of picking the wrong one is
// subtle: lines that track the world correctly at the crosshair and drift
// further out towards the edges of the screen.
//
// No heuristic here can tell those apart, so rather than guess harder, show
// them and let the answer be one click -- and remember it.
struct WrScanCandidateInfo
{
    const void *addr;
    bool transposed;
    bool alive;
    bool chosen;
    bool pinned;
    int hits;
    int changes;
    int jumps;
    float travel;
    float fov;          // horizontal, degrees
    float aspect;
};

bool WrScanCandidateAt(int i, WrScanCandidateInfo *out);
void WrScanUseCandidate(int i);      // pick this one by hand, right now

// Remember the chosen address as module+offset so the next launch adopts it
// immediately instead of re-deriving it (and possibly landing on a different
// one). Returns false if it does not live inside a loaded module, in which case
// its address is different every launch and there is nothing to remember.
bool WrScanPinChosen(void);
void WrScanForgetPin(void);
const char *WrScanPinDescription(void);   // "engine.dll+0x1a2b3c4", or ""

#endif // WR_SCAN_H
