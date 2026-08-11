// wr_peek.cpp  --  the demo -> map index. See wr_peek.h for why it exists.

#include "wr_peek.h"
#include "wr_mtv.h"
#include "wr_log.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The lock
// ---------------------------------------------------------------------------

// NOT the `if (!ready) { Initialize(); ready = true; }` shape the rest of this
// project uses. That one is fine everywhere it appears, because in every other
// case the first call comes from the UI thread before any worker exists. Here
// it does not: a map change starts CountThread and the panel can start the
// extractor's coordinator in the same breath, and whichever of the two gets
// here first is genuinely a race. Two threads running InitializeCriticalSection
// on one object is undefined, and the failure is a lock that does not lock.
static volatile LONG g_init = 0;        // 0 untouched, 1 in progress, 2 ready
static CRITICAL_SECTION g_cs;

static void EnsureCs(void)
{
    if (g_init == 2)
        return;
    if (InterlockedCompareExchange(&g_init, 1, 0) == 0)
    {
        InitializeCriticalSection(&g_cs);
        InterlockedExchange(&g_init, 2);
        return;
    }
    // Someone else is constructing it. Sleep(0) rather than a tight spin: this
    // happens once per process and costs a scheduler quantum at most.
    while (g_init != 2)
        Sleep(0);
}

// ---------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------

struct Entry
{
    char *path;                 // malloc'd, as given
    unsigned int hash;          // of the lowercased path
    long long size;
    long long mtime;            // FILETIME as an integer
    char map[65];               // "" means "not an MMTV file"
    bool ok;                    // did the header parse cleanly
};

static Entry *g_tab = NULL;
static int g_cap = 0;           // always a power of two, or 0
static int g_n = 0;
static bool g_loaded = false;
static bool g_dirty = false;
static bool g_full = false;     // said the "cache is full" line already
static int g_hits = 0, g_misses = 0;

static unsigned int HashPath(const char *s)
{
    // FNV-1a over the ASCII-lowercased bytes. Lowercased because Windows paths
    // are case-insensitive and the same demo reached through two spellings must
    // not become two rows.
    unsigned int h = 2166136261u;
    for (const char *p = s; *p; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        if (c == '/')
            c = '\\';           // and neither must the two separators
        h ^= c;
        h *= 16777619u;
    }
    return h ? h : 1u;          // 0 is the empty-slot marker
}

static bool SamePath(const char *a, const char *b)
{
    for (;; a++, b++)
    {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x == '/') x = '\\';
        if (y == '/') y = '\\';
        if (x != y)
            return false;
        if (!x)
            return true;
    }
}

// Where a path lives, or where it would go. Never fails: the table is kept
// below half full, so there is always an empty slot to stop at.
static int Slot(const char *path, unsigned int h)
{
    const unsigned int mask = (unsigned int)g_cap - 1;
    unsigned int i = h & mask;
    for (;;)
    {
        if (!g_tab[i].hash)
            return (int)i;
        if (g_tab[i].hash == h && SamePath(g_tab[i].path, path))
            return (int)i;
        i = (i + 1) & mask;
    }
}

static bool Grow(void)
{
    const int grown = g_cap ? g_cap * 2 : 1024;
    Entry *bigger = (Entry *)calloc((size_t)grown, sizeof(Entry));
    if (!bigger)
        return false;

    Entry *old = g_tab;
    const int oldCap = g_cap;
    g_tab = bigger;
    g_cap = grown;

    for (int i = 0; i < oldCap; i++)
    {
        if (!old[i].hash)
            continue;
        g_tab[Slot(old[i].path, old[i].hash)] = old[i];
    }
    free(old);
    return true;
}

static void Put(const char *path, long long size, long long mtime,
                const char *map, bool ok)
{
    // Half full at most. Open addressing degrades badly past about 0.7 and this
    // table is looked up once per demo per walk, six thousand times in a row.
    if ((g_n + 1) * 2 > g_cap)
    {
        if (!Grow())
            return;             // out of memory: stop caching, keep working
    }

    const unsigned int h = HashPath(path);
    const int i = Slot(path, h);

    if (!g_tab[i].hash)
    {
        if (g_n >= WR_PEEK_MAX)
        {
            if (!g_full)
            {
                g_full = true;
                WrLogf("[!] peek: the demo index is full at %d entries, so the "
                       "rest of this walk reads headers the slow way. Delete "
                       "%s to rebuild it.", WR_PEEK_MAX, "wrlines_data\\demoindex.txt");
            }
            return;
        }
        const size_t n = strlen(path) + 1;
        g_tab[i].path = (char *)malloc(n);
        if (!g_tab[i].path)
            return;
        memcpy(g_tab[i].path, path, n);
        g_tab[i].hash = h;
        g_n++;
    }

    g_tab[i].size = size;
    g_tab[i].mtime = mtime;
    strncpy_s(g_tab[i].map, sizeof(g_tab[i].map), map ? map : "", _TRUNCATE);
    g_tab[i].ok = ok;
    g_dirty = true;
}

// ---------------------------------------------------------------------------
// The file
// ---------------------------------------------------------------------------

// Resolved once and copied immediately: WrDataPath hands back one of four
// rotating static buffers with no lock, and two threads can be in here.
static void IndexPath(char *out, int cap)
{
    strcpy_s(out, (size_t)cap, WrDataPath("demoindex.txt"));
}

static void Load(void)
{
    if (g_loaded)
        return;
    g_loaded = true;

    char path[MAX_PATH];
    IndexPath(path, sizeof(path));

    FILE *f = NULL;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return;                 // no index yet, which is not a problem

    char line[MAX_PATH + 160];
    while (fgets(line, (int)sizeof(line), f))
    {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;

        // size <tab> mtime <tab> ok <tab> map <tab> path
        //
        // The path is LAST on purpose. It is the one field that could contain
        // anything, and taking "the rest of the line" means a tab in a demo's
        // name costs nothing. A short or malformed line is dropped rather than
        // repaired -- it is a cache, and the repair is one file open.
        char *p = line;
        char *tabs[4];
        int nt = 0;
        for (char *q = line; *q && nt < 4; q++)
            if (*q == '\t')
                tabs[nt++] = q;
        if (nt < 4)
            continue;
        for (int i = 0; i < 4; i++)
            *tabs[i] = '\0';

        const long long size = _strtoi64(p, NULL, 10);
        const long long mtime = _strtoi64(tabs[0] + 1, NULL, 10);
        const bool ok = (tabs[1] + 1)[0] == '1';
        const char *map = tabs[2] + 1;
        char *fp = tabs[3] + 1;

        size_t len = strlen(fp);
        while (len && (fp[len - 1] == '\n' || fp[len - 1] == '\r'))
            fp[--len] = '\0';
        if (!len)
            continue;

        Put(fp, size, mtime, map, ok);
    }
    fclose(f);

    // Loading is not a change. Without this, a run that hit the cache for every
    // single demo would still rewrite the file on the way out.
    g_dirty = false;
}

void WrPeekSave(void)
{
    EnsureCs();
    EnterCriticalSection(&g_cs);

    if (!g_dirty || !g_tab)
    {
        LeaveCriticalSection(&g_cs);
        return;
    }

    char path[MAX_PATH];
    IndexPath(path, sizeof(path));

    char tmp[MAX_PATH];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s.tmp", path);

    FILE *f = NULL;
    if (fopen_s(&f, tmp, "wb") != 0 || !f)
    {
        LeaveCriticalSection(&g_cs);
        WrLogf("[!] peek: could not write %s", tmp);
        return;
    }

    fprintf(f,
        "# WrLines demo index. Written automatically; safe to delete.\n"
        "#\n"
        "# Which map each of your .mtv demos is for, so that finding the ones\n"
        "# for a map does not mean opening every file every time. Deleting this\n"
        "# costs one slow run and nothing else.\n"
        "#\n"
        "# A row is believed only while the file's size and last-write time\n"
        "# still match, so a re-downloaded demo is re-read rather than trusted.\n"
        "#\n"
        "# size <tab> mtime <tab> header-ok <tab> map <tab> path\n"
        "#\n"
        "# NOT display settings, unlike settings.cfg: these are paths to demo\n"
        "# files on this machine, and the map names of other players' runs.\n"
        "# It lives under wrlines_data for that reason and should not be shared.\n"
        "\n");

    int written = 0;
    for (int i = 0; i < g_cap; i++)
    {
        const Entry *e = &g_tab[i];
        if (!e->hash)
            continue;
        // A tab inside a map name would produce a row that reads back as a
        // different shape. Vanishingly unlikely and cheap to refuse: the row is
        // simply not written and the demo is peeked again next time.
        if (strchr(e->map, '\t') || strchr(e->map, '\n'))
            continue;
        fprintf(f, "%lld\t%lld\t%d\t%s\t%s\n", e->size, e->mtime,
                e->ok ? 1 : 0, e->map, e->path);
        written++;
    }
    fclose(f);

    if (MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
        g_dirty = false;
    else
    {
        DeleteFileA(tmp);
        WrLogf("[!] peek: could not replace %s", path);
    }

    LeaveCriticalSection(&g_cs);
    (void)written;
}

// ---------------------------------------------------------------------------
// The one question this module answers
// ---------------------------------------------------------------------------

bool WrPeekMapOf(const char *path, long long size, long long mtime,
                 char *mapOut, int mapCap, bool *ok)
{
    if (mapOut && mapCap > 0)
        mapOut[0] = '\0';
    if (ok)
        *ok = false;
    if (!path || !*path)
        return false;

    EnsureCs();
    EnterCriticalSection(&g_cs);
    Load();

    if (g_cap)
    {
        const unsigned int h = HashPath(path);
        const int i = Slot(path, h);
        if (g_tab[i].hash && g_tab[i].size == size && g_tab[i].mtime == mtime)
        {
            if (mapOut && mapCap > 0)
                strncpy_s(mapOut, (size_t)mapCap, g_tab[i].map, _TRUNCATE);
            if (ok)
                *ok = g_tab[i].ok;
            g_hits++;
            LeaveCriticalSection(&g_cs);
            return false;
        }
    }

    g_misses++;
    LeaveCriticalSection(&g_cs);

    // OUTSIDE the lock. This is the twenty-millisecond part on a cold disk, and
    // holding the lock across it would serialise the two walkers into one --
    // turning a cache into a queue, which is worse than not having it.
    //
    // The cost of letting two threads peek the same file at once is that one of
    // them repeats work already done. The cost of the alternative is that the
    // counter and the extractor take turns. This is the cheaper mistake.
    WrMtvHeader hdr;
    char why[128];
    const bool parsed = WrMtvPeek(path, &hdr, why, sizeof(why));

    EnterCriticalSection(&g_cs);
    Put(path, size, mtime, hdr.map, parsed);
    LeaveCriticalSection(&g_cs);

    if (mapOut && mapCap > 0)
        strncpy_s(mapOut, (size_t)mapCap, hdr.map, _TRUNCATE);
    if (ok)
        *ok = parsed;
    return true;
}

// ---------------------------------------------------------------------------
// Housekeeping
// ---------------------------------------------------------------------------

int WrPeekCount(void)
{
    EnsureCs();
    EnterCriticalSection(&g_cs);
    const int n = g_n;
    LeaveCriticalSection(&g_cs);
    return n;
}

void WrPeekStats(int *hits, int *misses)
{
    EnsureCs();
    EnterCriticalSection(&g_cs);
    if (hits) *hits = g_hits;
    if (misses) *misses = g_misses;
    LeaveCriticalSection(&g_cs);
}

static void FreeTableLocked(void)
{
    for (int i = 0; i < g_cap; i++)
        if (g_tab[i].hash)
            free(g_tab[i].path);
    free(g_tab);
    g_tab = NULL;
    g_cap = 0;
    g_n = 0;
}

void WrPeekForget(void)
{
    EnsureCs();
    EnterCriticalSection(&g_cs);

    char path[MAX_PATH];
    IndexPath(path, sizeof(path));
    DeleteFileA(path);

    FreeTableLocked();
    g_loaded = false;
    g_dirty = false;
    g_full = false;
    g_hits = g_misses = 0;

    LeaveCriticalSection(&g_cs);
}

void WrPeekShutdown(void)
{
    if (g_init != 2)
        return;
    EnterCriticalSection(&g_cs);
    FreeTableLocked();
    g_loaded = false;
    g_dirty = false;
    LeaveCriticalSection(&g_cs);
}
