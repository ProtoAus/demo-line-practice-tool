// wr_intogame.cpp  --  see wr_intogame.h.

#include "wr_intogame.h"
#include "wr_log.h"

#include <stdio.h>
#include <stdarg.h>
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
    WrIntoGameWhere where;
};

// Manifest format version.
//
// 1 was three tab-separated fields, mapid/hash/map, and every entry meant the
// online tree because that was the only tree this could write. A line with three
// fields still loads and still means exactly that, so nothing has to be
// migrated -- which is just as well, since the file on the machine this was
// written on had a version 1 header and no entries at all.
#define WR_INTOGAME_MANIFEST_VERSION 2

static Entry g_entries[WR_INTOGAME_MAX];
static int g_count = 0;
static bool g_loaded = false;
static char g_status[384] = {0};

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
    fprintf(f, "# wrlines-intogame %d\n", WR_INTOGAME_MANIFEST_VERSION);
    fprintf(f, "# demos WrLines copied into the game's replay folder.\n");
    fprintf(f, "# Only files listed here are ever removed by the panel's\n");
    fprintf(f, "# \"remove ours\" button. Anything the game downloaded itself is\n");
    fprintf(f, "# not in this file and cannot be reached from there.\n");
    fprintf(f, "# mapid\\thash\\tmap\\twhere   (where: online | local)\n");
    for (int i = 0; i < g_count; i++)
        fprintf(f, "%d\t%s\t%s\t%s\n", g_entries[i].mapId, g_entries[i].hash,
                g_entries[i].map,
                g_entries[i].where == WR_INTO_LOCAL ? "local" : "online");
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

        // The fourth field is optional: a version 1 line has three, and three
        // means the online tree, which is the only place that version could
        // write. Split before the newline scrub so an absent field and an empty
        // one read the same.
        WrIntoGameWhere where = WR_INTO_ONLINE;
        char *tab3 = strchr(end, '\t');
        if (tab3)
        {
            *tab3 = '\0';
            if (_strnicmp(tab3 + 1, "local", 5) == 0)
                where = WR_INTO_LOCAL;
        }
        for (char *p = end; *p; p++)
            if (*p == '\r' || *p == '\n') { *p = '\0'; break; }

        int id = atoi(line);
        if (id <= 0 || !HashLooksSane(tab1 + 1))
            continue;
        g_entries[g_count].where = where;
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

static int Find(int mapId, const char *hash, WrIntoGameWhere where)
{
    for (int i = 0; i < g_count; i++)
        if (g_entries[i].mapId == mapId && g_entries[i].where == where &&
            _stricmp(g_entries[i].hash, hash) == 0)
            return i;
    return -1;
}

static int FindAny(int mapId, const char *hash)
{
    for (int i = 0; i < g_count; i++)
        if (g_entries[i].mapId == mapId && _stricmp(g_entries[i].hash, hash) == 0)
            return i;
    return -1;
}

// Our own fetched copy. Its existence is the proof of ownership the adoption
// rule turns on: the filename is the replay hash and the hash is the content, so
// a file of that name at the destination is a copy of this one.
static const char *OursPath(const char *map, const char *hash)
{
    if (!map || !*map || !HashLooksSane(hash))
        return NULL;
    char rel[MAX_PATH];
    _snprintf_s(rel, sizeof(rel), _TRUNCATE, "demos\\%s\\%s.mtv", map, hash);
    return WrDataPath(rel);
}

static bool FileThere(const char *path)
{
    return path && *path && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static bool Stat(const char *path, WIN32_FILE_ATTRIBUTE_DATA *fad)
{
    return path && *path &&
           GetFileAttributesExA(path, GetFileExInfoStandard, fad) != 0;
}

// Is the file at `dst` a copy WE made of `ours`?
//
// "Our tree holds a file with that hash" is NOT the answer, and getting this
// wrong is the one mistake this whole module exists to avoid. The hash is the
// content, so two files with the same name are byte-identical whoever fetched
// them -- and the game can download a run we already have, at any time, after
// which "we hold it too" is true of a file the game put there. Adopting on that
// basis would let "Remove ours" delete the game's own download, which is
// exactly the promise the manifest is for.
//
// Size AND last-write time, to the tick, is an answer. Every copy this makes is
// a CopyFile, and every copy the fetcher makes is a shutil.copy2 from our own
// file -- both carry the source's write time across unchanged. The game writes
// its downloads when it downloaded them, which is some other instant entirely;
// matching a 100-nanosecond FILETIME by chance is not a thing that happens.
//
// The failure mode if it is ever wrong is the safe one: a copy of ours goes
// unadopted and stays in the game's folder until you delete it yourself.
static bool LooksLikeOurCopy(const char *dst, const char *ours)
{
    WIN32_FILE_ATTRIBUTE_DATA a, b;
    if (!Stat(dst, &a) || !Stat(ours, &b))
        return false;
    if (a.nFileSizeLow != b.nFileSizeLow || a.nFileSizeHigh != b.nFileSizeHigh)
        return false;
    return a.ftLastWriteTime.dwLowDateTime == b.ftLastWriteTime.dwLowDateTime &&
           a.ftLastWriteTime.dwHighDateTime == b.ftLastWriteTime.dwHighDateTime;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

bool WrIntoGamePathAt(WrIntoGameWhere where, const char *map, int mapId,
                      const char *hash, char *out, int outLen)
{
    if (!HashLooksSane(hash) || !out || outLen < 8)
        return false;
    const char *game = WrGameDir();
    if (!game || !*game)
        return false;

    if (where == WR_INTO_LOCAL)
    {
        if (!map || !*map)
            return false;
        _snprintf_s(out, (size_t)outLen, _TRUNCATE,
                    "%s\\momentum\\momtv\\local\\%s\\%s.mtv", game, map, hash);
        return true;
    }

    if (mapId <= 0)
        return false;
    _snprintf_s(out, (size_t)outLen, _TRUNCATE,
                "%s\\momentum\\momtv\\online\\%d\\%s.mtv", game, mapId, hash);
    return true;
}

// The same file, written the way the GAME's own filesystem names it.
//
// This is what mom_tv_replay_watch takes. Momentum's end-of-run screen runs
// `mom_tv_replay_watch ${baseRun.filePath}` (panorama/scripts/pages/end-of-run/
// end-of-run.ts), so the command plays a replay BY PATH, with no leaderboard row
// and no list involved -- which is the only mechanism here that does not depend
// on the game agreeing that a demo exists.
//
// Forward slashes and relative to `momentum\`, which is how every other path in
// that filesystem is written, and how the game's own local recordings appear.
bool WrIntoGameGamePathAt(WrIntoGameWhere where, const char *map, int mapId,
                          const char *hash, char *out, int outLen)
{
    if (!HashLooksSane(hash) || !out || outLen < 8)
        return false;

    if (where == WR_INTO_LOCAL)
    {
        if (!map || !*map)
            return false;
        _snprintf_s(out, (size_t)outLen, _TRUNCATE, "momtv/local/%s/%s.mtv",
                    map, hash);
        return true;
    }

    if (mapId <= 0)
        return false;
    _snprintf_s(out, (size_t)outLen, _TRUNCATE, "momtv/online/%d/%s.mtv",
                mapId, hash);
    return true;
}

// The command for wherever this run's demo ACTUALLY is, rather than for where
// one would go.
//
// Built from WrIntoGameSourceOf's answer and not from the hash, because for 202
// of the 1,749 .wrpath files here the "hash" is a filename stem like
// "104455274-surf_fiellu-1781797367-main-nrm-60.990" -- the player's own
// recordings, which are the ones most worth being able to replay and the ones
// HashLooksSane rejects. Those are already in the game's local tree under that
// exact name, so the path is known even though the name is not a hash.
//
// Returns false when the demo is only in OUR folder, which the game's filesystem
// cannot see: the caller has to copy it in first. And when there is no demo at
// all, which is 35 more of them.
bool WrIntoGameWatchCommand(const char *map, int mapId, const char *hash,
                            char *out, int outLen)
{
    char path[MAX_PATH];
    WrIntoGameSource src = WrIntoGameSourceOf(map, mapId, hash, path,
                                              sizeof(path));
    if (src != WR_DEMO_GAME_LOCAL && src != WR_DEMO_GAME_ONLINE)
        return false;

    // Absolute to game-relative. Everything under momentum\ is addressable by
    // the game's filesystem and nothing above it is, so the split is at that
    // directory -- searched from the RIGHT, since an install can perfectly well
    // live in a path that already contains the word.
    const char *tail = NULL;
    for (const char *p = path; *p; p++)
        if (_strnicmp(p, "\\momentum\\", 10) == 0)
            tail = p + 10;
    if (!tail || !*tail)
        return false;

    char rel[MAX_PATH];
    strcpy_s(rel, sizeof(rel), tail);
    for (char *p = rel; *p; p++)
        if (*p == '\\')
            *p = '/';

    _snprintf_s(out, (size_t)outLen, _TRUNCATE, "mom_tv_replay_watch \"%s\"",
                rel);
    return true;
}

bool WrIntoGameHasFileAt(WrIntoGameWhere where, const char *map, int mapId,
                         const char *hash)
{
    char path[MAX_PATH];
    if (!WrIntoGamePathAt(where, map, mapId, hash, path, sizeof(path)))
        return false;
    return FileThere(path);
}

WrIntoGameSource WrIntoGameSourceOf(const char *map, int mapId, const char *hash,
                                    char *out, int outLen)
{
    if (out && outLen > 0)
        out[0] = '\0';

    // A run extracted from the game's own local recordings does not have a hash
    // for a name -- srcSha1 is the source .mtv's filename STEM, and for those it
    // looks like "104455274-surf_fiellu-1781797367-main-nrm-60.990". 202 of the
    // 1,749 .wrpath files on this machine are that shape. They are already in
    // the tree the game reads, so the answer is not "cannot" but "already".
    if (!HashLooksSane(hash))
    {
        const char *game = WrGameDir();
        if (game && *game && map && *map)
        {
            char path[MAX_PATH];
            _snprintf_s(path, sizeof(path), _TRUNCATE,
                        "%s\\momentum\\momtv\\local\\%s\\%s.mtv", game, map, hash);
            if (FileThere(path))
            {
                if (out) strncpy_s(out, (size_t)outLen, path, _TRUNCATE);
                return WR_DEMO_GAME_LOCAL;
            }
        }
        return WR_DEMO_NONE;
    }

    // Ours first. It is the copy we know the provenance of, and on a machine
    // where the game has also downloaded the same run the two are identical
    // anyway -- the hash is the filename because the hash is the content.
    const char *ours = OursPath(map, hash);
    if (FileThere(ours))
    {
        if (out) strncpy_s(out, (size_t)outLen, ours, _TRUNCATE);
        return WR_DEMO_OURS;
    }

    const char *game = WrGameDir();
    if (!game || !*game)
        return WR_DEMO_NONE;

    char path[MAX_PATH];
    if (mapId > 0)
    {
        _snprintf_s(path, sizeof(path), _TRUNCATE,
                    "%s\\momentum\\momtv\\online\\%d\\%s.mtv", game, mapId, hash);
        if (FileThere(path))
        {
            if (out) strncpy_s(out, (size_t)outLen, path, _TRUNCATE);
            return WR_DEMO_GAME_ONLINE;
        }
    }
    if (map && *map)
    {
        _snprintf_s(path, sizeof(path), _TRUNCATE,
                    "%s\\momentum\\momtv\\local\\%s\\%s.mtv", game, map, hash);
        if (FileThere(path))
        {
            if (out) strncpy_s(out, (size_t)outLen, path, _TRUNCATE);
            return WR_DEMO_GAME_LOCAL;
        }
    }
    return WR_DEMO_NONE;
}

bool WrIntoGameFindSource(const char *map, int mapId, const char *hash,
                          char *out, int outLen)
{
    return WrIntoGameSourceOf(map, mapId, hash, out, outLen) != WR_DEMO_NONE;
}

// ---------------------------------------------------------------------------

// MakeTree lived here, and in wr_fetch.cpp, and inside WrDataPath. One copy
// now, in wr_log.cpp; see WrMakeTree in wr_common.h for why three was one too
// many even before extraction had worker threads.

// Put an entry in the manifest, if it is not there already. Returns false only
// when the manifest is full.
static bool Record(WrIntoGameWhere where, const char *map, int mapId,
                   const char *hash)
{
    if (Find(mapId, hash, where) >= 0)
        return true;
    if (g_count >= WR_INTOGAME_MAX)
        return false;
    g_entries[g_count].mapId = mapId;
    g_entries[g_count].where = where;
    strcpy_s(g_entries[g_count].hash, sizeof(g_entries[g_count].hash), hash);
    strcpy_s(g_entries[g_count].map, sizeof(g_entries[g_count].map),
             map ? map : "");
    g_count++;
    Save();
    return true;
}

static void Say(char *detail, int detailLen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(g_status, sizeof(g_status), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (detail && detailLen > 0)
        strncpy_s(detail, (size_t)detailLen, g_status, _TRUNCATE);
}

WrIntoGameResult WrIntoGameSendTo(WrIntoGameWhere where, const char *map,
                                  int mapId, const char *hash,
                                  char *detail, int detailLen)
{
    EnsureLoaded();

    // The two reasons a destination cannot be built, told apart.
    //
    // They used to share one message -- "no numeric map id for %s ... refresh
    // the map list" -- which was the wrong advice for every run whose source is
    // one of your own local recordings, since those fail on the NAME and the map
    // id was fine. That is 202 of 1,749 files here, all of them told to fix
    // something that was not broken.
    if (!HashLooksSane(hash))
    {
        char src[MAX_PATH];
        if (WrIntoGameSourceOf(map, mapId, hash, src, sizeof(src)) ==
            WR_DEMO_GAME_LOCAL)
        {
            Say(detail, detailLen,
                "that is one of your own recordings -- the game already keeps it "
                "at %s", src);
            return WR_SEND_ALREADY_LOCAL;
        }
        Say(detail, detailLen,
            "that demo is not named like a replay hash, so there is no name to "
            "give it in the game's folder");
        return WR_SEND_BAD_NAME;
    }
    if (where == WR_INTO_ONLINE && mapId <= 0)
    {
        Say(detail, detailLen,
            "no numeric map id for \"%s\", so there is no folder to put it in -- "
            "refresh the map list", map ? map : "?");
        return WR_SEND_NO_MAPID;
    }

    char dst[MAX_PATH];
    if (!WrIntoGamePathAt(where, map, mapId, hash, dst, sizeof(dst)))
    {
        Say(detail, detailLen, "cannot work out where that would go");
        return WR_SEND_NO_MAPID;
    }

    if (FileThere(dst))
    {
        // Already there. ADOPTED only if it is provably a copy of ours -- see
        // LooksLikeOurCopy, and note that "our tree holds this hash" is NOT
        // that proof. Without adoption the fetcher's --into-game copies were
        // permanently unremovable and invisible to the count, which is exactly
        // what was wrong on this machine.
        char oursCopy[MAX_PATH];
        const char *op = OursPath(map, hash);
        oursCopy[0] = '\0';
        if (op) strncpy_s(oursCopy, sizeof(oursCopy), op, _TRUNCATE);
        if (LooksLikeOurCopy(dst, oursCopy) && Record(where, map, mapId, hash))
            Say(detail, detailLen,
                "already there, and it is one of ours -- %s", dst);
        else
            Say(detail, detailLen,
                "the game already has that one, and it is not ours to remove -- "
                "%s", dst);
        return WR_SEND_ALREADY;
    }

    char src[MAX_PATH];
    WrIntoGameSource kind = WrIntoGameSourceOf(map, mapId, hash, src, sizeof(src));
    if (kind == WR_DEMO_NONE)
    {
        Say(detail, detailLen,
            "no .mtv on disk for that run -- fetch it, or it was deleted in game "
            "and only the path cache is left");
        return WR_SEND_NO_SOURCE;
    }
    if (kind == WR_DEMO_GAME_LOCAL && where == WR_INTO_LOCAL)
    {
        Say(detail, detailLen, "already in the game's local folder -- %s", src);
        return WR_SEND_ALREADY_LOCAL;
    }

    char dir[MAX_PATH];
    strcpy_s(dir, sizeof(dir), dst);
    char *slash = strrchr(dir, '\\');
    if (slash)
        *slash = '\0';
    if (!WrMakeTree(dir))
    {
        Say(detail, detailLen, "could not create %s", dir);
        return WR_SEND_FAILED;
    }

    // Recorded BEFORE the copy. If this crashes between the two lines the file
    // is still ours to remove; the other order leaves a file nothing knows
    // about, in a directory we have promised not to guess about.
    if (!Record(where, map, mapId, hash))
    {
        Say(detail, detailLen,
            "%d already sent; remove some before adding more", WR_INTOGAME_MAX);
        return WR_SEND_FULL;
    }

    if (!CopyFileA(src, dst, FALSE))
    {
        DWORD err = GetLastError();
        Say(detail, detailLen,
            "copy failed (%lu) -- is the game holding the folder open?",
            (unsigned long)err);
        WrLogf("[!] into-game: CopyFile %s -> %s failed, %lu", src, dst,
               (unsigned long)err);
        return WR_SEND_FAILED;
    }

    WrLogf("into-game: %s -> %s", src, dst);
    Say(detail, detailLen, "written to %s", dst);
    return WR_SEND_OK;
}

// Walk our own fetched demos for this map and adopt any at the online
// destination that are provably copies of them -- see LooksLikeOurCopy. Without
// this the fetcher's --into-game copies are unremovable and uncounted, which is
// the state this machine was found in.
//
// Only when the map changes, because it is a directory enumeration -- a few tens
// of names -- rather than the handful of stats the prune costs.
static void AdoptForMap(const char *map, int mapId)
{
    if (!map || !*map || mapId <= 0)
        return;

    char rel[MAX_PATH];
    _snprintf_s(rel, sizeof(rel), _TRUNCATE, "demos\\%s", map);
    const char *ourDir = WrDataPath(rel);
    if (!ourDir || !*ourDir)
        return;

    // Copied out: WrDataPath hands back a static, and Save() below goes through
    // it again for the manifest's own path.
    char dir[MAX_PATH];
    strncpy_s(dir, sizeof(dir), ourDir, _TRUNCATE);

    char pattern[MAX_PATH];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*.mtv", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;

    int adopted = 0;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        char hash[48];
        strncpy_s(hash, sizeof(hash), fd.cFileName, _TRUNCATE);
        char *dot = strrchr(hash, '.');
        if (dot)
            *dot = '\0';
        if (!HashLooksSane(hash))
            continue;
        if (Find(mapId, hash, WR_INTO_ONLINE) >= 0)
            continue;

        char dst[MAX_PATH];
        if (!WrIntoGamePathAt(WR_INTO_ONLINE, map, mapId, hash, dst, sizeof(dst)))
            continue;
        // The enumeration gave us the name; this is what decides whether the
        // file at the destination is ours. A same-hash file the game downloaded
        // for itself fails here and is left alone, which is the whole point.
        char oursCopy[MAX_PATH];
        _snprintf_s(oursCopy, sizeof(oursCopy), _TRUNCATE, "%s\\%s", dir,
                    fd.cFileName);
        if (!LooksLikeOurCopy(dst, oursCopy))
            continue;
        if (!Record(WR_INTO_ONLINE, map, mapId, hash))
            break;              // manifest full; the rest stay unadopted
        adopted++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if (adopted)
        WrLogf("into-game: adopted %d copy/copies of ours already in the game's "
               "folder for %s", adopted, map);
}

void WrIntoGameRefresh(const char *map, int mapId)
{
    EnsureLoaded();

    // Adopt once per map, before the throttle, so switching map reconciles
    // immediately rather than up to half a second later.
    static char lastMap[72] = {0};
    static int lastMapId = 0;
    if (map && *map && mapId > 0 &&
        (mapId != lastMapId || _stricmp(map, lastMap) != 0))
    {
        strncpy_s(lastMap, sizeof(lastMap), map, _TRUNCATE);
        lastMapId = mapId;
        AdoptForMap(map, mapId);
    }

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
        if (WrIntoGameHasFileAt(g_entries[i].where, g_entries[i].map,
                                g_entries[i].mapId, g_entries[i].hash))
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
    return HashLooksSane(hash) && FindAny(mapId, hash) >= 0;
}

// Take out one entry by index. The caller owns the loop, because removing a run
// may take out two files -- one in each tree -- and compaction would otherwise
// skip the second.
static int RemoveAt(int at)
{
    if (at < 0 || at >= g_count)
        return 0;

    char path[MAX_PATH];
    int gone = 0;
    if (WrIntoGamePathAt(g_entries[at].where, g_entries[at].map,
                         g_entries[at].mapId, g_entries[at].hash, path,
                         sizeof(path)))
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

int WrIntoGameRemoveOne(int mapId, const char *hash)
{
    EnsureLoaded();
    if (!HashLooksSane(hash))
        return 0;
    // Every copy of that run, in either tree -- one press of "take out" should
    // undo whatever "send" and the Local button between them put on disk.
    // Backwards, so the compaction cannot skip the second one.
    int gone = 0;
    for (int i = g_count - 1; i >= 0; i--)
        if (g_entries[i].mapId == mapId &&
            _stricmp(g_entries[i].hash, hash) == 0)
            gone += RemoveAt(i);
    return gone;
}

int WrIntoGameRemoveAll(void)
{
    EnsureLoaded();
    int gone = 0;
    // Backwards, so the compaction inside RemoveAt cannot skip an entry.
    for (int i = g_count - 1; i >= 0; i--)
        gone += RemoveAt(i);
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
