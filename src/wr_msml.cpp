// wr_msml.cpp  --  see wr_msml.h.

#include "wr_msml.h"
#include "wr_json.h"
#include "wr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"

// The decompressed catalogue is about 12 MB today across two files. This is the
// refusal point, not an expectation: the size comes out of a file somebody
// else's program wrote, and a corrupt one claiming 4 GB should be rejected
// rather than attempted.
#define WR_MSML_MAX_BYTES (256u * 1024u * 1024u)

void WrMsmlCacheDir(const char *gameDir, char *out, int cap)
{
    _snprintf_s(out, cap, _TRUNCATE, "%s\\momentum\\_cache", gameDir);
}

// ---------------------------------------------------------------------------
// One .dat
// ---------------------------------------------------------------------------

// Slurp the file. Returns NULL on any failure; caller frees.
static unsigned char *ReadWhole(const char *path, size_t *lenOut)
{
    *lenOut = 0;
    FILE *f = NULL;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return NULL;

    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || (unsigned long)n > WR_MSML_MAX_BYTES)
    {
        fclose(f);
        return NULL;
    }

    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf)
    {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n)
    {
        free(buf);
        return NULL;
    }
    *lenOut = got;
    return buf;
}

static unsigned int Le32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

// Find where this map already sits in `out`, or -1. Linear, and that is fine:
// it runs 2135 times over a list that reaches 2050, which is four million
// string compares once per button press.
static int FindByName(const WrMsmlMap *out, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (strcmp(out[i].name, name) == 0)
            return i;
    return -1;
}

// Pull one map object out of the array. The cursor is on its opening brace.
static bool ReadMap(WrJson *j, WrMsmlMap *m)
{
    m->id = 0;
    m->name[0] = '\0';
    m->tier = 0;
    m->modes = 0;
    m->stages = 0;
    m->bonuses = 0;

    bool haveId = false, haveName = false;
    if (!WrJsonEnterObject(j))
        return false;

    char key[64];
    while (WrJsonNextMember(j, key, sizeof(key)))
    {
        if (strcmp(key, "id") == 0)
        {
            bool ok = false;
            long long v = WrJsonInt(j, 0, &ok);
            // An int, specifically. A catalogue saying "id": 265.0 is skipped
            // rather than rounded, which is what isinstance(mid, int) does.
            if (ok && v > 0 && v <= 0x7FFFFFFF)
            {
                m->id = (int)v;
                haveId = true;
            }
        }
        else if (strcmp(key, "name") == 0)
        {
            haveName = WrJsonString(j, m->name, sizeof(m->name)) && m->name[0];
        }
        else if (strcmp(key, "leaderboards") == 0)
        {
            if (!WrJsonEnterArray(j))
                continue;
            while (WrJsonNextElement(j))
            {
                if (!WrJsonEnterObject(j))
                    continue;
                int trackType = -1, tier = 0, mode = -1, trackNum = -1;
                bool haveTier = false;
                char lk[32];
                while (WrJsonNextMember(j, lk, sizeof(lk)))
                {
                    if (strcmp(lk, "trackType") == 0)
                    {
                        bool ok = false;
                        long long v = WrJsonInt(j, -1, &ok);
                        trackType = ok ? (int)v : -1;
                    }
                    else if (strcmp(lk, "trackNum") == 0)
                    {
                        bool ok = false;
                        long long v = WrJsonInt(j, -1, &ok);
                        trackNum = ok ? (int)v : -1;
                    }
                    else if (strcmp(lk, "tier") == 0)
                    {
                        // Very often null on a submitted map, and null is not
                        // an int, so it leaves the tier at 0.
                        bool ok = false;
                        long long v = WrJsonInt(j, 0, &ok);
                        haveTier = ok;
                        tier = (int)v;
                    }
                    else if (strcmp(lk, "gamemode") == 0)
                    {
                        bool ok = false;
                        long long v = WrJsonInt(j, -1, &ok);
                        mode = ok ? (int)v : -1;
                    }
                    else
                    {
                        WrJsonSkip(j);
                    }
                }
                // The LAST trackType-0 board with a real tier wins, because the
                // reference assigns without breaking out of its loop. Every map
                // seen has one such board, so this only matters if that ever
                // stops being true -- and then it should still agree.
                if (trackType == 0 && haveTier)
                    m->tier = tier;
                if (mode >= 0 && mode < 32)
                    m->modes |= (1u << mode);

                // The highest leg number of each kind, across every gamemode.
                // Across, and not per mode, because how a map is cut up is a
                // property of the map: a stage exists whether or not anybody has
                // ever run it in bhop. Capped at 255 by the field, which is two
                // orders of magnitude past any real map.
                if (trackNum > 0 && trackNum <= 255)
                {
                    if (trackType == 1 && trackNum > m->stages)
                        m->stages = (unsigned char)trackNum;
                    else if (trackType == 2 && trackNum > m->bonuses)
                        m->bonuses = (unsigned char)trackNum;
                }
            }
        }
        else
        {
            WrJsonSkip(j);
        }
    }
    return haveId && haveName && !WrJsonFailed(j);
}

// One cache file, merged into `out`.
static int ReadDat(const char *path, bool approved, WrMsmlMap *out, int n,
                   int maxOut)
{
    size_t rawLen = 0;
    unsigned char *raw = ReadWhole(path, &rawLen);
    if (!raw)
        return n;

    if (rawLen < 16 || memcmp(raw, "MSML", 4) != 0)
    {
        free(raw);
        return n;
    }

    // The header's own idea of how big this gets. Believed only as far as
    // sizing a buffer; miniz is told the real limit and stops at it.
    unsigned int claimed = Le32(raw + 4);
    if (claimed == 0 || claimed > WR_MSML_MAX_BYTES)
        claimed = WR_MSML_MAX_BYTES;

    size_t jsonLen = 0;
    void *json = tinfl_decompress_mem_to_heap(raw + 12, rawLen - 12, &jsonLen,
                                              TINFL_FLAG_PARSE_ZLIB_HEADER);
    free(raw);
    if (!json)
    {
        WrLogf("[!] maps: %s did not decompress", path);
        return n;
    }
    if (jsonLen > WR_MSML_MAX_BYTES)
    {
        mz_free(json);
        WrLogf("[!] maps: %s decompressed to %zu bytes, refusing", path, jsonLen);
        return n;
    }

    WrJson j;
    WrJsonInit(&j, (const char *)json, jsonLen);

    // The reference accepts either a bare array or an object with a "maps"
    // member. Every file seen is the array; the other branch is kept because
    // dropping it would be a silent behaviour change on a file we have not met.
    bool entered = WrJsonEnterArray(&j);
    if (!entered && WrJsonEnterObject(&j))
    {
        char key[32];
        while (WrJsonNextMember(&j, key, sizeof(key)))
        {
            if (strcmp(key, "maps") == 0 && WrJsonEnterArray(&j))
            {
                entered = true;
                break;
            }
            WrJsonSkip(&j);
        }
    }
    if (!entered)
    {
        mz_free(json);
        WrLogf("[!] maps: %s is not a map list", path);
        return n;
    }

    int added = 0, replaced = 0;
    while (WrJsonNextElement(&j))
    {
        WrMsmlMap m;
        if (!ReadMap(&j, &m))
        {
            if (WrJsonFailed(&j))
                break;
            continue;
        }
        m.approved = approved;

        int at = FindByName(out, n, m.name);
        if (at >= 0)
        {
            out[at] = m;        // a later file wins, as a dict assignment does
            replaced++;
        }
        else if (n < maxOut)
        {
            out[n++] = m;
            added++;
        }
        else
        {
            WrLogf("[!] maps: more than %d maps in the cache; the rest are "
                   "ignored", maxOut);
            break;
        }
    }

    if (WrJsonFailed(&j))
        WrLogf("[!] maps: %s stopped parsing part way through (%d read)",
               path, added + replaced);
    mz_free(json);
    return n;
}

// ---------------------------------------------------------------------------

int WrMsmlRead(const char *gameDir, WrMsmlMap *out, int maxOut, int *skippedFiles)
{
    if (skippedFiles)
        *skippedFiles = 0;
    if (!gameDir || !*gameDir || !out || maxOut <= 0)
        return -1;

    char dir[MAX_PATH];
    WrMsmlCacheDir(gameDir, dir, sizeof(dir));

    char glob[MAX_PATH];
    _snprintf_s(glob, sizeof(glob), _TRUNCATE, "%s\\*.dat", dir);

    // Sorted filename order, because a later file overwrites an earlier one and
    // FindFirstFile makes no promise about order. approved_1432 before
    // submission_8791 is the difference between a map reading approved and not.
    #define WR_MSML_MAX_FILES 64
    char names[WR_MSML_MAX_FILES][MAX_PATH];
    int fileCount = 0;

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(glob, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (fileCount >= WR_MSML_MAX_FILES)
            break;
        strcpy_s(names[fileCount], MAX_PATH, fd.cFileName);
        fileCount++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    for (int i = 1; i < fileCount; i++)
    {
        char tmp[MAX_PATH];
        strcpy_s(tmp, MAX_PATH, names[i]);
        int k = i - 1;
        while (k >= 0 && strcmp(names[k], tmp) > 0)
        {
            strcpy_s(names[k + 1], MAX_PATH, names[k]);
            k--;
        }
        strcpy_s(names[k + 1], MAX_PATH, tmp);
    }

    int n = 0;
    for (int i = 0; i < fileCount; i++)
    {
        char path[MAX_PATH];
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", dir, names[i]);
        bool approved = (_strnicmp(names[i], "approved", 8) == 0);
        int before = n;
        n = ReadDat(path, approved, out, n, maxOut);
        if (n == before && skippedFiles)
            (*skippedFiles)++;
    }
    return n;
}
