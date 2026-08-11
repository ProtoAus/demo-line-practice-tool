// wr_fetch.cpp  --  see wr_fetch.h.

#include "wr_fetch.h"
#include "wr_board.h"
#include "wr_msml.h"
#include "wr_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void Emitf(WrEmitFn emit, const char *fmt, ...)
{
    if (!emit)
        return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    emit(buf);
}

// ---------------------------------------------------------------------------
// parse_ranks
// ---------------------------------------------------------------------------

// The reference's cap, applied FROM THE START of the range rather than to its
// width: range(lo, min(hi, lo + 4096) + 1). "1-999999" is therefore 1..4097.
#define WR_RANKS_RANGE_CAP 4096

static int CompareInt(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

// Python's int() on an already-stripped token: an optional sign then digits,
// and nothing else. Returns false where int() would raise, which is where the
// reference does `continue`.
static bool RankInt(const char *s, int *out)
{
    if (!*s)
        return false;
    const char *p = s;
    if (*p == '+' || *p == '-')
        p++;
    if (!*p)
        return false;
    for (const char *q = p; *q; q++)
        if (*q < '0' || *q > '9')
            return false;
    long long v = _strtoi64(s, NULL, 10);
    if (v < -2147483648LL || v > 2147483647LL)
        return false;           // int() would not overflow; nothing sane gets here
    *out = (int)v;
    return true;
}

static void StripEnds(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (p != s)
        memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
}

int WrFetchParseRanks(const char *spec, int *out, int maxOut)
{
    if (!spec || !*spec)
        return 0;

    // Gathered then sorted and deduplicated, which is `sorted(set(...))`.
    int cap = 256, n = 0;
    int *v = (int *)malloc(sizeof(int) * (size_t)cap);
    if (!v)
        return 0;

    const char *p = spec;
    for (;;)
    {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);

        char part[64];
        if (len >= sizeof(part))
            len = sizeof(part) - 1;
        memcpy(part, p, len);
        part[len] = '\0';
        StripEnds(part);

        if (part[0])
        {
            // `"-" in part[1:]` -- a '-' at index 0 is a SIGN and not a range
            // separator, so "-5" is the single number -5 (which the caller then
            // drops for being out of range) rather than a malformed range.
            const char *dash = part[0] ? strchr(part + 1, '-') : NULL;
            if (dash)
            {
                char lo[64];
                size_t at = (size_t)(dash - part);
                memcpy(lo, part, at);
                lo[at] = '\0';

                int a = 0, b = 0;
                if (RankInt(lo, &a) && RankInt(dash + 1, &b))
                {
                    if (b < a) { int t = a; a = b; b = t; }
                    int stop = b;
                    if (stop > a + WR_RANKS_RANGE_CAP)
                        stop = a + WR_RANKS_RANGE_CAP;
                    for (int r = a; r <= stop; r++)
                    {
                        if (n >= cap)
                        {
                            int grown = cap * 2;
                            int *bigger = (int *)realloc(v, sizeof(int) * (size_t)grown);
                            if (!bigger)
                                break;
                            v = bigger;
                            cap = grown;
                        }
                        v[n++] = r;
                    }
                }
            }
            else
            {
                int a = 0;
                if (RankInt(part, &a))
                {
                    if (n >= cap)
                    {
                        int grown = cap * 2;
                        int *bigger = (int *)realloc(v, sizeof(int) * (size_t)grown);
                        if (bigger) { v = bigger; cap = grown; }
                    }
                    if (n < cap)
                        v[n++] = a;
                }
            }
        }

        if (!comma)
            break;
        p = comma + 1;
    }

    if (n > 1)
        qsort(v, (size_t)n, sizeof(int), CompareInt);

    int kept = 0;
    for (int i = 0; i < n; i++)
    {
        if (i > 0 && v[i] == v[i - 1])
            continue;           // set()
        if (out)
        {
            if (kept >= maxOut)
                break;
            out[kept] = v[i];
        }
        kept++;
    }
    free(v);
    return kept;
}

int WrFetchParseRanksFile(const char *path, int *out, int maxOut, char *err,
                          int errCap)
{
    FILE *f = NULL;
    if (fopen_s(&f, path, "r") != 0 || !f)
    {
        // The reference prints the OSError's own text, which names errno and
        // the path in CPython's wording and is not reproducible. Same shape,
        // our words; it cannot reach a compared artefact.
        _snprintf_s(err, (size_t)errCap, _TRUNCATE, "cannot open %s", path);
        return -1;
    }

    // The reference joins the file's lines with commas and hands the result to
    // parse_ranks, so a line may itself be "12,14-16" and it still works. Same
    // here: one accumulated spec, one parse.
    size_t cap = 4096, used = 0;
    char *spec = (char *)malloc(cap);
    if (!spec)
    {
        fclose(f);
        _snprintf_s(err, (size_t)errCap, _TRUNCATE, "out of memory");
        return -1;
    }
    spec[0] = '\0';

    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        // `line.strip()` first, then the '#' test -- so an INDENTED comment is
        // still a comment. The reference tests line.startswith("#") on the
        // stripped value for the emptiness check but on the RAW line for the
        // comment, so a leading space before '#' would slip through there. It
        // then fails to parse and is dropped anyway, which is the same outcome
        // by a different route.
        StripEnds(line);
        if (!line[0] || line[0] == '#')
            continue;

        size_t need = used + strlen(line) + 2;
        if (need > cap)
        {
            while (cap < need)
                cap *= 2;
            char *grown = (char *)realloc(spec, cap);
            if (!grown)
                break;
            spec = grown;
        }
        if (used)
            spec[used++] = ',';
        strcpy_s(spec + used, cap - used, line);
        used += strlen(line);
    }
    fclose(f);

    int n = WrFetchParseRanks(spec, out, maxOut);
    free(spec);
    return n;
}

// ---------------------------------------------------------------------------
// What is already on disk
// ---------------------------------------------------------------------------

static int CompareHash(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void HeldAdd(WrFetchHeld *h, const char *name)
{
    if (h->n >= h->cap)
    {
        int cap = h->cap ? h->cap * 2 : 1024;
        char **grown = (char **)realloc(h->hash, sizeof(char *) * (size_t)cap);
        if (!grown)
            return;
        h->hash = grown;
        h->cap = cap;
    }

    // The whole stem, however long it is. See WrFetchHeld in wr_fetch.h: a
    // fixed 48-byte field lost four demos out of six thousand on this machine,
    // silently, by collapsing two long filenames into one.
    size_t n = strlen(name);
    char *copy = (char *)malloc(n + 1);
    if (!copy)
        return;
    for (size_t i = 0; i < n; i++)
        copy[i] = (name[i] >= 'A' && name[i] <= 'Z') ? (char)(name[i] - 'A' + 'a')
                                                     : name[i];
    copy[n] = '\0';
    h->hash[h->n++] = copy;
}

static void WalkTree(WrFetchHeld *h, const char *root)
{
    char pattern[MAX_PATH];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*", root);

    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == '\0' ||
             (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
            continue;

        char full[MAX_PATH];
        _snprintf_s(full, sizeof(full), _TRUNCATE, "%s\\%s", root, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            // _have_hashes walks with os.walk, whose followlinks defaults to
            // False, and CPython has reported junctions as links since 3.8. So
            // a junction inside either demo tree contributes nothing to the
            // reference's held set and must contribute nothing to ours: this
            // count is printed to the user ("N of M are already here") and
            // decides what gets downloaded. It is also the same ancestor-cycle
            // hazard WalkDemos guards against.
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                continue;
            WalkTree(h, full);
            continue;
        }
        const char *dot = strrchr(fd.cFileName, '.');
        if (!dot || _stricmp(dot, ".mtv") != 0)
            continue;

        char base[MAX_PATH];
        strncpy_s(base, sizeof(base), fd.cFileName, _TRUNCATE);
        char *bdot = strrchr(base, '.');
        if (bdot)
            *bdot = '\0';
        HeldAdd(h, base);
    } while (FindNextFileA(find, &fd));
    FindClose(find);
}

void WrFetchHeldBuild(WrFetchHeld *h, const char *gameDir)
{
    memset(h, 0, sizeof(*h));

    // demo_roots(game): the game's whole momtv tree, and ours. Both walked
    // recursively, because momtv holds online\<id>\ and local\<map>\ and ours
    // holds demos\<map>\.
    char root[MAX_PATH];
    _snprintf_s(root, sizeof(root), _TRUNCATE, "%s\\momentum\\momtv", gameDir);
    WalkTree(h, root);
    WalkTree(h, WrDataPath("demos"));

    if (h->n > 1)
    {
        qsort(h->hash, (size_t)h->n, sizeof(char *), CompareHash);

        // A SET, not a list, and the difference is visible. The reference's
        // _have_hashes builds a set, and the count it holds is printed -- "N
        // demos on disk across every map". The same name is in both trees
        // whenever --into-game has been used, which is precisely the case this
        // file creates, so a list would report a number that grows every time
        // somebody copies a demo they already had.
        int w = 1;
        for (int i = 1; i < h->n; i++)
        {
            if (strcmp(h->hash[i], h->hash[w - 1]) == 0)
            {
                free(h->hash[i]);
                continue;
            }
            h->hash[w++] = h->hash[i];
        }
        h->n = w;
    }
}

bool WrFetchHeldHas(const WrFetchHeld *h, const char *hash)
{
    if (!h->hash || h->n <= 0)
        return false;

    char stack[64];
    size_t n = strlen(hash);
    char *key = (n + 1 <= sizeof(stack)) ? stack : (char *)malloc(n + 1);
    if (!key)
        return false;
    for (size_t i = 0; i < n; i++)
        key[i] = (hash[i] >= 'A' && hash[i] <= 'Z') ? (char)(hash[i] - 'A' + 'a')
                                                    : hash[i];
    key[n] = '\0';

    const char *probe = key;
    bool found = bsearch(&probe, h->hash, (size_t)h->n, sizeof(char *),
                         CompareHash) != NULL;
    if (key != stack)
        free(key);
    return found;
}

void WrFetchHeldFree(WrFetchHeld *h)
{
    for (int i = 0; i < h->n; i++)
        free(h->hash[i]);
    free(h->hash);
    memset(h, 0, sizeof(*h));
}

// ---------------------------------------------------------------------------
// Writing a demo down
// ---------------------------------------------------------------------------

// MakeTree used to be here, and in wr_intogame.cpp, and inside WrDataPath. One
// copy now, in wr_log.cpp; see WrMakeTree in wr_common.h for why three was one
// too many even before there were worker threads.

static bool WriteWhole(const char *path, const unsigned char *data, size_t len)
{
    FILE *f = NULL;
    if (fopen_s(&f, path, "wb") != 0 || !f)
        return false;
    bool ok = (len == 0) || (fwrite(data, 1, len, f) == len);
    return (fclose(f) == 0) && ok;
}

// shutil.copy2 plus os.replace, which is three Win32 calls and one of them is
// the point. See the long comment in wr_fetch.h: CopyFileA gives the
// destination a FRESH write time, and the whole ownership test in
// wr_intogame.h is "same size and same write time as our copy". So the
// timestamp is carried across explicitly, and a failure to carry it is a
// failure of the copy rather than something to shrug at -- a file that lands
// with the wrong stamp is one the removal button will refuse to touch for ever.
bool WrFetchCopyWithTime(const char *from, const char *to, char *err,
                         int errCap)
{
    char tmp[MAX_PATH];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s.tmp", to);

    if (!CopyFileA(from, tmp, FALSE))
    {
        _snprintf_s(err, (size_t)errCap, _TRUNCATE, "copy failed (%lu)",
                    GetLastError());
        return false;
    }

    bool stamped = false;
    HANDLE src = CreateFileA(from, GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (src != INVALID_HANDLE_VALUE)
    {
        FILETIME created, accessed, written;
        if (GetFileTime(src, &created, &accessed, &written))
        {
            HANDLE dst = CreateFileA(tmp, FILE_WRITE_ATTRIBUTES, 0, NULL,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (dst != INVALID_HANDLE_VALUE)
            {
                // copy2 carries mtime and atime. The creation time is left to
                // the filesystem, as it is there -- Python has no portable way
                // to set it and does not try.
                stamped = SetFileTime(dst, NULL, &accessed, &written) != 0;
                CloseHandle(dst);
            }
        }
        CloseHandle(src);
    }
    if (!stamped)
    {
        DeleteFileA(tmp);
        _snprintf_s(err, (size_t)errCap, _TRUNCATE,
                    "could not carry the write time across (%lu)", GetLastError());
        return false;
    }

    // Atomic, so the game can never see a half-written replay even if it is
    // scanning that directory at the time.
    if (!MoveFileExA(tmp, to, MOVEFILE_REPLACE_EXISTING))
    {
        _snprintf_s(err, (size_t)errCap, _TRUNCATE, "could not place it (%lu)",
                    GetLastError());
        DeleteFileA(tmp);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// _download
// ---------------------------------------------------------------------------

static int Download(const char *dest, const WrBoardCacheRow *rows, int count,
                    const WrFetchHeld *have, const char *intoGame,
                    WrEmitFn emit, WrApiAbortFn abort, void *abortUser)
{
    // What is left after the dedupe. A row with no downloadURL counts as
    // "already here" in the line below, which is the reference's arithmetic:
    // len(rows) - len(todo) lumps the two together.
    int *todo = (int *)malloc(sizeof(int) * (size_t)(count > 0 ? count : 1));
    int n = 0;
    if (todo)
        for (int i = 0; i < count; i++)
            if (!WrFetchHeldHas(have, rows[i].hash) && rows[i].url[0])
                todo[n++] = i;

    Emitf(emit, "%d of %d are already here; %d to fetch", count - n, count, n);
    if (n == 0)
    {
        free(todo);
        return 0;
    }

    WrMakeTree(dest);

    char intoGameDir[MAX_PATH] = "";
    if (intoGame && *intoGame)
    {
        WrMakeTree(intoGame);
        DWORD attr = GetFileAttributesA(intoGame);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
        {
            strcpy_s(intoGameDir, sizeof(intoGameDir), intoGame);
            Emitf(emit, "also placing them where the game looks: %s", intoGame);
        }
        else
        {
            // The reference puts the OSError's own text in brackets here.
            // That string is CPython's and is not reproducible, and this is
            // the one message in this file that can only appear when the game
            // folder is read-only -- which no comparison run can arrange.
            Emitf(emit, "[!] cannot write to the game's replay folder; "
                        "downloading to wrlines_data only");
        }
    }

    int got = 0;
    for (int k = 0; k < n; k++)
    {
        if (abort && abort(abortUser))
        {
            Emitf(emit, "stopped after %d demo%s", got, got == 1 ? "" : "s");
            break;
        }

        const WrBoardCacheRow *r = &rows[todo[k]];
        Emitf(emit, "[%d/%d] rank %d  %.3fs  %s", k + 1, n, r->rank, r->time,
              r->alias);

        unsigned char *blob = NULL;
        size_t len = 0;
        char err[256] = "";
        if (!WrApiGet(r->url, &blob, &len, err, sizeof(err)))
        {
            Emitf(emit, "      failed: %s", err);
            continue;
        }

        // A downloadURL can answer with an error page or a redirect to a login,
        // and both are plausible bytes. Refused before it reaches the disk,
        // because a half-file named after a replay hash would be counted as
        // "we have that run" by the dedupe above, for ever.
        if (len < 0x100 || memcmp(blob, "MMTV", 4) != 0)
        {
            Emitf(emit, "      not a demo (%zu bytes); skipped", len);
            free(blob);
            continue;
        }

        char ours[MAX_PATH];
        _snprintf_s(ours, sizeof(ours), _TRUNCATE, "%s\\%s.mtv", dest, r->hash);
        if (!WriteWhole(ours, blob, len))
        {
            Emitf(emit, "      could not write %s", ours);
            free(blob);
            continue;
        }
        free(blob);

        if (intoGameDir[0])
        {
            char gpath[MAX_PATH];
            _snprintf_s(gpath, sizeof(gpath), _TRUNCATE, "%s\\%s.mtv",
                        intoGameDir, r->hash);
            char cerr[128] = "";
            if (!WrFetchCopyWithTime(ours, gpath, cerr, sizeof(cerr)))
                Emitf(emit, "      (could not place it in the game folder: %s)",
                      cerr);
        }

        got++;
        if (!WrApiPace(abort, abortUser))
        {
            Emitf(emit, "stopped after %d demo%s", got, got == 1 ? "" : "s");
            break;
        }
    }

    free(todo);
    Emitf(emit, "fetched %d demo%s into %s", got, got == 1 ? "" : "s", dest);
    if (emit)
        emit("run the extractor on this map to turn them into lines");
    return got;
}

// ---------------------------------------------------------------------------
// cmd_fetch
// ---------------------------------------------------------------------------

#define WR_FETCH_MAX_CATALOGUE 4096

// _resolve_map again. Not shared with wr_api.cpp's copy because that one is
// static and this one wants the same three lines -- if a third caller ever
// appears it moves, and until then two eight-line functions that print the
// same two sentences are cheaper to read than an export.
static bool ResolveMap(const WrFetchArgs *a, WrEmitFn emit, char *nameOut,
                       int nameCap, int *idOut)
{
    const char *name = (a->map && a->map[0]) ? a->map : NULL;
    int mapId = a->mapId;

    if (!mapId)
    {
        WrMsmlMap *cat = NULL;
        int n = 0;
        if (name)
        {
            cat = (WrMsmlMap *)malloc(sizeof(WrMsmlMap) * WR_FETCH_MAX_CATALOGUE);
            if (cat)
                n = WrMsmlRead(a->gameDir, cat, WR_FETCH_MAX_CATALOGUE, NULL);
        }
        int at = -1;
        for (int i = 0; i < n; i++)
            if (strcmp(cat[i].name, name) == 0)
            {
                at = i;
                break;
            }
        if (at < 0)
        {
            free(cat);
            if (name)
                Emitf(emit, "[!] don't know a map id for '%s'.", name);
            else
                Emitf(emit, "[!] don't know a map id for None.");
            if (emit)
                emit("    Run --index-maps first, or pass --map-id.");
            return false;
        }
        mapId = cat[at].id;
        free(cat);
    }

    if (name)
        strncpy_s(nameOut, (size_t)nameCap, name, _TRUNCATE);
    else
        _snprintf_s(nameOut, (size_t)nameCap, _TRUNCATE, "map%d", mapId);
    *idOut = mapId;
    return true;
}

int WrFetchRun(const WrFetchArgs *a, WrEmitFn emit,
               WrApiAbortFn abort, void *abortUser)
{
    char name[72];
    int mapId = 0;
    if (!ResolveMap(a, emit, name, sizeof(name), &mapId))
        return 1;

    char rel[MAX_PATH];
    _snprintf_s(rel, sizeof(rel), _TRUNCATE, "demos\\%s", name);
    char dest[MAX_PATH];
    strcpy_s(dest, sizeof(dest), WrDataPath(rel));

    WrFetchHeld have;
    WrFetchHeldBuild(&have, a->gameDir);

    // momtv\online\<mapId>\ -- the game's own layout, and the same filename we
    // already write, because the hash IS the name.
    char intoGame[MAX_PATH] = "";
    if (a->intoGame && mapId)
        _snprintf_s(intoGame, sizeof(intoGame), _TRUNCATE,
                    "%s\\momentum\\momtv\\online\\%d", a->gameDir, mapId);

    Emitf(emit, "%d demos on disk across every map; each run below is checked "
                "against all of them by hash", have.n);

    // --- named places out of the cached board -------------------------------
    //
    // The cache holds the downloadURL the server itself handed back, so picking
    // rows out of a board you have already browsed costs no leaderboard request
    // at all. This is what the Board tab's tick-and-download uses.
    if (a->rankCount > 0 && a->ranks)
    {
        char path[MAX_PATH];
        WrBoardCachePath(path, sizeof(path), name, a->gamemode, a->trackType,
                         a->trackNum);

        WrBoardCache held;
        if (!WrBoardReadCache(path, &held, 0) || held.count == 0)
        {
            WrBoardCacheFree(&held);
            WrFetchHeldFree(&have);
            char modeName[24];
            if (a->gamemode >= 1 && a->gamemode <= WR_GAMEMODE_COUNT)
                strcpy_s(modeName, sizeof(modeName), WrGamemodeName(a->gamemode));
            else
                _snprintf_s(modeName, sizeof(modeName), _TRUNCATE, "%d", a->gamemode);
            Emitf(emit, "[!] no cached board for %s (%s, track %d/%d).",
                  name, modeName, a->trackType, a->trackNum);
            if (emit)
                emit("    Fetch a window of it first -- see --board.");
            return 1;
        }

        // {r[0]: r for r in held.values()} -- one row per RANK, and a later row
        // in file order wins a tie. The file is written rank-sorted with ties
        // broken by insertion order, so "later" is well defined on both sides.
        WrBoardCacheRow *rows =
            (WrBoardCacheRow *)malloc(sizeof(WrBoardCacheRow) *
                                      (size_t)(a->rankCount > 0 ? a->rankCount : 1));
        int found = 0;

        int missing[13];
        int missingCount = 0, missingShown = 0;
        char missingText[160] = "";

        for (int i = 0; i < a->rankCount; i++)
        {
            int want = a->ranks[i];
            int at = -1;
            for (int j = 0; j < held.count; j++)
                if (held.rows[j].rank == want)
                    at = j;                     // last one wins
            if (at >= 0)
            {
                if (rows)
                    rows[found] = held.rows[at];
                found++;
            }
            else
            {
                if (missingShown < 12)
                    missing[missingShown++] = want;
                missingCount++;
            }
        }

        Emitf(emit, "map %s, %d rank%s asked for, %d in the cache, 0 "
                    "leaderboard requests",
              name, a->rankCount, a->rankCount == 1 ? "" : "s", found);

        if (missingCount)
        {
            // Named, not silently dropped: a rank outside the cached windows is
            // something the user can fix by fetching that window.
            int used = 0;
            for (int i = 0; i < missingShown; i++)
                used += _snprintf_s(missingText + used, sizeof(missingText) - used,
                                    _TRUNCATE, "%s%d", i ? ", " : "", missing[i]);
            Emitf(emit, "    not cached, so skipped: %s%s", missingText,
                  missingCount > 12 ? " ..." : "");
        }

        if (found == 0 || !rows)
        {
            free(rows);
            WrBoardCacheFree(&held);
            WrFetchHeldFree(&have);
            return 1;
        }

        if (a->dryRun)
        {
            // A DELIBERATE DIVERGENCE, and the only one in this file.
            //
            // The reference does not check --dry-run on this path: cmd_fetch's
            // ranks branch calls _download and returns before the dry-run test
            // further down ever runs. So `--fetch --ranks 5 --dry-run`
            // downloads there. That is a bug rather than a decision, and it is
            // not one to reproduce faithfully: reproducing it means writing
            // files to somebody's disk and fetching them off somebody's server
            // after they said not to.
            //
            // It cannot affect a comparison. The panel cannot produce the
            // combination -- the Board tab's tick-and-download sets ranks and
            // never dryRun, the Maps tab's browse sets dryRun and never ranks
            // -- and the parity driver does not pass both.
            for (int i = 0; i < found; i++)
                Emitf(emit, "  %s  rank %-5d %8.3fs  %s",
                      WrFetchHeldHas(&have, rows[i].hash) ? "have" : "  --",
                      rows[i].rank, rows[i].time, rows[i].alias);
        }
        else
        {
            Download(dest, rows, found, &have, intoGame, emit, abort, abortUser);
        }

        free(rows);
        WrBoardCacheFree(&held);
        WrFetchHeldFree(&have);
        return 0;
    }

    // --- straight from the leaderboard --------------------------------------
    int count = a->count > 0 ? a->count : (a->top > 0 ? a->top : WR_API_MAX_DEFAULT);

    WrApiWindow w;
    memset(&w, 0, sizeof(w));
    int first = 1;

    if (a->slowest)
    {
        char url[512];
        WrApiLeaderboardUrl(url, sizeof(url), mapId, a->gamemode, a->trackType,
                            a->trackNum, 1, 0);
        unsigned char *body = NULL;
        size_t len = 0;
        char err[256] = "";
        if (!WrApiGet(url, &body, &len, err, sizeof(err)))
        {
            Emitf(emit, "[!] leaderboard request failed: %s", err);
            WrFetchHeldFree(&have);
            return 1;
        }
        WrBoardCacheRow probe[WR_API_PAGE];
        int entries = 0;
        long long tc = 0;
        bool haveTc = false;
        int kept = WrApiParsePage((const char *)body, len, probe, WR_API_PAGE,
                                  &entries, &tc, &haveTc);
        free(body);
        if (kept < 0)
        {
            Emitf(emit, "[!] leaderboard request failed: the reply was not a "
                        "leaderboard page");
            WrFetchHeldFree(&have);
            return 1;
        }
        long long total = haveTc ? tc : 0;
        if (total <= 0)
        {
            Emitf(emit, "no runs on this track.");
            WrFetchHeldFree(&have);
            return 0;
        }
        first = (int)(total - count + 1);
        if (first < 1)
            first = 1;
        Emitf(emit, "the leaderboard holds %lld runs; taking ranks %d-%lld",
              total, first, total);
        if (!WrApiPace(abort, abortUser))
        {
            Emitf(emit, "stopped before anything was downloaded");
            WrFetchHeldFree(&have);
            return 1;
        }
    }
    else
    {
        first = a->fromRank > 0 ? a->fromRank : 1;
        Emitf(emit, "map %s (id %d), track %d/%d, ranks %d-%d",
              name, mapId, a->trackType, a->trackNum, first, first + count - 1);
    }

    WrApiFetchWindow(&w, mapId, a->gamemode, a->trackType, a->trackNum,
                     first, count, abort, abortUser);

    if (w.err[0])
    {
        Emitf(emit, "[!] leaderboard request failed: %s", w.err);
        WrApiWindowFree(&w);
        WrFetchHeldFree(&have);
        return 1;
    }
    if (w.haveTotal)
        Emitf(emit, "the leaderboard holds %lld run%s for this track (%d "
                    "request%s made)",
              w.total, w.total == 1 ? "" : "s", w.requests,
              w.requests == 1 ? "" : "s");

    if (w.count == 0)
    {
        // The reference's wording here is NOT the one cmd_board prints: this
        // one stops at the track suggestion and does not add "and check the
        // gamemode". Two nearly identical sentences, and they have to stay
        // nearly identical.
        if (emit)
            emit("no runs on this track. If the map has stages or bonuses, the "
                 "main track can be empty while the stages are not -- try "
                 "--track-type 1 --track-num 1.");
        WrApiWindowFree(&w);
        WrFetchHeldFree(&have);
        return 0;
    }

    if (a->dryRun)
    {
        // The whole page is listed and what is already here is marked, so this
        // doubles as "show me the leaderboard" rather than only "show me the
        // gap". Nothing is downloaded and nothing is written.
        for (int i = 0; i < w.count; i++)
            Emitf(emit, "  %s  rank %-5d %8.3fs  %s",
                  WrFetchHeldHas(&have, w.rows[i].hash) ? "have" : "  --",
                  w.rows[i].rank, w.rows[i].time, w.rows[i].alias);
    }
    else
    {
        Download(dest, w.rows, w.count, &have, intoGame, emit, abort, abortUser);
    }

    WrApiWindowFree(&w);
    WrFetchHeldFree(&have);
    return 0;
}
