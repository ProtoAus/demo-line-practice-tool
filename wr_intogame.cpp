// wr_intogame.cpp  --  see wr_intogame.h.

#include "wr_intogame.h"
#include "wr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Ten is the game's own list length, so a few dozen covers wanting a shortlist
// per map across a session. A cap at all because this is a fixed array read on
// the UI thread and an unbounded one would be a growable buffer for no gain.
#define WR_INTOGAME_MAX 256

struct Entry
{
    int mapId;
    char hash[48];
    char map[72];
};

static Entry g_entries[WR_INTOGAME_MAX];
static int g_count = 0;
static bool g_loaded = false;
static char g_status[192] = {0};

// ---------------------------------------------------------------------------
// The manifest
// ---------------------------------------------------------------------------

static const char *ManifestPath(void)
{
    return WrDataPath("into_game.txt");
}

static bool HashLooksSane(const char *hash)
{
    // A replay hash is a 40-character hex SHA1, and it becomes a FILENAME. This
    // is the guard that stops anything else ever reaching a path we build: no
    // separators, no dots, no traversal, nothing that is not hex.
    if (!hash)
        return false;
    int n = 0;
    for (const char *p = hash; *p; p++, n++)
    {
        if (n >= 44)
            return false;
        bool hex = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
                   (*p >= 'A' && *p <= 'F');
        if (!hex)
            return false;
    }
    return n >= 8;
}

static void Save(void)
{
    FILE *f = NULL;
    if (fopen_s(&f, ManifestPath(), "wb") != 0 || !f)
    {
        WrLogf("[!] into-game: could not write %s", ManifestPath());
        return;
    }
    fprintf(f, "# demos WrLines copied into the game's replay folder.\n");
    fprintf(f, "# Only files listed here are ever removed by the panel's\n");
    fprintf(f, "# \"remove ours\" button. Anything the game downloaded itself is\n");
    fprintf(f, "# not in this file and cannot be reached from there.\n");
    fprintf(f, "# mapid\\thash\\tmap\n");
    for (int i = 0; i < g_count; i++)
        fprintf(f, "%d\t%s\t%s\n", g_entries[i].mapId, g_entries[i].hash,
                g_entries[i].map);
    fclose(f);
}

static void Load(void)
{
    g_count = 0;
    g_loaded = true;

    FILE *f = NULL;
    if (fopen_s(&f, ManifestPath(), "rb") != 0 || !f)
        return;                 // no manifest is the normal first-run state

    char line[512];
    while (fgets(line, sizeof(line), f) && g_count < WR_INTOGAME_MAX)
    {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        char *tab1 = strchr(line, '\t');
        if (!tab1)
            continue;
        *tab1 = '\0';
        char *tab2 = strchr(tab1 + 1, '\t');
        if (!tab2)
            continue;
        *tab2 = '\0';
        char *end = tab2 + 1;
        for (char *p = end; *p; p++)
            if (*p == '\r' || *p == '\n') { *p = '\0'; break; }

        int id = atoi(line);
        if (id <= 0 || !HashLooksSane(tab1 + 1))
            continue;
        g_entries[g_count].mapId = id;
        strcpy_s(g_entries[g_count].hash, sizeof(g_entries[g_count].hash),
                 tab1 + 1);
        // TRUNCATE, not strcpy_s. This field comes off a line of a text file
        // that a user can edit and that a half-written line can corrupt, and it
        // can be four hundred characters long. strcpy_s does not truncate an
        // over-long source -- it calls the invalid parameter handler, which
        // ends the process. Inside somebody else's game.
        strncpy_s(g_entries[g_count].map, sizeof(g_entries[g_count].map), end,
                  _TRUNCATE);
        g_count++;
    }
    fclose(f);
}

static void EnsureLoaded(void)
{
    if (!g_loaded)
        Load();
}

static int Find(int mapId, const char *hash)
{
    for (int i = 0; i < g_count; i++)
        if (g_entries[i].mapId == mapId && _stricmp(g_entries[i].hash, hash) == 0)
            return i;
    return -1;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

bool WrIntoGamePath(int mapId, const char *hash, char *out, int outLen)
{
    if (mapId <= 0 || !HashLooksSane(hash) || !out || outLen < 8)
        return false;
    const char *game = WrGameDir();
    if (!game || !*game)
        return false;
    _snprintf_s(out, (size_t)outLen, _TRUNCATE,
                "%s\\momentum\\momtv\\online\\%d\\%s.mtv", game, mapId, hash);
    return true;
}

bool WrIntoGameHasFile(int mapId, const char *hash)
{
    char path[MAX_PATH];
    if (!WrIntoGamePath(mapId, hash, path, sizeof(path)))
        return false;
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

bool WrIntoGameFindSource(const char *map, int mapId, const char *hash,
                          char *out, int outLen)
{
    if (!HashLooksSane(hash) || !out || outLen < 8)
        return false;

    // Ours first. It is the copy we know the provenance of, and on a machine
    // where the game has also downloaded the same run the two are identical
    // anyway -- the hash is the filename because the hash is the content.
    char rel[MAX_PATH];
    if (map && *map)
    {
        _snprintf_s(rel, sizeof(rel), _TRUNCATE, "demos\\%s\\%s.mtv", map, hash);
        const char *ours = WrDataPath(rel);
        if (ours && GetFileAttributesA(ours) != INVALID_FILE_ATTRIBUTES)
        {
            strcpy_s(out, (size_t)outLen, ours);
            return true;
        }
    }

    const char *game = WrGameDir();
    if (!game || !*game)
        return false;

    if (mapId > 0)
    {
        _snprintf_s(out, (size_t)outLen, _TRUNCATE,
                    "%s\\momentum\\momtv\\online\\%d\\%s.mtv", game, mapId, hash);
        if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES)
            return true;
    }
    if (map && *map)
    {
        _snprintf_s(out, (size_t)outLen, _TRUNCATE,
                    "%s\\momentum\\momtv\\local\\%s\\%s.mtv", game, map, hash);
        if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES)
            return true;
    }
    out[0] = '\0';
    return false;
}

// ---------------------------------------------------------------------------

static bool MakeTree(const char *dir)
{
    // CreateDirectory one level at a time, which is the whole of mkdir -p here
    // because the parents all exist in a real install.
    char tmp[MAX_PATH];
    strcpy_s(tmp, sizeof(tmp), dir);
    for (char *p = tmp + 3; *p; p++)
    {
        if (*p != '\\')
            continue;
        *p = '\0';
        CreateDirectoryA(tmp, NULL);
        *p = '\\';
    }
    CreateDirectoryA(tmp, NULL);
    return GetFileAttributesA(tmp) != INVALID_FILE_ATTRIBUTES;
}

bool WrIntoGameSend(const char *map, int mapId, const char *hash)
{
    EnsureLoaded();

    char dst[MAX_PATH];
    if (!WrIntoGamePath(mapId, hash, dst, sizeof(dst)))
    {
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "no numeric map id for \"%s\", so there is no folder to put "
                    "it in -- refresh the map list", map ? map : "?");
        return false;
    }

    if (WrIntoGameHasFile(mapId, hash))
    {
        // Already there. Record it only if it was us; a demo the game
        // downloaded must not become ours to delete just because it was asked
        // for.
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "the game already has that one");
        return true;
    }

    char src[MAX_PATH];
    if (!WrIntoGameFindSource(map, mapId, hash, src, sizeof(src)))
    {
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "no .mtv on disk for that run -- fetch it first");
        return false;
    }

    if (g_count >= WR_INTOGAME_MAX)
    {
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "%d already sent; remove some before adding more",
                    WR_INTOGAME_MAX);
        return false;
    }

    char dir[MAX_PATH];
    strcpy_s(dir, sizeof(dir), dst);
    char *slash = strrchr(dir, '\\');
    if (slash)
        *slash = '\0';
    if (!MakeTree(dir))
    {
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "could not create %s", dir);
        return false;
    }

    // Recorded BEFORE the copy. If this crashes between the two lines the file
    // is still ours to remove; the other order leaves a file nothing knows
    // about, in a directory we have promised not to guess about.
    if (Find(mapId, hash) < 0)
    {
        g_entries[g_count].mapId = mapId;
        strcpy_s(g_entries[g_count].hash, sizeof(g_entries[g_count].hash), hash);
        strcpy_s(g_entries[g_count].map, sizeof(g_entries[g_count].map),
                 map ? map : "");
        g_count++;
        Save();
    }

    if (!CopyFileA(src, dst, FALSE))
    {
        DWORD err = GetLastError();
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "copy failed (%lu) -- is the game holding the folder open?",
                    (unsigned long)err);
        WrLogf("[!] into-game: CopyFile %s -> %s failed, %lu", src, dst,
               (unsigned long)err);
        return false;
    }

    WrLogf("into-game: %s -> %s", src, dst);
    _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                "sent; %d of ours in the game's folder", WrIntoGameCount());
    return true;
}

void WrIntoGameRefresh(void)
{
    EnsureLoaded();

    // Throttled, because the panel calls this every frame it is open and the
    // work is one GetFileAttributes per entry -- up to 256 file-system round
    // trips a frame, and under Proton every one of them is translated. Half a
    // second is far faster than a game cache clear, which is the only thing
    // this is watching for.
    static DWORD lastAt = 0;
    DWORD now = GetTickCount();
    if (lastAt && now - lastAt < 500)
        return;
    lastAt = now;

    // Drop anything whose file has gone. A game cache clear takes our copies
    // with everything else, and a count that still claimed them would be a lie
    // about the ten-slot budget the whole feature is about.
    int kept = 0;
    bool changed = false;
    for (int i = 0; i < g_count; i++)
    {
        if (WrIntoGameHasFile(g_entries[i].mapId, g_entries[i].hash))
            g_entries[kept++] = g_entries[i];
        else
            changed = true;
    }
    g_count = kept;
    if (changed)
        Save();
}

int WrIntoGameCount(void)
{
    EnsureLoaded();
    return g_count;
}

bool WrIntoGameMine(int mapId, const char *hash)
{
    EnsureLoaded();
    return HashLooksSane(hash) && Find(mapId, hash) >= 0;
}

int WrIntoGameRemoveOne(int mapId, const char *hash)
{
    EnsureLoaded();
    int at = Find(mapId, hash);
    if (at < 0)
        return 0;               // not ours; not ours to delete

    char path[MAX_PATH];
    int gone = 0;
    if (WrIntoGamePath(mapId, hash, path, sizeof(path)))
    {
        if (DeleteFileA(path) ||
            GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            gone = 1;
        else
            WrLogf("[!] into-game: could not delete %s (%lu)", path,
                   (unsigned long)GetLastError());
    }
    if (gone)
    {
        for (int i = at; i + 1 < g_count; i++)
            g_entries[i] = g_entries[i + 1];
        g_count--;
        Save();
    }
    return gone;
}

int WrIntoGameRemoveAll(void)
{
    EnsureLoaded();
    int gone = 0;
    // Backwards, so the compaction inside RemoveOne cannot skip an entry.
    for (int i = g_count - 1; i >= 0; i--)
        gone += WrIntoGameRemoveOne(g_entries[i].mapId, g_entries[i].hash);
    _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                "removed %d of ours; the game's own downloads are untouched",
                gone);
    WrLogf("into-game: removed %d of our copies", gone);
    return gone;
}

const char *WrIntoGameStatus(void)
{
    return g_status;
}

void WrIntoGameShutdown(void)
{
    g_count = 0;
    g_loaded = false;
}
