// wr_savelocs.cpp  --  see wr_savelocs.h.

#include "wr_savelocs.h"
#include "wr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SAVELOCS 256

// How close a position has to be to count as "the same save-loc". Momentum
// restores the exact stored origin, so this only has to absorb the difference
// between the player origin it stores and the camera we see -- which is why the
// test below is horizontal, with a generous vertical allowance.
#define MATCH_RADIUS 24.0f
#define MATCH_VERTICAL 96.0f

struct Saveloc
{
    Vec3 pos;
    float ourTime;      // seconds, or -1 when we have never timed it
};

static CRITICAL_SECTION g_cs;
static bool g_csReady = false;

static char g_map[72] = {0};
static Saveloc g_locs[MAX_SAVELOCS];
static int g_count = 0;
static int g_timed = 0;
static char g_status[160] = "not looked yet";

static long long g_mtime = 0;
static volatile LONG g_busy = 0;
static HANDLE g_thread = NULL;
static float g_elapsed = 0.0f;

static void EnsureCs(void)
{
    if (!g_csReady)
    {
        InitializeCriticalSection(&g_cs);
        g_csReady = true;
    }
}

static const char *GamePath(void)
{
    static char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\momentum\\savedlocs.txt",
                WrGameDir());
    return path;
}

static const char *SidecarPath(const char *map)
{
    static char rel[MAX_PATH];
    _snprintf_s(rel, sizeof(rel), _TRUNCATE, "savelocs\\%s.txt", map);
    return WrDataPath(rel);
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
//
// Not a general KeyValues parser, and deliberately so. We need one thing: the
// "pos" of every entry inside one named map's block. So this tracks brace depth,
// notes the depth at which the map's block opened, and reads pos lines below it.
// A malformed file yields no save-locs rather than a crash.

static bool ParseGameFile(const char *map, Saveloc *out, int maxOut, int *count)
{
    *count = 0;

    FILE *f = NULL;
    if (fopen_s(&f, GamePath(), "r") != 0 || !f)
        return false;

    char line[512];
    int depth = 0;
    int mapDepth = -1;
    bool inMap = false;
    char pendingKey[128] = {0};

    while (fgets(line, sizeof(line), f))
    {
        // Trim.
        char *p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;

        if (*p == '{')
        {
            depth++;
            if (!inMap && pendingKey[0] && _stricmp(pendingKey, map) == 0)
            {
                inMap = true;
                mapDepth = depth;
            }
            pendingKey[0] = '\0';
            continue;
        }
        if (*p == '}')
        {
            if (inMap && depth == mapDepth)
                break;                  // finished this map's block
            depth--;
            pendingKey[0] = '\0';
            continue;
        }

        // "key"  "value"   or   "key" alone (a block name on the next line).
        if (*p != '"')
            continue;
        p++;
        char key[128];
        int n = 0;
        while (*p && *p != '"' && n < (int)sizeof(key) - 1)
            key[n++] = *p++;
        key[n] = '\0';
        if (*p != '"')
            continue;
        p++;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != '"')
        {
            strcpy_s(pendingKey, sizeof(pendingKey), key);
            continue;
        }
        p++;
        char val[160];
        n = 0;
        while (*p && *p != '"' && n < (int)sizeof(val) - 1)
            val[n++] = *p++;
        val[n] = '\0';
        pendingKey[0] = '\0';

        if (inMap && _stricmp(key, "pos") == 0 && *count < maxOut)
        {
            float x = 0.0f, y = 0.0f, z = 0.0f;
            if (sscanf_s(val, "%f %f %f", &x, &y, &z) == 3)
            {
                out[*count].pos = WrVec(x, y, z);
                out[*count].ourTime = -1.0f;
                (*count)++;
            }
        }
    }
    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// Our sidecar
// ---------------------------------------------------------------------------

static void LoadSidecar(const char *map, Saveloc *locs, int count, int *timed)
{
    *timed = 0;
    FILE *f = NULL;
    if (fopen_s(&f, SidecarPath(map), "r") != 0 || !f)
        return;

    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        float x, y, z, t;
        if (sscanf_s(line, "%f %f %f %f", &x, &y, &z, &t) != 4)
            continue;
        for (int i = 0; i < count; i++)
        {
            float dx = locs[i].pos.x - x, dy = locs[i].pos.y - y;
            float dz = locs[i].pos.z - z;
            if (dx * dx + dy * dy + dz * dz < 1.0f)
            {
                locs[i].ourTime = t;
                (*timed)++;
                break;
            }
        }
    }
    fclose(f);
}

static void SaveSidecar(const char *map)
{
    char dir[MAX_PATH];
    strcpy_s(dir, sizeof(dir), WrDataPath("savelocs"));
    CreateDirectoryA(dir, NULL);

    FILE *f = NULL;
    if (fopen_s(&f, SidecarPath(map), "w") != 0 || !f)
        return;
    fprintf(f, "# WrLines: our own elapsed time at each of this map's save-locs.\n");
    fprintf(f, "# Momentum's savedlocs.txt has a \"time\" field but never fills\n");
    fprintf(f, "# it in, so this is where ours lives. Keyed on position, because\n");
    fprintf(f, "# save-loc indices renumber when one is deleted.\n");
    fprintf(f, "# x y z seconds\n");
    for (int i = 0; i < g_count; i++)
        if (g_locs[i].ourTime >= 0.0f)
            fprintf(f, "%.1f %.1f %.1f %.3f\n", g_locs[i].pos.x, g_locs[i].pos.y,
                    g_locs[i].pos.z, g_locs[i].ourTime);
    fclose(f);
}

// ---------------------------------------------------------------------------

static DWORD WINAPI ReadThread(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    char map[72];
    EnterCriticalSection(&g_cs);
    strcpy_s(map, sizeof(map), g_map);
    LeaveCriticalSection(&g_cs);

    static Saveloc found[MAX_SAVELOCS];
    int n = 0;
    bool ok = map[0] && ParseGameFile(map, found, MAX_SAVELOCS, &n);

    int timed = 0;
    if (ok)
        LoadSidecar(map, found, n, &timed);

    EnterCriticalSection(&g_cs);
    if (ok)
    {
        // Carry forward times for save-locs that are still there, so a re-read
        // triggered by the user making a new one does not lose the others.
        for (int i = 0; i < n; i++)
        {
            if (found[i].ourTime >= 0.0f)
                continue;
            for (int j = 0; j < g_count; j++)
            {
                if (g_locs[j].ourTime < 0.0f)
                    continue;
                float dx = g_locs[j].pos.x - found[i].pos.x;
                float dy = g_locs[j].pos.y - found[i].pos.y;
                float dz = g_locs[j].pos.z - found[i].pos.z;
                if (dx * dx + dy * dy + dz * dz < 1.0f)
                {
                    found[i].ourTime = g_locs[j].ourTime;
                    timed++;
                    break;
                }
            }
        }

        memcpy(g_locs, found, sizeof(Saveloc) * (size_t)n);
        g_count = n;
        g_timed = timed;
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "%d save-loc%s for this map, %d with a time",
                    n, n == 1 ? "" : "s", timed);
    }
    else
    {
        g_count = 0;
        g_timed = 0;
        strcpy_s(g_status, sizeof(g_status),
                 "no savedlocs.txt, or none for this map");
    }
    LeaveCriticalSection(&g_cs);

    InterlockedExchange(&g_busy, 0);
    return 0;
}

void WrSavelocRefresh(const char *map)
{
    EnsureCs();
    if (!map || !*map || !WrGameDir()[0])
        return;

    // Only when the file has actually changed, or the map has.
    WIN32_FILE_ATTRIBUTE_DATA fad;
    long long mtime = 0;
    if (GetFileAttributesExA(GamePath(), GetFileExInfoStandard, &fad))
        mtime = ((long long)fad.ftLastWriteTime.dwHighDateTime << 32) |
                fad.ftLastWriteTime.dwLowDateTime;

    bool mapChanged;
    EnterCriticalSection(&g_cs);
    mapChanged = (strcmp(map, g_map) != 0);
    if (mapChanged)
        strcpy_s(g_map, sizeof(g_map), map);
    LeaveCriticalSection(&g_cs);

    if (!mapChanged && mtime == g_mtime)
        return;
    g_mtime = mtime;

    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0)
        return;
    if (g_thread)
    {
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    g_thread = CreateThread(NULL, 0, ReadThread, NULL, 0, NULL);
    if (!g_thread)
        InterlockedExchange(&g_busy, 0);
}

// Stamp a save-loc that has no time yet and that the player is standing on.
//
// That is what "the user just made a save-loc" looks like from out here: the
// file's timestamp changed, the re-read turned up an entry we have never timed,
// and the player is at it. There is no need to know which command they typed.
void WrSavelocTick(const Vec3 &cam, float elapsed, bool running)
{
    if (!g_csReady || !running)
        return;
    g_elapsed = elapsed;

    EnterCriticalSection(&g_cs);
    int best = -1;
    float bestD = MATCH_RADIUS * MATCH_RADIUS;
    for (int i = 0; i < g_count; i++)
    {
        if (g_locs[i].ourTime >= 0.0f)
            continue;                   // already timed; leave it alone
        float dz = g_locs[i].pos.z - cam.z;
        if (dz > MATCH_VERTICAL || dz < -MATCH_VERTICAL)
            continue;
        float dx = g_locs[i].pos.x - cam.x, dy = g_locs[i].pos.y - cam.y;
        float d = dx * dx + dy * dy;
        if (d < bestD) { bestD = d; best = i; }
    }
    bool dirty = false;
    if (best >= 0)
    {
        g_locs[best].ourTime = elapsed;
        g_timed++;
        dirty = true;
        WrLogf("saveloc: stamped the one at (%.0f %.0f %.0f) with %.2fs",
               g_locs[best].pos.x, g_locs[best].pos.y, g_locs[best].pos.z,
               elapsed);
    }
    LeaveCriticalSection(&g_cs);
    if (dirty)
        SaveSidecar(g_map);
}

bool WrSavelocTimeAt(const Vec3 &pos, float *seconds)
{
    if (!g_csReady)
        return false;
    bool found = false;
    EnterCriticalSection(&g_cs);
    float bestD = MATCH_RADIUS * MATCH_RADIUS;
    for (int i = 0; i < g_count; i++)
    {
        if (g_locs[i].ourTime < 0.0f)
            continue;
        float dz = g_locs[i].pos.z - pos.z;
        if (dz > MATCH_VERTICAL || dz < -MATCH_VERTICAL)
            continue;
        float dx = g_locs[i].pos.x - pos.x, dy = g_locs[i].pos.y - pos.y;
        float d = dx * dx + dy * dy;
        if (d < bestD)
        {
            bestD = d;
            if (seconds) *seconds = g_locs[i].ourTime;
            found = true;
        }
    }
    LeaveCriticalSection(&g_cs);
    return found;
}

int WrSavelocCount(void) { return g_count; }
int WrSavelocTimedCount(void) { return g_timed; }
const char *WrSavelocStatus(void) { return g_status; }

void WrSavelocShutdown(void) {}
