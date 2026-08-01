// wr_pe.h  --  minimal PE inspection, used to sanity-check function pointers.
//
// Before we call a vtable slot we have not proven anything about, we at least
// want to know it points at executable code inside the module the interface came
// from. That one check removes almost every way a bad index can go wrong.

#ifndef WR_PE_H
#define WR_PE_H

#include "wr_common.h"

// Records the executable sections of a module so WrIsCodeIn can be a couple of
// comparisons. Safe to call repeatedly; results are cached.
bool WrPeRegister(HMODULE mod);

// True if ptr lands inside an executable section of that specific module.
bool WrIsCodeIn(HMODULE mod, const void *ptr);

// Walk a vtable until an entry stops looking like code in the owning module.
// The length is a useful fingerprint: if it changes after a game update, the
// interface itself changed and every cached index should be distrusted.
int WrVTableLength(HMODULE mod, void **vtable, int maxProbe);

#endif // WR_PE_H
