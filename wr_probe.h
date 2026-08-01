// wr_probe.h  --  the safe-call layer.
//
// Strata Source is closed-source. The interface version strings the shipped
// engine.dll advertises (VEngineClient015, VDebugOverlay004) are ahead of every
// public SDK, so no vtable index anywhere can be taken on faith -- they have to
// be found at runtime and proved by observation.
//
// Calling an unknown vtable slot is obviously dangerous, so every probe goes
// through here and gets four guards:
//
//   1. Code-pointer validation. The slot must point into an executable section
//      of the module the interface came from (wr_pe.h).
//
//   2. A shadow `this`. We never pass the live interface pointer. The first
//      256 bytes of the object are copied to a scratch page and the copy is
//      passed instead. Member reads still work, but if we hit a method that
//      returns a large struct by value -- MSVC x64 passes a hidden sret pointer
//      in RCX, which is exactly where `this` goes -- it scribbles over our page
//      rather than corrupting the engine's object. Without this one guard a
//      single wrong index silently trashes a vtable pointer and the game dies
//      minutes later somewhere unrelated. This is the guard that matters most.
//
//   3. Distinct scratch arguments. Eight separate zeroed slots, so a method
//      expecting (int &w, int &h) writes to two different places we can read
//      back, and one expecting a string gets a readable NUL.
//
//   4. A crash-resume breadcrumb. The (interface, index) about to be called is
//      written to disk and flushed first, then deleted on return. If it is still
//      there at startup, that index killed the game last run and is blacklisted
//      permanently. Probing can cost a restart; it cannot cost the same restart
//      twice.
//
// Plus SEH around every call and, by policy, at most one probe per frame, so a
// hang is attributable to exactly one slot.
//
// WHAT THIS DELIBERATELY DOESN'T DO
//   It cannot make an arbitrary call safe. It makes a wrong guess survivable and
//   self-diagnosing, which is a different and achievable goal.

#ifndef WR_PROBE_H
#define WR_PROBE_H

#include "wr_common.h"

#define WR_SCRATCH_SLOTS 8
#define WR_SCRATCH_SLOT_SIZE 512

bool WrProbeInit(void);

// Scratch argument slots. Zeroed by WrProbeReset before each call.
void *WrScratch(int slot);
void WrProbeReset(void);

// The shadow object for `iface`, refreshed from the live object each call.
void *WrShadow(void *iface);

// Guarded invocation. Returns false if the call raised, or if the slot did not
// look like code to begin with. `ret` receives the raw RAX value.
bool WrProbeCall(HMODULE owner, void *iface, int index, void **ret);

// Crash-resume bookkeeping.
void WrProbeBegin(const char *iface, int index);
void WrProbeEnd(void);
bool WrProbeIsBlacklisted(const char *iface, int index);
void WrProbeLoadBlacklist(void);

// Guarded memory reads, for poking at whatever a probe returned.
bool WrSafeReadBytes(const void *src, void *dst, size_t n);
bool WrSafeReadFloats(const void *src, float *dst, int count);
// Reads a NUL-terminated printable ASCII string. Returns length, or -1.
int WrSafeReadString(const void *src, char *dst, int maxLen);

#endif // WR_PROBE_H
