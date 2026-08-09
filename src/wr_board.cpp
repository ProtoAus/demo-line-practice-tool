// wr_board.cpp  --  see wr_board.h.

#include "wr_board.h"
#include "wr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *kGamemodes[WR_GAMEMODE_COUNT] = {
    "surf", "bhop", "bhop (HL1)", "climb (Mom)", "climb (KZT)", "climb (16)",
    "RJ", "SJ", "ahop", "conc", "defrag CPM", "defrag VQ3", "defrag VTG"
};

const char *WrGamemodeName(int mode)
{
    if (mode < 1 || mode > WR_GAMEMODE_COUNT)
        return "?";
    return kGamemodes[mode - 1];
}

// --- state ------------------------------------------------------------------
//
// Double-buffered rather than free-and-swap.
//
// wr_maps.cpp hands out a raw pointer into the block its reader thread will
// free, and gets away with it because a map refresh only happens on a button
// press. A board reloads whenever a download finishes, so the window where the
// UI is walking rows that are being freed is real. Two buffers and an index
// mean the reader always fills the one nobody is reading.

struct Buffer
{
    WrBoardRow *rows;
    int count;
    int total;
    long long fetched;
    char map[72];
    int mapId;
};

static Buffer g_buf[2];
static volatile LONG g_front = 0;
static volatile LONG g_busy = 0;
static bool g_ready = false;
static char g_status[224] = "nothing asked for yet";

static CRITICAL_SECTION g_cs;
static bool g_csReady = false;

// What the next read should load. Written by the UI thread before starting a
// read, so the worker takes a copy under the lock.
static char g_wantMap[72] = {0};
static int g_wantMode = 1, g_wantType = 0, g_wantNum = 1;

static void EnsureCs(void)
{
    if (!g_csReady)
    {
        InitializeCriticalSection(&g_cs);
        g_csReady = true;
    }
}

// --- the "do we already hold this demo" set ---------------------------------
//
// One walk of the three demo trees into a sorted array of lowercased basenames,
// then a binary search per row. The alternative -- GetFileAttributes on three
// candidate paths per row -- is 60 000 syscalls on a 20 000-row board.

struct HashSet
{
    char (*h)[48];
    int n, cap;
};

static int CompareHash(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void HashAdd(HashSet *s, const char *name)
{
    if (s->n >= s->cap)
    {
        int cap = s->cap ? s->cap * 2 : 256;
        char (*grown)[48] = (char (*)[48])realloc(s->h, sizeof(*grown) * cap);
        if (!grown)
            return;
        s->h = grown;
        s->cap = cap;
    }
    strncpy_s(s->h[s->n], 48, name, _TRUNCATE);
    for (char *p = s->h[s->n]; *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p = (char)(*p - 'A' + 'a');
    s->n++;
}

static void ScanDir(HashSet *s, const char *dir)
{
    char pat[MAX_PATH];
    _snprintf_s(pat, sizeof(pat), _TRUNCATE, "%s\\*.mtv", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        char base[64];
        strncpy_s(base, sizeof(base), fd.cFileName, _TRUNCATE);
        char *dot = strrchr(base, '.');
        if (dot)
            *dot = '\0';
        HashAdd(s, base);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static bool HashHas(const HashSet *s, const char *hash)
{
    if (!s->h || s->n <= 0)
        return false;
    char key[48];
    strncpy_s(key, sizeof(key), hash, _TRUNCATE);
    for (char *p = key; *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p = (char)(*p - 'A' + 'a');
    return bsearch(key, s->h, s->n, 48, CompareHash) != NULL;
}

// --- parsing ----------------------------------------------------------------

static const char *NextField(const char *p, char *out, size_t cap)
{
    size_t n = 0;
    while (*p && *p != '\t' && *p != '\n' && *p != '\r')
    {
        if (n + 1 < cap)
            out[n++] = *p;
        p++;
    }
    out[n] = '\0';
    return (*p == '\t') ? p + 1 : p;
}

int WrBoardParseFile(const char *path, WrBoardRow *out, int maxRows,
                     int *total, long long *fetched, int *mapId)
{
    if (total)   *total = 0;
    if (fetched) *fetched = 0;
    if (mapId)   *mapId = 0;

    FILE *f = NULL;
    if (fopen_s(&f, path, "r") != 0 || !f)
        return -1;

    int n = 0;
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        if (line[0] == '\n' || line[0] == '\r')
            continue;

        // "# key<TAB>value" carries what the cache knows about itself. The
        // column-name header line has no second field it can be confused with,
        // so both are handled by the same parse.
        if (line[0] == '#')
        {
            char key[32], val[64];
            const char *p = line + 1;
            while (*p == ' ')
                p++;
            p = NextField(p, key, sizeof(key));
            NextField(p, val, sizeof(val));
            if (total && _stricmp(key, "total") == 0)
                *total = atoi(val);
            else if (fetched && _stricmp(key, "fetched") == 0)
                *fetched = _atoi64(val);
            else if (mapId && _stricmp(key, "mapid") == 0)
                *mapId = atoi(val);
            continue;
        }

        if (n >= maxRows)
            break;

        char buf[128];
        const char *p = line;
        WrBoardRow r;
        memset(&r, 0, sizeof(r));

        p = NextField(p, buf, sizeof(buf));  r.rank = atoi(buf);
        p = NextField(p, buf, sizeof(buf));  r.time = (float)atof(buf);
        p = NextField(p, buf, sizeof(buf));  r.steamId = _strtoui64(buf, NULL, 10);
        p = NextField(p, r.alias, sizeof(r.alias));
        p = NextField(p, r.hash, sizeof(r.hash));
        p = NextField(p, buf, sizeof(buf));  r.dateEpoch = _atoi64(buf);
        NextField(p, r.url, sizeof(r.url));

        // A row without a rank or a hash is not a row. Python writes neither
        // as empty, so this only fires on a truncated or hand-edited file --
        // which is exactly when quietly keeping half a record is worst.
        if (r.rank <= 0 || !r.hash[0])
            continue;

        out[n++] = r;
    }
    fclose(f);
    return n;
}

// --- the reader thread ------------------------------------------------------

static void BoardPath(char *out, size_t cap, const char *map, int mode,
                      int type, int num)
{
    char rel[160];
    _snprintf_s(rel, sizeof(rel), _TRUNCATE, "boards\\%s_g%d_t%d%d.tsv",
                map, mode, type, num);
    strncpy_s(out, cap, WrDataPath(rel), _TRUNCATE);
}

static DWORD WINAPI ReadThread(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    char map[72];
    int mode, type, num;
    EnterCriticalSection(&g_cs);
    strcpy_s(map, sizeof(map), g_wantMap);
    mode = g_wantMode; type = g_wantType; num = g_wantNum;
    LeaveCriticalSection(&g_cs);

    Buffer *back = &g_buf[1 - g_front];
    free(back->rows);
    back->rows = NULL;
    back->count = 0;
    back->total = 0;
    back->fetched = 0;
    back->mapId = 0;
    strcpy_s(back->map, sizeof(back->map), map);

    char status[224];

    if (!map[0])
    {
        strcpy_s(status, sizeof(status), "no map selected");
    }
    else
    {
        char path[MAX_PATH];
        BoardPath(path, sizeof(path), map, mode, type, num);

        WrBoardRow *rows = (WrBoardRow *)malloc(sizeof(WrBoardRow) * WR_BOARD_MAX);
        int n = rows ? WrBoardParseFile(path, rows, WR_BOARD_MAX, &back->total,
                                        &back->fetched, &back->mapId) : -1;
        if (n < 0)
        {
            free(rows);
            _snprintf_s(status, sizeof(status), _TRUNCATE,
                        "nothing cached for %s in %s, track %d/%d -- fetch a "
                        "window below", map, WrGamemodeName(mode), type, num);
        }
        else
        {
            // Resolve `have` against the three trees a .mtv can be in. Keyed by
            // map id for the game's online tree and by name for the other two,
            // which is why this is three walks and not one.
            HashSet set;
            memset(&set, 0, sizeof(set));

            char dir[MAX_PATH];
            if (back->mapId > 0)
            {
                // The game's own downloads sit under the numeric map id, which
                // is why the cache file carries it: without that number this
                // tree cannot be found from a map name.
                _snprintf_s(dir, sizeof(dir), _TRUNCATE,
                            "%s\\momentum\\momtv\\online\\%d", WrGameDir(),
                            back->mapId);
                ScanDir(&set, dir);
            }
            _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\momentum\\momtv\\local\\%s",
                        WrGameDir(), map);
            ScanDir(&set, dir);
            char rel[MAX_PATH];
            _snprintf_s(rel, sizeof(rel), _TRUNCATE, "demos\\%s", map);
            ScanDir(&set, WrDataPath(rel));

            if (set.n > 1)
                qsort(set.h, set.n, 48, CompareHash);
            for (int i = 0; i < n; i++)
                rows[i].have = HashHas(&set, rows[i].hash);
            free(set.h);

            back->rows = rows;
            back->count = n;

            int held = 0;
            for (int i = 0; i < n; i++)
                if (rows[i].have)
                    held++;

            if (back->total > 0)
                _snprintf_s(status, sizeof(status), _TRUNCATE,
                            "%d of %d places cached, %d of them downloaded",
                            n, back->total, held);
            else
                _snprintf_s(status, sizeof(status), _TRUNCATE,
                            "%d places cached, %d of them downloaded", n, held);
            if (n >= WR_BOARD_MAX)
                strcat_s(status, sizeof(status), " -- and the file was longer");
        }
    }

    EnterCriticalSection(&g_cs);
    strcpy_s(g_status, sizeof(g_status), status);
    InterlockedExchange(&g_front, 1 - g_front);
    g_ready = true;
    LeaveCriticalSection(&g_cs);

    InterlockedExchange(&g_busy, 0);
    return 0;
}

static void Start(void)
{
    EnsureCs();
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0)
        return;
    HANDLE h = CreateThread(NULL, 0, ReadThread, NULL, 0, NULL);
    if (h)
        CloseHandle(h);
    else
        InterlockedExchange(&g_busy, 0);
}

void WrBoardLoad(const char *map, int gamemode, int trackType, int trackNum)
{
    EnsureCs();
    EnterCriticalSection(&g_cs);
    strcpy_s(g_wantMap, sizeof(g_wantMap), map ? map : "");
    g_wantMode = gamemode;
    g_wantType = trackType;
    g_wantNum = trackNum;
    LeaveCriticalSection(&g_cs);
    Start();
}

void WrBoardRefresh(void) { Start(); }
bool WrBoardReady(void) { return g_ready; }
bool WrBoardBusy(void) { return g_busy != 0; }
const char *WrBoardStatus(void) { return g_status; }

int WrBoardTotal(void) { return g_buf[g_front].total; }
long long WrBoardFetched(void) { return g_buf[g_front].fetched; }
const char *WrBoardMap(void) { return g_buf[g_front].map; }
int WrBoardMapId(void) { return g_buf[g_front].mapId; }
int WrBoardCount(void) { return g_buf[g_front].count; }

const WrBoardRow *WrBoardAt(int index)
{
    const Buffer *b = &g_buf[g_front];
    if (!b->rows || index < 0 || index >= b->count)
        return NULL;
    return &b->rows[index];
}

void WrBoardShutdown(void)
{
    for (int i = 0; i < 2; i++)
    {
        free(g_buf[i].rows);
        g_buf[i].rows = NULL;
        g_buf[i].count = 0;
    }
}
