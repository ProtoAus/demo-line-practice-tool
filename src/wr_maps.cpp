// wr_maps.cpp  --  see wr_maps.h.

#include "wr_maps.h"
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

void WrMapsShutdown(void) {}
