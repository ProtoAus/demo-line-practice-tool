// wr_probe.cpp  --  see wr_probe.h.
//
// This file is compiled with SEH in use and deliberately contains no C++ objects
// with destructors, so /EHsc is fine and C2712 ("cannot use __try in functions
// requiring object unwinding") can never fire. If it ever does, the fix is to
// move code OUT of this file -- not to switch the whole build to /EHa, which
// would slow ImGui down for no reason.

#include "wr_probe.h"
#include "wr_pe.h"
#include "wr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>

#define WR_SHADOW_BYTES 256
#define WR_MAX_BLACKLIST 64

// One generic signature covers every probe. Passing eight distinct pointers
// means a method with almost any small-argument shape gets something readable
// and non-aliasing in each register / stack slot.
typedef void *(*wr_probe_fn_t)(void *, void *, void *, void *,
                               void *, void *, void *, void *);

static unsigned char *g_scratch = NULL;
static unsigned char *g_shadow = NULL;

struct WrBlackEntry
{
    char iface[64];
    int index;
};
static WrBlackEntry g_black[WR_MAX_BLACKLIST];
static int g_blackCount = 0;

bool WrProbeInit(void)
{
    if (g_scratch)
        return true;

    g_scratch = (unsigned char *)VirtualAlloc(
        NULL, WR_SCRATCH_SLOTS * WR_SCRATCH_SLOT_SIZE,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_shadow = (unsigned char *)VirtualAlloc(
        NULL, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (!g_scratch || !g_shadow)
    {
        WrLogf("[!] probe scratch allocation failed");
        return false;
    }
    WrProbeLoadBlacklist();
    return true;
}

void *WrScratch(int slot)
{
    if (!g_scratch || slot < 0 || slot >= WR_SCRATCH_SLOTS)
        return NULL;
    return g_scratch + (size_t)slot * WR_SCRATCH_SLOT_SIZE;
}

void WrProbeReset(void)
{
    if (g_scratch)
        memset(g_scratch, 0, WR_SCRATCH_SLOTS * WR_SCRATCH_SLOT_SIZE);
}

void *WrShadow(void *iface)
{
    if (!g_shadow || !iface)
        return NULL;
    memset(g_shadow, 0, 8192);
    if (!WrSafeReadBytes(iface, g_shadow, WR_SHADOW_BYTES))
        return NULL;
    return g_shadow;
}

// ---------------------------------------------------------------------------
// Crash-resume breadcrumb
// ---------------------------------------------------------------------------

static const char *BreadcrumbPath(void)
{
    return WrDataPath("wrlines_probe_state.tmp");
}

void WrProbeBegin(const char *iface, int index)
{
    FILE *f = NULL;
    if (fopen_s(&f, BreadcrumbPath(), "w") == 0 && f)
    {
        fprintf(f, "%s|%d\n", iface, index);
        fflush(f);
        // Get it onto the disk, not just into the CRT buffer -- the whole point
        // is that it survives the process dying in the very next instruction.
        FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(f)));
        fclose(f);
    }
}

void WrProbeEnd(void)
{
    DeleteFileA(BreadcrumbPath());
}

static void AddBlacklist(const char *iface, int index)
{
    if (g_blackCount >= WR_MAX_BLACKLIST)
        return;
    strcpy_s(g_black[g_blackCount].iface, sizeof(g_black[0].iface), iface);
    g_black[g_blackCount].index = index;
    g_blackCount++;
}

bool WrProbeIsBlacklisted(const char *iface, int index)
{
    for (int i = 0; i < g_blackCount; i++)
        if (g_black[i].index == index && _stricmp(g_black[i].iface, iface) == 0)
            return true;
    return false;
}

void WrProbeLoadBlacklist(void)
{
    // Anything left in the breadcrumb file is a slot whose call never returned.
    FILE *f = NULL;
    if (fopen_s(&f, BreadcrumbPath(), "r") == 0 && f)
    {
        char line[128];
        if (fgets(line, sizeof(line), f))
        {
            char *bar = strchr(line, '|');
            if (bar)
            {
                *bar = '\0';
                int idx = atoi(bar + 1);
                AddBlacklist(line, idx);
                WrLogf("[!] previous run died probing %s index %d -- blacklisted",
                       line, idx);
            }
        }
        fclose(f);
        DeleteFileA(BreadcrumbPath());

        // Persist it so the blacklist survives more than one restart.
        FILE *bf = NULL;
        if (fopen_s(&bf, WrDataPath("wrlines_blacklist.txt"), "a") == 0 && bf)
        {
            fprintf(bf, "%s|%d\n", g_black[g_blackCount - 1].iface,
                    g_black[g_blackCount - 1].index);
            fclose(bf);
        }
    }

    FILE *bf = NULL;
    if (fopen_s(&bf, WrDataPath("wrlines_blacklist.txt"), "r") == 0 && bf)
    {
        char line[128];
        while (fgets(line, sizeof(line), bf))
        {
            char *bar = strchr(line, '|');
            if (!bar)
                continue;
            *bar = '\0';
            int idx = atoi(bar + 1);
            if (!WrProbeIsBlacklisted(line, idx))
                AddBlacklist(line, idx);
        }
        fclose(bf);
    }
    if (g_blackCount)
        WrLogf("probe blacklist holds %d entr%s", g_blackCount,
               g_blackCount == 1 ? "y" : "ies");
}

// ---------------------------------------------------------------------------
// Guarded reads
// ---------------------------------------------------------------------------

bool WrSafeReadBytes(const void *src, void *dst, size_t n)
{
    if (!src || !dst)
        return false;
    __try
    {
        memcpy(dst, src, n);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool WrSafeReadFloats(const void *src, float *dst, int count)
{
    if (!WrSafeReadBytes(src, dst, sizeof(float) * (size_t)count))
        return false;
    for (int i = 0; i < count; i++)
        if (!WrSaneFloat(dst[i]))
            return false;
    return true;
}

int WrSafeReadString(const void *src, char *dst, int maxLen)
{
    if (!src || !dst || maxLen <= 1)
        return -1;
    __try
    {
        const char *s = (const char *)src;
        int i = 0;
        for (; i < maxLen - 1; i++)
        {
            char c = s[i];
            if (c == '\0')
                break;
            if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E)
                return -1;      // not a printable ASCII string; reject
            dst[i] = c;
        }
        if (i >= maxLen - 1)
            return -1;          // no terminator in range
        dst[i] = '\0';
        return i;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

// ---------------------------------------------------------------------------
// The call itself
// ---------------------------------------------------------------------------

bool WrProbeCall(HMODULE owner, void *iface, int index, void **ret)
{
    if (ret)
        *ret = NULL;
    if (!iface || index < 0 || !g_scratch)
        return false;

    void **vtable = NULL;
    if (!WrSafeReadBytes(iface, &vtable, sizeof(vtable)) || !vtable)
        return false;

    void *fn = NULL;
    if (!WrSafeReadBytes(&vtable[index], &fn, sizeof(fn)) || !fn)
        return false;

    // Guard 1: it has to be code, in the module the interface came from.
    if (!WrIsCodeIn(owner, fn))
        return false;

    // Guard 2: a shadow `this`, so an sret write lands on our page.
    void *self = WrShadow(iface);
    if (!self)
        return false;

    // Guard 3: eight distinct, zeroed, readable/writable argument slots.
    WrProbeReset();

    wr_probe_fn_t call = (wr_probe_fn_t)fn;
    __try
    {
        void *r = call(self, WrScratch(1), WrScratch(2), WrScratch(3),
                       WrScratch(4), WrScratch(5), WrScratch(6), WrScratch(7));
        if (ret)
            *ret = r;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
