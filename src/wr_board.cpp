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

// --- the cache file: one reader, one writer ---------------------------------

void WrBoardCachePath(char *out, int cap, const char *map, int gamemode,
                      int trackType, int trackNum)
{
    char rel[192];
    _snprintf_s(rel, sizeof(rel), _TRUNCATE, "boards\\%s_g%d_t%d%d.tsv",
                map ? map : "", gamemode, trackType, trackNum);
    strncpy_s(out, (size_t)cap, WrDataPath(rel), _TRUNCATE);
}

static bool IsAsciiSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// Python's int(s): optional surrounding whitespace, an optional sign, and then
// nothing but digits. Deliberately stricter than atoi, which reads "5abc" as 5
// where the reference raises ValueError and drops the row -- and a row silently
// kept on one side and dropped on the other is precisely the kind of difference
// this file exists to not have.
static bool ParseInt(const char *s, long long *out)
{
    while (IsAsciiSpace(*s)) s++;
    const char *start = s;
    if (*s == '+' || *s == '-') s++;
    if (*s < '0' || *s > '9')
        return false;
    char *end = NULL;
    long long v = _strtoi64(start, &end, 10);
    if (!end) return false;
    while (IsAsciiSpace(*end)) end++;
    if (*end != '\0')
        return false;
    *out = v;
    return true;
}

// Python's float(s), near enough. strtod also accepts C99 hex floats, which
// float() does not; nothing writes one and a hand-typed "0x10" in a time column
// is not a case worth thirty lines.
static bool ParseReal(const char *s, double *out)
{
    while (IsAsciiSpace(*s)) s++;
    if (!*s) return false;
    char *end = NULL;
    double v = strtod(s, &end);
    if (!end || end == s) return false;
    while (IsAsciiSpace(*end)) end++;
    if (*end != '\0')
        return false;
    *out = v;
    return true;
}

static void LowerAscii(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z')
            *s = (char)(*s - 'A' + 'a');
}

// The reference keys its dict on hash.lower(). A replay hash is hex, so ASCII
// folding is the whole of str.lower() here -- and _stricmp is avoided anyway,
// because it consults the C locale and a hash comparison should not depend on
// what locale the game happened to start in.
static bool SameHash(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
    {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a - 'A' + 'a') : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b - 'A' + 'a') : *b;
        if (x != y)
            return false;
    }
    return *a == *b;
}

int WrBoardCacheFindRow(const WrBoardCache *c, const char *hash)
{
    for (int i = 0; i < c->count; i++)
        if (SameHash(c->rows[i].hash, hash))
            return i;
    return -1;
}

bool WrBoardCacheMerge(WrBoardCache *c, const WrBoardCacheRow *r)
{
    int at = WrBoardCacheFindRow(c, r->hash);
    if (at >= 0)
    {
        // Replaced where it already sits. A dict assignment to an existing key
        // does not move it, and the write order depends on that.
        c->rows[at] = *r;
        return false;
    }

    if (c->count >= c->cap)
    {
        int cap = c->cap ? c->cap * 2 : 256;
        WrBoardCacheRow *grown =
            (WrBoardCacheRow *)realloc(c->rows, sizeof(*grown) * (size_t)cap);
        if (!grown)
            return false;       // dropped, and the caller's count will show it
        c->rows = grown;
        c->cap = cap;
    }
    c->rows[c->count++] = *r;
    return true;
}

void WrBoardCacheFree(WrBoardCache *c)
{
    free(c->rows);
    memset(c, 0, sizeof(*c));
}

// Strip ASCII whitespace from both ends, in place.
static char *Strip(char *s)
{
    while (IsAsciiSpace(*s)) s++;
    size_t n = strlen(s);
    while (n > 0 && IsAsciiSpace(s[n - 1])) n--;
    s[n] = '\0';
    return s;
}

bool WrBoardReadCache(const char *path, WrBoardCache *c, int maxRows)
{
    memset(c, 0, sizeof(*c));

    FILE *f = NULL;
    if (fopen_s(&f, path, "r") != 0 || !f)
        return false;

    // Long enough for the longest line the writer can produce -- a 191-byte
    // alias and a 319-byte URL and the rest -- with room to spare. A line over
    // this is split by fgets, and the tail would parse as a row with too few
    // fields and be dropped, which is what the reference does with a line it
    // cannot make seven fields out of.
    char line[1024];
    while (fgets(line, sizeof(line), f))
    {
        // The reference rstrips "\n" then "\r". Opened in text mode here, so
        // the \r is already gone on a CRLF file and this catches a stray one.
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (n == 0)
            continue;

        if (line[0] == '#')
        {
            // "# key<TAB>value", where value keeps any further tabs -- that is
            // parts[1:] joined back with tabs, which is what write_board does.
            char *body = Strip(line + 1);
            char *tab = strchr(body, '\t');
            if (!tab)
                continue;
            *tab = '\0';
            const char *key = body, *val = tab + 1;

            if      (!strcmp(key, "map"))      strcpy_s(c->map, sizeof(c->map), val);
            else if (!strcmp(key, "mapid"))    strcpy_s(c->mapId, sizeof(c->mapId), val);
            else if (!strcmp(key, "gamemode")) strcpy_s(c->gamemode, sizeof(c->gamemode), val);
            else if (!strcmp(key, "track"))    strcpy_s(c->track, sizeof(c->track), val);
            else if (!strcmp(key, "total"))    strcpy_s(c->total, sizeof(c->total), val);
            else if (!strcmp(key, "fetched"))  strcpy_s(c->fetched, sizeof(c->fetched), val);
            // Anything else -- including the "# rank time steamid..." column
            // line -- is read into the reference's meta dict and then never
            // written back, because write_board only emits the six keys above.
            // Dropping it here is the same outcome by a shorter route.
            continue;
        }

        if (maxRows > 0 && c->count >= maxRows)
        {
            c->truncated = true;
            break;
        }

        // Exactly seven fields. An eighth and beyond are dropped, as
        // line.split("\t")[6] drops them.
        char *field[7];
        int nf = 0;
        char *p = line;
        for (;;)
        {
            field[nf++] = p;
            if (nf == 7)
                break;
            char *tab = strchr(p, '\t');
            if (!tab)
                break;
            *tab = '\0';
            p = tab + 1;
        }
        if (nf < 7)
            continue;
        {
            char *tab = strchr(field[6], '\t');
            if (tab)
                *tab = '\0';
        }

        long long rank = 0, epoch = 0;
        double time = 0.0;
        if (!ParseInt(field[0], &rank) || !ParseReal(field[1], &time) ||
            !ParseInt(field[5], &epoch))
            continue;           // the reference's except ValueError: continue

        WrBoardCacheRow r;
        memset(&r, 0, sizeof(r));
        r.rank = (int)rank;
        r.time = time;
        strncpy_s(r.steamId, sizeof(r.steamId), field[2], _TRUNCATE);
        strncpy_s(r.alias, sizeof(r.alias), field[3], _TRUNCATE);
        strncpy_s(r.hash, sizeof(r.hash), field[4], _TRUNCATE);
        r.epoch = epoch;
        strncpy_s(r.url, sizeof(r.url), field[6], _TRUNCATE);
        WrBoardCacheMerge(c, &r);
    }
    fclose(f);
    return true;
}

// Stable by rank, breaking ties by the order the rows were first seen. See the
// long comment in wr_board.h -- this three-line struct is the whole of it.
struct SortKey { int rank; int at; };

static int ByRankThenSeen(const void *a, const void *b)
{
    const SortKey *x = (const SortKey *)a, *y = (const SortKey *)b;
    if (x->rank != y->rank)
        return x->rank < y->rank ? -1 : 1;
    return x->at < y->at ? -1 : (x->at > y->at ? 1 : 0);
}

bool WrBoardWriteCache(const char *path, const WrBoardCache *c)
{
    SortKey *order = NULL;
    if (c->count > 0)
    {
        order = (SortKey *)malloc(sizeof(SortKey) * (size_t)c->count);
        if (!order)
        {
            WrLogf("[!] board: out of memory sorting %d rows", c->count);
            return false;
        }
        for (int i = 0; i < c->count; i++)
        {
            order[i].rank = c->rows[i].rank;
            order[i].at = i;
        }
        qsort(order, (size_t)c->count, sizeof(SortKey), ByRankThenSeen);
    }

    char tmp[MAX_PATH];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s.tmp", path);

    // Text mode, so every \n becomes \r\n. The reference writes with Python's
    // default newline translation and the readers on both sides open in text
    // mode, so this is not a preference -- a binary write here would put a
    // carriage return on the end of every url field.
    FILE *f = NULL;
    if (fopen_s(&f, tmp, "w") != 0 || !f)
    {
        free(order);
        WrLogf("[!] board: could not write %s", tmp);
        return false;
    }

    fprintf(f, "# WrLines leaderboard cache -- the windows you asked for, not "
               "the whole board.\n");
    // Fixed key order, and only the keys that are set. This is the reference's
    // ("map", "mapid", "gamemode", "track", "total", "fetched") tuple, which is
    // why it is a written-out list and not a loop over whatever we happen to
    // hold.
    if (c->map[0])      fprintf(f, "# map\t%s\n", c->map);
    if (c->mapId[0])    fprintf(f, "# mapid\t%s\n", c->mapId);
    if (c->gamemode[0]) fprintf(f, "# gamemode\t%s\n", c->gamemode);
    if (c->track[0])    fprintf(f, "# track\t%s\n", c->track);
    if (c->total[0])    fprintf(f, "# total\t%s\n", c->total);
    if (c->fetched[0])  fprintf(f, "# fetched\t%s\n", c->fetched);
    fprintf(f, "# rank\ttime\tsteamid\talias\thash\tepoch\turl\n");

    for (int i = 0; i < c->count; i++)
    {
        const WrBoardCacheRow *r = &c->rows[order ? order[i].at : i];
        fprintf(f, "%d\t%.6f\t%s\t%s\t%s\t%lld\t%s\n",
                r->rank, r->time, r->steamId, r->alias, r->hash,
                r->epoch, r->url);
    }

    bool wrote = (fclose(f) == 0);
    free(order);

    // Atomic, which the reference is too -- write_board is tmp + os.replace.
    if (!wrote || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
    {
        WrLogf("[!] board: could not replace %s", path);
        DeleteFileA(tmp);
        return false;
    }
    return true;
}

// --- what the table reads ---------------------------------------------------

int WrBoardParseFile(const char *path, WrBoardRow *out, int maxRows,
                     int *total, long long *fetched, int *mapId)
{
    if (total)   *total = 0;
    if (fetched) *fetched = 0;
    if (mapId)   *mapId = 0;

    WrBoardCache c;
    if (!WrBoardReadCache(path, &c, maxRows))
        return -1;

    if (total)   *total = atoi(c.total);
    if (fetched) *fetched = _atoi64(c.fetched);
    if (mapId)   *mapId = atoi(c.mapId);

    int n = 0;
    for (int i = 0; i < c.count && n < maxRows; i++)
    {
        const WrBoardCacheRow *s = &c.rows[i];

        // A row without a rank or a hash is not a row. The writer produces
        // neither as empty, so this only fires on a hand-edited file -- which
        // is exactly when quietly keeping half a record is worst. It is a
        // DISPLAY rule and not a format rule, which is why it is here and not
        // in the reader: the fetcher must round-trip such a row unchanged
        // rather than silently delete it from somebody's cache.
        if (s->rank <= 0 || !s->hash[0])
            continue;

        WrBoardRow r;
        memset(&r, 0, sizeof(r));
        r.rank = s->rank;
        r.time = (float)s->time;
        r.steamId = _strtoui64(s->steamId, NULL, 10);
        strncpy_s(r.alias, sizeof(r.alias), s->alias, _TRUNCATE);
        strncpy_s(r.hash, sizeof(r.hash), s->hash, _TRUNCATE);
        r.dateEpoch = s->epoch;
        strncpy_s(r.url, sizeof(r.url), s->url, _TRUNCATE);
        out[n++] = r;
    }

    WrBoardCacheFree(&c);
    return n;
}

// --- the reader thread ------------------------------------------------------

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
        WrBoardCachePath(path, sizeof(path), map, mode, type, num);

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
