// wr_maps.cpp  --  see wr_maps.h.

#include "wr_maps.h"
#include "wr_msml.h"
#include "wr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MAPS 4096

static WrMapInfo *g_maps = NULL;
static int g_count = 0;
static bool g_ready = false;
static char g_status[192] = "not read yet";
static volatile LONG g_busy = 0;
static CRITICAL_SECTION g_cs;
static bool g_csReady = false;

static void EnsureCs(void)
{
    if (!g_csReady)
    {
        InitializeCriticalSection(&g_cs);
        g_csReady = true;
    }
}

// Count .mtv files for one map across both trees, and .wrpath files for it.
//
// The demo trees are keyed differently and that is the whole reason this is not
// one walk: the game files sit under momtv\online\<mapID>\ and momtv\local\
// <mapname>\, and ours under wrlines_data\demos\<mapname>\. So a map is looked
// up by id in one place and by name in two others.
static int CountFiles(const char *dir, const char *ext)
{
    char pat[MAX_PATH];
    _snprintf_s(pat, sizeof(pat), _TRUNCATE, "%s\\*%s", dir, ext);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    int n = 0;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

static void CountForMap(WrMapInfo *m)
{
    char dir[MAX_PATH];
    m->demos = 0;

    if (m->id > 0)
    {
        _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\momentum\\momtv\\online\\%d",
                    WrGameDir(), m->id);
        m->demos += CountFiles(dir, ".mtv");
    }
    _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\momentum\\momtv\\local\\%s",
                WrGameDir(), m->name);
    m->demos += CountFiles(dir, ".mtv");

    char rel[MAX_PATH];
    _snprintf_s(rel, sizeof(rel), _TRUNCATE, "demos\\%s", m->name);
    m->demos += CountFiles(WrDataPath(rel), ".mtv");

    _snprintf_s(rel, sizeof(rel), _TRUNCATE, "paths\\%s", m->name);
    m->extracted = CountFiles(WrDataPath(rel), ".wrpath");
}

// ---------------------------------------------------------------------------
// Writing the index
// ---------------------------------------------------------------------------

static int __cdecl ByName(const void *a, const void *b)
{
    // strcmp, not _stricmp. The reference sorts Python strings, which compares
    // by code point, and UTF-8 byte order is code point order -- so this is the
    // same ordering for any name, not just the ASCII ones we actually see.
    return strcmp(((const WrMsmlMap *)a)->name, ((const WrMsmlMap *)b)->name);
}

// wrlines_data\tracks.txt. See the essay in wr_maps.h for why this is a second
// file and not two more columns of maps.txt.
//
// Maps with neither a stage nor a bonus are LEFT OUT. Most of the catalogue is
// single-track, so writing them all would triple the file to say "nothing" two
// thousand times, and a name that is absent already means what an absent name
// should mean. 2050 maps come to about 8 KB this way.
static void WriteTracks(const WrMsmlMap *cat, int n)
{
    const char *path = WrDataPath("tracks.txt");
    char tmp[MAX_PATH];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s.tmp", path);

    FILE *f = NULL;
    if (fopen_s(&f, tmp, "w") != 0 || !f)
    {
        WrLogf("[!] maps: could not write %s", tmp);
        return;
    }

    fprintf(f, "# WrLines track index, from the game's own _cache. No network.\n");
    fprintf(f, "# Maps with no stages and no bonuses are omitted.\n");
    fprintf(f, "# name\tstages\tbonuses\n");
    int written = 0;
    for (int i = 0; i < n; i++)
    {
        if (!cat[i].stages && !cat[i].bonuses)
            continue;
        fprintf(f, "%s\t%d\t%d\n", cat[i].name, (int)cat[i].stages,
                (int)cat[i].bonuses);
        written++;
    }

    if (fclose(f) != 0 || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
    {
        WrLogf("[!] maps: could not replace %s", path);
        DeleteFileA(tmp);
        return;
    }
    WrLogf("maps: %d of %d maps have stages or bonuses -> %s", written, n, path);
    WrMapsTracksReload();
}

int WrMapsWriteIndex(const char *gameDir, WrMapsEmitFn emit)
{
    char msg[256];

    WrMsmlMap *cat = (WrMsmlMap *)malloc(sizeof(WrMsmlMap) * MAX_MAPS);
    if (!cat)
        return -1;

    int skipped = 0;
    int n = WrMsmlRead(gameDir, cat, MAX_MAPS, &skipped);
    if (n <= 0)
    {
        free(cat);
        if (emit)
        {
            char dir[MAX_PATH];
            WrMsmlCacheDir(gameDir, dir, sizeof(dir));
            _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                        "[!] no map cache found under %s", dir);
            emit(msg);
            emit("    Momentum writes it when it fetches the map list; open the");
            emit("    map selector in game once and it will appear.");
        }
        return -1;
    }

    qsort(cat, (size_t)n, sizeof(WrMsmlMap), ByName);

    const char *path = WrDataPath("maps.txt");
    char tmp[MAX_PATH];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s.tmp", path);

    // Text mode, so \n becomes \r\n. The reference writes with Python's default
    // text mode and the reader above opens "r", so a binary write here would
    // leave every last field carrying a stray \r -- and the parity check would
    // fail on 2050 lines at once, which is at least a loud way to find out.
    //
    // Atomic, which the reference is NOT for this one file: it writes maps.txt
    // in place, so an interrupted rebuild leaves a half index that reads as a
    // short one. A deliberate divergence, and it cannot affect parity because
    // the bytes that land are identical either way.
    FILE *f = NULL;
    if (fopen_s(&f, tmp, "w") != 0 || !f)
    {
        free(cat);
        WrLogf("[!] maps: could not write %s", tmp);
        if (emit)
            emit("could not write the index");
        return -1;
    }

    fprintf(f, "# WrLines map index, from the game's own _cache. No network.\n");
    fprintf(f, "# id\tname\ttier\tapproved\tmodes\n");
    for (int i = 0; i < n; i++)
    {
        const WrMsmlMap *m = &cat[i];

        // The modes, ascending, comma separated. A set in the reference; a
        // bitmask here, and walking a bitmask low to high IS sorted order.
        char modes[96];
        int used = 0;
        modes[0] = '\0';
        for (int b = 0; b < 32; b++)
        {
            if (!(m->modes & (1u << b)))
                continue;
            used += _snprintf_s(modes + used, sizeof(modes) - used, _TRUNCATE,
                                "%s%d", used ? "," : "", b);
            if (used >= (int)sizeof(modes) - 4)
                break;
        }
        fprintf(f, "%d\t%s\t%d\t%d\t%s\n", m->id, m->name, m->tier,
                m->approved ? 1 : 0, modes);
    }
    bool wrote = (fclose(f) == 0);

    // The second file, out of the same pass. Silent by design -- it emits
    // nothing, so `wrextract --index-maps` prints exactly what it printed
    // before -- and its failure is logged rather than fatal, because a missing
    // tracks.txt costs the quick menu a guess and costs the map index nothing.
    WriteTracks(cat, n);
    free(cat);

    if (!wrote || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
    {
        WrLogf("[!] maps: could not replace %s", path);
        DeleteFileA(tmp);
        if (emit)
            emit("could not replace the index");
        return -1;
    }

    if (emit)
    {
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "indexed %d maps -> %s", n, path);
        emit(msg);
    }
    WrLogf("maps: indexed %d maps (%d cache files unreadable)", n, skipped);
    return n;
}

// ---------------------------------------------------------------------------
// Reading it back
// ---------------------------------------------------------------------------

static DWORD WINAPI ReadThread(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    WrMapInfo *table = (WrMapInfo *)calloc(MAX_MAPS, sizeof(WrMapInfo));
    int n = 0;
    bool haveIndex = false;

    FILE *f = NULL;
    if (table && fopen_s(&f, WrDataPath("maps.txt"), "r") == 0 && f)
    {
        haveIndex = true;
        char line[512];
        while (n < MAX_MAPS && fgets(line, sizeof(line), f))
        {
            if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
                continue;
            int id = 0, tier = 0, approved = 0;
            char name[72] = {0};
            // id \t name \t tier \t approved \t modes
            if (sscanf_s(line, "%d\t%71[^\t]\t%d\t%d", &id, name,
                         (unsigned)sizeof(name), &tier, &approved) < 4)
                continue;
            table[n].id = id;
            strcpy_s(table[n].name, sizeof(table[n].name), name);
            table[n].tier = tier;
            table[n].approved = (approved != 0);
            n++;
        }
        fclose(f);
    }

    if (table)
        for (int i = 0; i < n; i++)
            CountForMap(&table[i]);

    EnterCriticalSection(&g_cs);
    free(g_maps);
    g_maps = table;
    g_count = table ? n : 0;
    if (!haveIndex)
        strcpy_s(g_status, sizeof(g_status),
                 "no map index yet -- press Rebuild, which reads the game's own "
                 "cache and asks nothing of the network");
    else
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "%d maps in the index", n);
    g_ready = true;
    LeaveCriticalSection(&g_cs);

    InterlockedExchange(&g_busy, 0);
    return 0;
}

void WrMapsRefresh(void)
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

bool WrMapsReady(void) { return g_ready; }
const char *WrMapsStatus(void) { return g_status; }
int WrMapsCount(void) { return g_count; }

const WrMapInfo *WrMapsAt(int index)
{
    if (!g_maps || index < 0 || index >= g_count)
        return NULL;
    return &g_maps[index];
}

int WrMapsFind(const char *name)
{
    if (!g_maps || !name || !*name)
        return -1;
    for (int i = 0; i < g_count; i++)
        if (_stricmp(g_maps[i].name, name) == 0)
            return i;
    return -1;
}

// ---------------------------------------------------------------------------
// tracks.txt
// ---------------------------------------------------------------------------
//
// Read once, lazily, into a flat array. It is a few thousand rows of two small
// numbers, so a linear search is a few microseconds and happens when the map
// changes rather than per frame -- an index would be more code than the whole
// file for no measurable difference.
//
// Its own lock, deliberately not g_cs: this is read from the render thread while
// the map index's background reader may be holding that one, and one lock across
// two unrelated tables is how a UI thread ends up waiting on a directory walk.

struct TrackRow { char name[72]; unsigned char stages, bonuses; };

static TrackRow *g_tracks = NULL;
static int g_trackCount = 0;
static bool g_tracksRead = false;
static CRITICAL_SECTION g_trackCs;
static volatile LONG g_trackCsInit = 0;

static void EnsureTrackCs(void)
{
    if (g_trackCsInit == 2)
        return;
    if (InterlockedCompareExchange(&g_trackCsInit, 1, 0) == 0)
    {
        InitializeCriticalSection(&g_trackCs);
        InterlockedExchange(&g_trackCsInit, 2);
        return;
    }
    while (g_trackCsInit != 2)
        Sleep(0);
}

// Caller holds the lock.
static void LoadTracksLocked(void)
{
    free(g_tracks);
    g_tracks = NULL;
    g_trackCount = 0;
    g_tracksRead = false;

    FILE *f = NULL;
    if (fopen_s(&f, WrDataPath("tracks.txt"), "r") != 0 || !f)
        return;

    int cap = 256;
    TrackRow *rows = (TrackRow *)malloc(sizeof(TrackRow) * (size_t)cap);
    int n = 0;

    char line[256];
    while (rows && fgets(line, sizeof(line), f))
    {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        char name[72] = {0};
        int stages = 0, bonuses = 0;
        if (sscanf_s(line, "%71[^\t]\t%d\t%d", name, (unsigned)sizeof(name),
                     &stages, &bonuses) < 3)
            continue;
        if (!name[0])
            continue;
        if (n == cap)
        {
            cap *= 2;
            TrackRow *bigger = (TrackRow *)realloc(rows, sizeof(TrackRow) * (size_t)cap);
            if (!bigger)
                break;
            rows = bigger;
        }
        strcpy_s(rows[n].name, sizeof(rows[n].name), name);
        rows[n].stages = (unsigned char)(stages < 0 ? 0 : stages > 255 ? 255 : stages);
        rows[n].bonuses = (unsigned char)(bonuses < 0 ? 0 : bonuses > 255 ? 255 : bonuses);
        n++;
    }
    fclose(f);

    g_tracks = rows;
    g_trackCount = rows ? n : 0;
    g_tracksRead = true;
}

void WrMapsTracksReload(void)
{
    EnsureTrackCs();
    EnterCriticalSection(&g_trackCs);
    LoadTracksLocked();
    LeaveCriticalSection(&g_trackCs);
}

bool WrMapsTracksKnown(void)
{
    EnsureTrackCs();
    EnterCriticalSection(&g_trackCs);
    if (!g_tracksRead)
        LoadTracksLocked();
    bool known = g_tracksRead;
    LeaveCriticalSection(&g_trackCs);
    return known;
}

bool WrMapsTracksFor(const char *map, int *stages, int *bonuses)
{
    if (stages)  *stages = 0;
    if (bonuses) *bonuses = 0;
    if (!map || !*map)
        return false;

    EnsureTrackCs();
    EnterCriticalSection(&g_trackCs);
    if (!g_tracksRead)
        LoadTracksLocked();

    bool known = g_tracksRead;
    for (int i = 0; i < g_trackCount; i++)
    {
        if (_stricmp(g_tracks[i].name, map) != 0)
            continue;
        if (stages)  *stages = g_tracks[i].stages;
        if (bonuses) *bonuses = g_tracks[i].bonuses;
        break;
    }
    LeaveCriticalSection(&g_trackCs);

    // True means the FILE was read, not that this map was in it -- a
    // single-track map is deliberately absent and 0/0 is its correct answer.
    return known;
}

void WrMapsShutdown(void) {}
