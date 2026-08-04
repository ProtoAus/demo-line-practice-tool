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

// Sidecar format version. 1 had no header line and keyed on position alone; its
// times were also written by the proximity bug, so they load as `suspect`.
#define SIDECAR_VERSION 2
#define SIDECAR_TAG "wrlines-savelocs"

struct Saveloc
{
    Vec3 pos;
    float ourTime;      // seconds, or -1 when we have never timed it
    int ordinal;        // among entries sharing this position, in file order
    bool suspect;       // time came from a v1 sidecar
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

// The run clock as it stood when a change to the game's file was noticed. This
// is what a newly created save-loc is stamped with, and capturing it here rather
// than when the read finishes is the point: the read is on a background thread
// and takes as long as it takes.
static float g_stampClock = -1.0f;
static bool g_stampValid = false;

// Something worth showing on screen for a moment.
static char g_recent[96] = {0};
static float g_recentAge = 1e9f;

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

// Number each entry among those sharing its position, in file order. Without
// this two save-locs at the same respawn point are indistinguishable, and the
// nearest-wins search below always picks whichever came first.
static void AssignOrdinals(Saveloc *locs, int count)
{
    for (int i = 0; i < count; i++)
    {
        int n = 0;
        for (int j = 0; j < i; j++)
        {
            float dx = locs[j].pos.x - locs[i].pos.x;
            float dy = locs[j].pos.y - locs[i].pos.y;
            float dz = locs[j].pos.z - locs[i].pos.z;
            if (dx * dx + dy * dy + dz * dz < 1.0f)
                n++;
        }
        locs[i].ordinal = n;
    }
}

static void LoadSidecar(const char *map, Saveloc *locs, int count, int *timed)
{
    *timed = 0;
    FILE *f = NULL;
    if (fopen_s(&f, SidecarPath(map), "r") != 0 || !f)
        return;

    int version = 1;        // no tag line means the original format
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        if (line[0] == '\n' || line[0] == '\r')
            continue;
        if (line[0] == '#')
        {
            int v = 0;
            if (sscanf_s(line, "# " SIDECAR_TAG " %d", &v) == 1 && v > 0)
                version = v;
            continue;
        }

        float x, y, z, t;
        int ord = 0;
        if (version >= 2)
        {
            if (sscanf_s(line, "%f %f %f %d %f", &x, &y, &z, &ord, &t) != 5)
                continue;
        }
        else if (sscanf_s(line, "%f %f %f %f", &x, &y, &z, &t) != 4)
        {
            continue;
        }

        for (int i = 0; i < count; i++)
        {
            if (locs[i].ourTime >= 0.0f)
                continue;
            float dx = locs[i].pos.x - x, dy = locs[i].pos.y - y;
            float dz = locs[i].pos.z - z;
            if (dx * dx + dy * dy + dz * dz >= 1.0f)
                continue;
            // In v2 the ordinal disambiguates a shared position. In v1 there is
            // none, so the first untimed match takes it -- which is the
            // ambiguity that version exists to record.
            if (version >= 2 && locs[i].ordinal != ord)
                continue;
            locs[i].ourTime = t;
            locs[i].suspect = (version < SIDECAR_VERSION);
            (*timed)++;
            break;
        }
    }
    fclose(f);
}

// Takes a snapshot rather than reading the shared array. It used to be called
// after LeaveCriticalSection and read g_locs/g_count/g_map directly, while the
// background reader could be memcpy-ing over all three -- and it does file I/O,
// so holding the lock across it instead would put a synchronous disk write
// inside Present with a background thread waiting on it.
static void SaveSidecar(const char *map, const Saveloc *locs, int count)
{
    char dir[MAX_PATH];
    strcpy_s(dir, sizeof(dir), WrDataPath("savelocs"));
    CreateDirectoryA(dir, NULL);

    FILE *f = NULL;
    if (fopen_s(&f, SidecarPath(map), "w") != 0 || !f)
        return;
    fprintf(f, "# " SIDECAR_TAG " %d\n", SIDECAR_VERSION);
    fprintf(f, "# WrLines: our own elapsed time at each of this map's save-locs.\n");
    fprintf(f, "# Momentum's savedlocs.txt has a \"time\" field but never fills\n");
    fprintf(f, "# it in, so this is where ours lives. Keyed on position (indices\n");
    fprintf(f, "# renumber when one is deleted) plus an ordinal, because several\n");
    fprintf(f, "# save-locs commonly share a respawn point.\n");
    fprintf(f, "# x y z ordinal seconds\n");
    for (int i = 0; i < count; i++)
    {
        // Zero is refused. The clock only starts once you leave the anchor, so
        // 0.000 can only mean it was stamped before the clock ran -- and every
        // 0.000 already on disk came from exactly that bug, then restored itself
        // for ever afterwards.
        if (locs[i].ourTime <= 0.0f)
            continue;
        fprintf(f, "%.1f %.1f %.1f %d %.3f\n", locs[i].pos.x, locs[i].pos.y,
                locs[i].pos.z, locs[i].ordinal, locs[i].ourTime);
    }
    fclose(f);
}

// Copy out under the lock, write outside it.
static void FlushSidecar(void)
{
    static Saveloc snap[MAX_SAVELOCS];
    char map[72];
    int n;

    EnterCriticalSection(&g_cs);
    n = g_count;
    memcpy(snap, g_locs, sizeof(Saveloc) * (size_t)n);
    strcpy_s(map, sizeof(map), g_map);
    LeaveCriticalSection(&g_cs);

    if (map[0])
        SaveSidecar(map, snap, n);
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
    {
        AssignOrdinals(found, n);
        LoadSidecar(map, found, n, &timed);
    }

    bool dirty = false;
    EnterCriticalSection(&g_cs);
    if (ok)
    {
        bool firstForMap = (g_count == 0 && g_timed == 0);

        // Carry forward times for save-locs that are still there, so a re-read
        // triggered by the user making a new one does not lose the others, and
        // note which entries matched nothing we already knew.
        for (int i = 0; i < n; i++)
        {
            if (found[i].ourTime >= 0.0f)
                continue;
            bool matched = false;
            for (int j = 0; j < g_count; j++)
            {
                float dx = g_locs[j].pos.x - found[i].pos.x;
                float dy = g_locs[j].pos.y - found[i].pos.y;
                float dz = g_locs[j].pos.z - found[i].pos.z;
                if (dx * dx + dy * dy + dz * dz >= 1.0f)
                    continue;
                if (g_locs[j].ordinal != found[i].ordinal)
                    continue;
                matched = true;
                if (g_locs[j].ourTime >= 0.0f)
                {
                    found[i].ourTime = g_locs[j].ourTime;
                    found[i].suspect = g_locs[j].suspect;
                    timed++;
                }
                break;
            }

            // THIS is a save-loc that was just made: the game's file changed,
            // and the re-read turned up an entry that matches nothing we held.
            // Not "the player is standing near an untimed one", which is what
            // this used to test and which fires every time you walk past.
            //
            // Suppressed on the first read for a map, where "new" only means
            // "we have not looked here before".
            if (!matched && !firstForMap && g_stampValid && g_stampClock > 0.0f)
            {
                found[i].ourTime = g_stampClock;
                found[i].suspect = false;
                timed++;
                dirty = true;
                WrLogf("saveloc: a NEW save-loc at (%.0f %.0f %.0f) stamped with "
                       "%.2fs", found[i].pos.x, found[i].pos.y, found[i].pos.z,
                       g_stampClock);
                _snprintf_s(g_recent, sizeof(g_recent), _TRUNCATE,
                            "save-loc saved at %.2fs", g_stampClock);
                g_recentAge = 0.0f;
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

    if (dirty)
        FlushSidecar();

    InterlockedExchange(&g_busy, 0);
    return 0;
}

void WrSavelocRefresh(const char *map, float elapsed, bool running)
{
    EnsureCs();
    if (g_recentAge < 1e8f)
        g_recentAge += 1.0f / 200.0f;   // aged approximately; display only
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

    // Captured HERE, not when the read finishes. The read is on a background
    // thread and takes as long as it takes; the clock that belongs to a new
    // save-loc is the one at the instant the file changed.
    g_stampClock = elapsed;
    g_stampValid = running && !mapChanged;

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

bool WrSavelocAt(int index, WrSavelocRow *out)
{
    if (!g_csReady)
        return false;
    bool ok = false;
    EnterCriticalSection(&g_cs);
    if (index >= 0 && index < g_count)
    {
        if (out)
        {
            out->pos = g_locs[index].pos;
            out->seconds = g_locs[index].ourTime;
            out->suspect = g_locs[index].suspect;
        }
        ok = true;
    }
    LeaveCriticalSection(&g_cs);
    return ok;
}

void WrSavelocForget(int index)
{
    if (!g_csReady)
        return;
    bool dirty = false;
    EnterCriticalSection(&g_cs);
    if (index >= 0 && index < g_count && g_locs[index].ourTime >= 0.0f)
    {
        g_locs[index].ourTime = -1.0f;
        g_locs[index].suspect = false;
        if (g_timed > 0) g_timed--;
        dirty = true;
    }
    LeaveCriticalSection(&g_cs);
    if (dirty)
        FlushSidecar();
}

void WrSavelocForgetAll(void)
{
    if (!g_csReady)
        return;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_count; i++)
    {
        g_locs[i].ourTime = -1.0f;
        g_locs[i].suspect = false;
    }
    g_timed = 0;
    LeaveCriticalSection(&g_cs);
    FlushSidecar();
    WrLogf("saveloc: every time for this map forgotten, on request");
}

const char *WrSavelocRecent(float *ageSeconds)
{
    if (ageSeconds) *ageSeconds = g_recentAge;
    return g_recent;
}

void WrSavelocNoteRestore(float seconds)
{
    _snprintf_s(g_recent, sizeof(g_recent), _TRUNCATE,
                "clock restored to %.2fs", seconds);
    g_recentAge = 0.0f;
}

void WrSavelocShutdown(void) {}
