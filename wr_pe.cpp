// wr_pe.cpp  --  see wr_pe.h.

#include "wr_pe.h"
#include "wr_log.h"

#include <string.h>

#define WR_MAX_MODULES 16
#define WR_MAX_SECTIONS 24

struct WrModuleInfo
{
    HMODULE mod;
    int sectionCount;
    struct { uintptr_t lo, hi; } exec[WR_MAX_SECTIONS];
};

static WrModuleInfo g_mods[WR_MAX_MODULES];
static int g_modCount = 0;

static WrModuleInfo *Find(HMODULE mod)
{
    for (int i = 0; i < g_modCount; i++)
        if (g_mods[i].mod == mod)
            return &g_mods[i];
    return NULL;
}

bool WrPeRegister(HMODULE mod)
{
    if (!mod)
        return false;
    if (Find(mod))
        return true;
    if (g_modCount >= WR_MAX_MODULES)
        return false;

    uintptr_t base = (uintptr_t)mod;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    if (IsBadReadPtr(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (IsBadReadPtr(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    WrModuleInfo *mi = &g_mods[g_modCount];
    memset(mi, 0, sizeof(*mi));
    mi->mod = mod;

    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    int n = nt->FileHeader.NumberOfSections;
    for (int i = 0; i < n && mi->sectionCount < WR_MAX_SECTIONS; i++)
    {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        DWORD size = sec[i].Misc.VirtualSize;
        if (size == 0)
            size = sec[i].SizeOfRawData;
        mi->exec[mi->sectionCount].lo = base + sec[i].VirtualAddress;
        mi->exec[mi->sectionCount].hi = base + sec[i].VirtualAddress + size;
        mi->sectionCount++;
    }

    g_modCount++;

    char name[MAX_PATH] = {0};
    GetModuleFileNameA(mod, name, MAX_PATH);
    const char *leaf = strrchr(name, '\\');
    WrLogf("pe: %s base=%p exec sections=%d",
           leaf ? leaf + 1 : name, (void *)base, mi->sectionCount);
    return true;
}

bool WrIsCodeIn(HMODULE mod, const void *ptr)
{
    WrModuleInfo *mi = Find(mod);
    if (!mi || !ptr)
        return false;
    uintptr_t p = (uintptr_t)ptr;
    for (int i = 0; i < mi->sectionCount; i++)
        if (p >= mi->exec[i].lo && p < mi->exec[i].hi)
            return true;
    return false;
}

int WrVTableLength(HMODULE mod, void **vtable, int maxProbe)
{
    if (!vtable || IsBadReadPtr(vtable, sizeof(void *)))
        return 0;
    int i = 0;
    for (; i < maxProbe; i++)
    {
        if (IsBadReadPtr(&vtable[i], sizeof(void *)))
            break;
        if (!WrIsCodeIn(mod, vtable[i]))
            break;
    }
    return i;
}
