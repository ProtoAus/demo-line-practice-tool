// wr_update.cpp  --  see wr_update.h.

#include "wr_update.h"
#include "wr_api.h"
#include "wr_json.h"
#include "wr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Versions
// ---------------------------------------------------------------------------

// Up to three components, and the string must END after them. A tag this does
// not fully understand is BAD, never "probably newer": BAD can only ever stop a
// download, and a guess could start one.
static bool ParseVersion(const char *s, int *out)
{
    if (!s)
        return false;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == 'v' || *s == 'V')
        s++;

    out[0] = out[1] = out[2] = 0;
    int n = 0;
    for (;;)
    {
        if (*s < '0' || *s > '9')
            return false;                       // a component with no digits
        int v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9')
        {
            if (digits < 6)                     // 999999 is plenty; no overflow
                v = v * 10 + (*s - '0');
            digits++;
            s++;
        }
        if (digits > 6)
            return false;
        if (n < 3)
            out[n] = v;
        n++;
        if (*s != '.')
            break;
        s++;
        if (n >= 3)
            return false;                       // a fourth component
    }

    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    return *s == 0;                             // "1.0.0-rc1" ends here, as BAD
}

int WrUpdateCompareVersions(const char *a, const char *b)
{
    int va[3], vb[3];
    if (!ParseVersion(a, va) || !ParseVersion(b, vb))
        return WR_UPDATE_VERSION_BAD;
    for (int i = 0; i < 3; i++)
    {
        if (va[i] < vb[i]) return -1;
        if (va[i] > vb[i]) return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The release document
// ---------------------------------------------------------------------------

static void CopyTrimmed(char *dst, int cap, const char *src)
{
    strncpy_s(dst, (size_t)cap, src, _TRUNCATE);
}

bool WrUpdateParseRelease(const char *json, size_t len, WrUpdateRelease *out)
{
    if (!json || !out)
        return false;
    memset(out, 0, sizeof(*out));

    WrJson j;
    WrJsonInit(&j, json, len);
    if (!WrJsonEnterObject(&j))
        return false;

    char key[64];
    while (WrJsonNextMember(&j, key, sizeof(key)))
    {
        if (strcmp(key, "tag_name") == 0)
        {
            if (!WrJsonString(&j, out->tag, sizeof(out->tag)))
                out->tag[0] = 0;
        }
        else if (strcmp(key, "html_url") == 0)
        {
            if (!WrJsonString(&j, out->htmlUrl, sizeof(out->htmlUrl)))
                out->htmlUrl[0] = 0;
        }
        else if (strcmp(key, "body") == 0)
        {
            // Truncated rather than refused. Notes are a courtesy; a release
            // with a very long body is still a release.
            if (!WrJsonString(&j, out->notes, sizeof(out->notes)))
                out->notes[0] = 0;
        }
        else if (strcmp(key, "assets") == 0)
        {
            if (WrJsonPeek(&j) != WR_JSON_ARRAY || !WrJsonEnterArray(&j))
            {
                WrJsonSkip(&j);
                continue;
            }
            while (WrJsonNextElement(&j))
            {
                if (WrJsonPeek(&j) != WR_JSON_OBJECT || !WrJsonEnterObject(&j))
                {
                    WrJsonSkip(&j);
                    continue;
                }

                char name[128] = {0};
                char url[512] = {0};
                long long size = 0;
                char akey[64];
                while (WrJsonNextMember(&j, akey, sizeof(akey)))
                {
                    if (strcmp(akey, "name") == 0)
                        WrJsonString(&j, name, sizeof(name));
                    else if (strcmp(akey, "browser_download_url") == 0)
                        WrJsonString(&j, url, sizeof(url));
                    else if (strcmp(akey, "size") == 0)
                        size = WrJsonInt(&j, 0, NULL);
                    else
                        WrJsonSkip(&j);
                }

                if (!url[0])
                    continue;
                if (strcmp(name, WR_UPDATE_DLL) == 0)
                {
                    CopyTrimmed(out->dllUrl, sizeof(out->dllUrl), url);
                    out->dllBytes = size;
                }
                else if (strcmp(name, WR_UPDATE_EXE) == 0)
                {
                    CopyTrimmed(out->exeUrl, sizeof(out->exeUrl), url);
                    out->exeBytes = size;
                }
                else if (strcmp(name, WR_UPDATE_SUMS) == 0)
                {
                    CopyTrimmed(out->sumsUrl, sizeof(out->sumsUrl), url);
                }
            }
        }
        else
        {
            WrJsonSkip(&j);
        }
    }

    if (WrJsonFailed(&j) || !out->tag[0])
        return false;

    out->loose = out->dllUrl[0] && out->exeUrl[0] && out->sumsUrl[0];
    return true;
}

// ---------------------------------------------------------------------------
// The manifest
// ---------------------------------------------------------------------------

bool WrUpdateManifestLookup(const char *text, size_t len, const char *name,
                            char *hexOut, int cap)
{
    if (!text || !name || !hexOut || cap < WR_SHA256_HEX)
        return false;
    hexOut[0] = 0;

    size_t at = 0;
    while (at < len)
    {
        size_t eol = at;
        while (eol < len && text[eol] != '\n')
            eol++;

        // "<64 hex><whitespace><name>", which is what the release workflow
        // writes. Anything else on the line is skipped rather than refused --
        // a manifest that grows a comment must not break this.
        size_t p = at;
        while (p < eol && (text[p] == ' ' || text[p] == '\t'))
            p++;

        size_t hexStart = p;
        while (p < eol && ((text[p] >= '0' && text[p] <= '9') ||
                           (text[p] >= 'a' && text[p] <= 'f') ||
                           (text[p] >= 'A' && text[p] <= 'F')))
            p++;

        if (p - hexStart == WR_SHA256_BYTES * 2 && p < eol &&
            (text[p] == ' ' || text[p] == '\t' || text[p] == '*'))
        {
            while (p < eol && (text[p] == ' ' || text[p] == '\t' ||
                               text[p] == '*'))
                p++;
            size_t nameStart = p;
            size_t nameEnd = eol;
            while (nameEnd > nameStart &&
                   (text[nameEnd - 1] == '\r' || text[nameEnd - 1] == ' ' ||
                    text[nameEnd - 1] == '\t'))
                nameEnd--;

            size_t nameLen = nameEnd - nameStart;
            if (nameLen == strlen(name) &&
                _strnicmp(text + nameStart, name, nameLen) == 0)
            {
                memcpy(hexOut, text + hexStart, WR_SHA256_BYTES * 2);
                hexOut[WR_SHA256_BYTES * 2] = 0;
                for (int i = 0; i < WR_SHA256_BYTES * 2; i++)
                    if (hexOut[i] >= 'A' && hexOut[i] <= 'F')
                        hexOut[i] = (char)(hexOut[i] - 'A' + 'a');
                return true;
            }
        }

        at = eol + 1;
    }
    return false;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
//
// wr_board.cpp's shape: an interlocked busy flag, one worker, and a critical
// section over everything the panel reads. There is only ever one of these
// running, so there is no double buffer to be had -- the UI copies the whole
// struct out in one call and holds nothing.

static CRITICAL_SECTION g_cs;
static bool g_csReady = false;
static volatile LONG g_busy = 0;

static WrUpdateInfo g_info;
static WrUpdateRelease g_rel;
static bool g_infoReady = false;

// Resolved ONCE, on the UI thread, before any worker exists. WrDataPath hands
// back one of four rotating static buffers with no lock, and wr_jobs.h makes
// that safe by rule rather than by hope: resolve on the caller, hand the worker
// absolute paths with nothing left to look up. Same rule here.
static char g_stageDir[MAX_PATH] = {0};
static char g_moduleDir[MAX_PATH] = {0};

static void EnsureCs(void)
{
    if (!g_csReady)
    {
        InitializeCriticalSection(&g_cs);
        g_csReady = true;
    }
}

static void EnsurePaths(void)
{
    if (!g_stageDir[0])
        strcpy_s(g_stageDir, sizeof(g_stageDir), WrDataPath("update"));
    if (!g_moduleDir[0])
        strcpy_s(g_moduleDir, sizeof(g_moduleDir), WrModuleDir());
}

static void SetStatus(WrUpdateStage stage, const char *status,
                      const char *detail)
{
    EnterCriticalSection(&g_cs);
    g_info.stage = stage;
    if (status)
        strncpy_s(g_info.status, sizeof(g_info.status), status, _TRUNCATE);
    strncpy_s(g_info.detail, sizeof(g_info.detail), detail ? detail : "",
              _TRUNCATE);
    LeaveCriticalSection(&g_cs);
}

static void Fail(const char *status, const char *detail, bool antivirus)
{
    EnterCriticalSection(&g_cs);
    g_info.stage = WR_UPDATE_FAILED;
    g_info.antivirus = antivirus;
    strncpy_s(g_info.status, sizeof(g_info.status), status, _TRUNCATE);
    strncpy_s(g_info.detail, sizeof(g_info.detail), detail ? detail : "",
              _TRUNCATE);
    LeaveCriticalSection(&g_cs);
    WrLogf("[update] %s%s%s", status, detail && *detail ? " -- " : "",
           detail ? detail : "");
}

bool WrUpdateBusy(void)
{
    return InterlockedCompareExchange(&g_busy, 0, 0) != 0;
}

void WrUpdateGet(WrUpdateInfo *out)
{
    if (!out)
        return;
    EnsureCs();
    EnterCriticalSection(&g_cs);
    if (!g_infoReady)
    {
        memset(&g_info, 0, sizeof(g_info));
        strcpy_s(g_info.running, sizeof(g_info.running), WRLINES_VERSION);
        strcpy_s(g_info.status, sizeof(g_info.status), "not checked");
        g_infoReady = true;
    }
    *out = g_info;
    LeaveCriticalSection(&g_cs);
}

// ---------------------------------------------------------------------------
// Fetching, hashing, writing
// ---------------------------------------------------------------------------

// Everything that arrives is hashed IN MEMORY and written only if it matched.
// The plan for this feature said to write a .part file and hash that; doing it
// this way round is strictly better and costs nothing, because the largest
// thing here is a two-megabyte DLL: an unverified byte never reaches the disk
// at all, so there is no partial file for anything to find, scan or leave
// behind when a download fails halfway.
static bool Fetch(const char *url, unsigned char **out, size_t *lenOut,
                  const char *what)
{
    char err[192] = {0};
    if (!WrApiGet(url, out, lenOut, err, sizeof(err)))
    {
        char status[192];
        _snprintf_s(status, sizeof(status), _TRUNCATE, "could not fetch %s",
                    what);
        Fail(status, err[0] ? err : "no reply", false);
        return false;
    }
    return true;
}

// Write bytes we have already checked, then read them back off the disk and
// check them again. The second hash is not paranoia about the file system: an
// unsigned, freshly written binary is exactly what real-time protection acts
// on, and a file that is correct in memory and gone from the disk a moment
// later is a specific, nameable thing rather than a mystery.
static bool WriteAndVerify(const char *path, const unsigned char *bytes,
                           size_t len, const char *wantHex, bool *antivirus)
{
    if (antivirus)
        *antivirus = false;

    FILE *f = NULL;
    if (fopen_s(&f, path, "wb") != 0 || !f)
        return false;
    size_t wrote = fwrite(bytes, 1, len, f);
    bool ok = (wrote == len) && (fflush(f) == 0);
    fclose(f);
    if (!ok)
    {
        DeleteFileA(path);
        return false;
    }

    char landed[WR_SHA256_HEX];
    if (!WrSha256File(path, landed, sizeof(landed)))
    {
        // Written, then unreadable. Nothing else does that.
        if (antivirus)
            *antivirus = true;
        return false;
    }
    if (!WrSha256HexEqual(landed, wantHex))
    {
        if (antivirus)
            *antivirus = true;
        DeleteFileA(path);
        return false;
    }
    return true;
}

// "A file we just wrote is gone, or is not the bytes we wrote" has exactly one
// likely cause on Windows and a completely different set on Linux, and naming
// the wrong one sends somebody hunting for a scanner that is not installed.
static const char *kAvDetailWindows =
    "That is what a real-time antivirus scan looks like from in here. The "
    "release page in a browser will do the same thing more visibly, and "
    "reporting the false positive is what actually fixes it -- the link is in "
    "the readme.";

static const char *kAvDetailWine =
    "There is no real-time scanner under Proton, so this is the filesystem "
    "rather than antivirus: a full disk, a Steam library mounted read-only, or "
    "a folder owned by another user. Downloading from the release page by hand "
    "will say which.";

static const char *AvDetail(void)
{
    return WrIsWine() ? kAvDetailWine : kAvDetailWindows;
}

// ---------------------------------------------------------------------------
// Check
// ---------------------------------------------------------------------------

static DWORD WINAPI CheckThread(LPVOID)
{
    char url[256];
    WrApiLatestReleaseUrl(url, sizeof(url));

    unsigned char *body = NULL;
    size_t len = 0;
    if (!Fetch(url, &body, &len, "the release list"))
    {
        InterlockedExchange(&g_busy, 0);
        return 0;
    }

    WrUpdateRelease rel;
    bool parsed = WrUpdateParseRelease((const char *)body, len, &rel);
    free(body);

    if (!parsed)
    {
        Fail("could not read GitHub's reply",
             "The check found the server but not a release in what it sent.",
             false);
        InterlockedExchange(&g_busy, 0);
        return 0;
    }

    int cmp = WrUpdateCompareVersions(rel.tag, WRLINES_VERSION);

    EnterCriticalSection(&g_cs);
    g_rel = rel;
    strncpy_s(g_info.latest, sizeof(g_info.latest), rel.tag, _TRUNCATE);
    strncpy_s(g_info.htmlUrl, sizeof(g_info.htmlUrl), rel.htmlUrl, _TRUNCATE);
    strncpy_s(g_info.notes, sizeof(g_info.notes), rel.notes, _TRUNCATE);
    g_info.bytes = rel.dllBytes + rel.exeBytes;
    g_info.loose = rel.loose;
    g_info.antivirus = false;
    LeaveCriticalSection(&g_cs);

    if (cmp == WR_UPDATE_VERSION_BAD)
    {
        char s[192];
        _snprintf_s(s, sizeof(s), _TRUNCATE,
                    "the newest tag is %s, which is not a plain version number",
                    rel.tag);
        SetStatus(WR_UPDATE_CURRENT, s,
                  "Nothing will be downloaded for a tag this cannot read. "
                  "Open the release page and judge it yourself.");
    }
    else if (cmp <= 0)
    {
        char s[192];
        _snprintf_s(s, sizeof(s), _TRUNCATE, "%s is the newest there is",
                    WRLINES_VERSION);
        SetStatus(WR_UPDATE_CURRENT, s, "");
    }
    else
    {
        char s[192];
        _snprintf_s(s, sizeof(s), _TRUNCATE, "%s is out. You have %s.",
                    rel.tag, WRLINES_VERSION);
        SetStatus(WR_UPDATE_AVAILABLE, s,
                  rel.loose ? ""
                            : "This release does not carry the two loose files "
                              "this can download, so it has to be fetched in a "
                              "browser.");
    }

    WrLogf("[update] checked: running %s, latest %s%s", WRLINES_VERSION,
           rel.tag, rel.loose ? "" : " (no loose assets)");
    InterlockedExchange(&g_busy, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// Download
// ---------------------------------------------------------------------------

static bool StageOne(const char *url, const char *name, const char *manifest,
                     size_t manifestLen, char *hexOut)
{
    char want[WR_SHA256_HEX];
    if (!WrUpdateManifestLookup(manifest, manifestLen, name, want,
                                sizeof(want)))
    {
        char s[192];
        _snprintf_s(s, sizeof(s), _TRUNCATE, "%s is not in " WR_UPDATE_SUMS,
                    name);
        Fail(s, "The release is not self-consistent; nothing was written.",
             false);
        return false;
    }

    unsigned char *bytes = NULL;
    size_t len = 0;
    if (!Fetch(url, &bytes, &len, name))
        return false;

    unsigned char digest[WR_SHA256_BYTES];
    char got[WR_SHA256_HEX];
    WrSha256Buffer(bytes, len, digest);
    WrSha256Hex(digest, got, sizeof(got));

    if (!WrSha256HexEqual(got, want))
    {
        free(bytes);
        char s[192];
        _snprintf_s(s, sizeof(s), _TRUNCATE, "%s did not arrive intact", name);
        Fail(s, "Its digest does not match the release's own manifest, so it "
                "was thrown away rather than written.", false);
        return false;
    }

    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", g_stageDir, name);

    bool av = false;
    bool ok = WriteAndVerify(path, bytes, len, want, &av);
    free(bytes);

    if (!ok)
    {
        char s[192];
        if (av)
            _snprintf_s(s, sizeof(s), _TRUNCATE,
                        "%s was removed as it was written", name);
        else
            _snprintf_s(s, sizeof(s), _TRUNCATE, "could not write %s", name);
        Fail(s, av ? AvDetail() : g_stageDir, av);
        return false;
    }

    strcpy_s(hexOut, WR_SHA256_HEX, want);
    return true;
}

static DWORD WINAPI DownloadThread(LPVOID)
{
    WrUpdateRelease rel;
    EnterCriticalSection(&g_cs);
    rel = g_rel;
    LeaveCriticalSection(&g_cs);

    // Made here and not in EnsurePaths, so that pressing Check leaves the disk
    // alone: WrDataPath creates the directories ALONG a path and not the last
    // element, so resolving "update" makes wrlines_data and stops. One level is
    // all that is left to make.
    CreateDirectoryA(g_stageDir, NULL);

    unsigned char *manifest = NULL;
    size_t manifestLen = 0;
    if (!Fetch(rel.sumsUrl, &manifest, &manifestLen, WR_UPDATE_SUMS))
    {
        InterlockedExchange(&g_busy, 0);
        return 0;
    }

    char dllHex[WR_SHA256_HEX] = {0};
    char exeHex[WR_SHA256_HEX] = {0};
    bool ok = StageOne(rel.dllUrl, WR_UPDATE_DLL, (const char *)manifest,
                       manifestLen, dllHex) &&
              StageOne(rel.exeUrl, WR_UPDATE_EXE, (const char *)manifest,
                       manifestLen, exeHex);
    free(manifest);

    if (!ok)
    {
        InterlockedExchange(&g_busy, 0);
        return 0;
    }

    EnterCriticalSection(&g_cs);
    strcpy_s(g_info.dllHex, sizeof(g_info.dllHex), dllHex);
    strcpy_s(g_info.exeHex, sizeof(g_info.exeHex), exeHex);
    LeaveCriticalSection(&g_cs);

    char s[192];
    _snprintf_s(s, sizeof(s), _TRUNCATE, "%s downloaded, and both digests match",
                rel.tag);
    SetStatus(WR_UPDATE_STAGED, s,
              "Staged in wrlines_data\\update. Nothing outside that folder has "
              "changed yet.");
    WrLogf("[update] staged %s in %s", rel.tag, g_stageDir);

    InterlockedExchange(&g_busy, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

// Can we write here at all? Asked BEFORE anything moves, so somebody who
// unzipped into Program Files is told to move the folder rather than handed a
// half-finished swap.
static bool CanWriteHere(const char *dir)
{
    char probe[MAX_PATH];
    _snprintf_s(probe, sizeof(probe), _TRUNCATE, "%s\\wrlines_write_probe.tmp",
                dir);
    HANDLE h = CreateFileA(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                           NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(h);
    return true;
}

// One file's half of the swap.
//
// The rename is the trick that makes this possible at all: a mapped image
// cannot be written through, but it CAN be renamed, because the loader's own
// handle was opened allowing FILE_SHARE_DELETE and a rename inside one
// directory changes a name rather than the bytes behind the section. So the
// DLL currently executing this line moves itself aside and stays running.
static bool SwapOne(const char *dir, const char *name, const char *stagedPath,
                    const char *wantHex, bool *antivirus, char *errOut,
                    int errCap)
{
    char live[MAX_PATH], old[MAX_PATH];
    _snprintf_s(live, sizeof(live), _TRUNCATE, "%s\\%s", dir, name);
    _snprintf_s(old, sizeof(old), _TRUNCATE, "%s\\%s" WR_UPDATE_OLD_SUFFIX, dir,
                name);

    bool moved = false;
    if (GetFileAttributesA(live) != INVALID_FILE_ATTRIBUTES)
    {
        if (!MoveFileExA(live, old, MOVEFILE_REPLACE_EXISTING))
        {
            _snprintf_s(errOut, (size_t)errCap, _TRUNCATE,
                        "could not move %s aside (%lu)", name, GetLastError());
            return false;
        }
        moved = true;
    }

    // CopyFile rather than MoveFile: the staged pair stays where it is, so a
    // second press after a restart is not needed and the folder is still a
    // record of what was installed.
    if (CopyFileA(stagedPath, live, FALSE))
    {
        char landed[WR_SHA256_HEX];
        if (WrSha256File(live, landed, sizeof(landed)) &&
            WrSha256HexEqual(landed, wantHex))
            return true;

        // It copied and then was not there, or not itself. This is the case the
        // whole shape of this function exists for.
        if (antivirus)
            *antivirus = true;
        _snprintf_s(errOut, (size_t)errCap, _TRUNCATE,
                    "%s did not survive being written", name);
    }
    else
    {
        _snprintf_s(errOut, (size_t)errCap, _TRUNCATE,
                    "could not write %s (%lu)", name, GetLastError());
    }

    // Put it back. Whatever else happened, the user ends this press with the
    // version they started it with.
    DeleteFileA(live);
    if (moved)
        MoveFileExA(old, live, MOVEFILE_REPLACE_EXISTING);
    return false;
}

static DWORD WINAPI InstallThread(LPVOID)
{
    char dllPath[MAX_PATH], exePath[MAX_PATH];
    char dllHex[WR_SHA256_HEX], exeHex[WR_SHA256_HEX];
    char tag[48];

    EnterCriticalSection(&g_cs);
    strcpy_s(dllHex, sizeof(dllHex), g_info.dllHex);
    strcpy_s(exeHex, sizeof(exeHex), g_info.exeHex);
    strcpy_s(tag, sizeof(tag), g_info.latest);
    LeaveCriticalSection(&g_cs);

    _snprintf_s(dllPath, sizeof(dllPath), _TRUNCATE, "%s\\" WR_UPDATE_DLL,
                g_stageDir);
    _snprintf_s(exePath, sizeof(exePath), _TRUNCATE, "%s\\" WR_UPDATE_EXE,
                g_stageDir);

    if (!CanWriteHere(g_moduleDir))
    {
        char s[256];
        _snprintf_s(s, sizeof(s), _TRUNCATE,
                    "cannot write into %s", g_moduleDir);
        Fail(s, WrIsWine()
                    ? "Nothing has been moved. This folder is not writable: a "
                      "Steam library mounted read-only, or a folder owned by "
                      "another user. Put the WrLines folder inside the game's "
                      "own directory and press Install again."
                    : "Nothing has been moved. Windows will not let this folder "
                      "be written to -- somewhere under Program Files usually "
                      "means that. Move the whole folder somewhere in your own "
                      "documents and press Install again.", false);
        InterlockedExchange(&g_busy, 0);
        return 0;
    }

    // The staged files must still be the ones that were checked. Between the
    // download and this press a scanner has had time to act on them.
    char now[WR_SHA256_HEX];
    if (!WrSha256File(dllPath, now, sizeof(now)) ||
        !WrSha256HexEqual(now, dllHex) ||
        !WrSha256File(exePath, now, sizeof(now)) ||
        !WrSha256HexEqual(now, exeHex))
    {
        Fail("the staged files are gone or have changed", AvDetail(), true);
        InterlockedExchange(&g_busy, 0);
        return 0;
    }

    bool av = false;
    char err[192] = {0};

    // The DLL first. If the injector fails after it, the pair is mismatched for
    // one press -- but wrinject.exe only loads whatever DLL is beside it, so a
    // new DLL with an old injector still works, and the other order would leave
    // a new injector with an old DLL, which is the same thing. Either way the
    // rollback below returns both.
    if (!SwapOne(g_moduleDir, WR_UPDATE_DLL, dllPath, dllHex, &av, err,
                 sizeof(err)))
    {
        Fail(err, av ? AvDetail()
                     : "Nothing changed -- the old file was put back.", av);
        InterlockedExchange(&g_busy, 0);
        return 0;
    }

    if (!SwapOne(g_moduleDir, WR_UPDATE_EXE, exePath, exeHex, &av, err,
                 sizeof(err)))
    {
        // Undo the DLL too, so a failed install leaves a matched pair.
        char live[MAX_PATH], old[MAX_PATH];
        _snprintf_s(live, sizeof(live), _TRUNCATE, "%s\\" WR_UPDATE_DLL,
                    g_moduleDir);
        _snprintf_s(old, sizeof(old), _TRUNCATE,
                    "%s\\" WR_UPDATE_DLL WR_UPDATE_OLD_SUFFIX, g_moduleDir);
        DeleteFileA(live);
        MoveFileExA(old, live, MOVEFILE_REPLACE_EXISTING);

        Fail(err, av ? AvDetail()
                     : "Nothing changed -- both old files were put back.", av);
        InterlockedExchange(&g_busy, 0);
        return 0;
    }

    char s[192];
    _snprintf_s(s, sizeof(s), _TRUNCATE, "%s installed", tag[0] ? tag : "it");
    SetStatus(WR_UPDATE_INSTALLED, s,
              WrIsWine()
                  ? "Close the game and start it again -- if you set the Proton "
                    "launch option from the readme, that re-injects on its own. "
                    "This DLL never unloads, so the version you are looking at "
                    "now keeps running until you do. The pair it replaced is "
                    "beside it as .old."
                  : "Close the game and run wrinject.exe again. This DLL never "
                    "unloads, so the version you are looking at now keeps "
                    "running until you do. The pair it replaced is beside it "
                    "as .old.");
    WrLogf("[update] installed %s into %s", tag, g_moduleDir);

    InterlockedExchange(&g_busy, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// The three presses
// ---------------------------------------------------------------------------

// The stage is set BEFORE the thread exists, not after. The other order is a
// real race and not a theoretical one: a check that fails on the first syscall
// gets to WR_UPDATE_FAILED while this function is still between CreateThread
// and its own SetStatus, and the failure would be overwritten with "asking
// GitHub" and sit there for ever.
static void Start(LPTHREAD_START_ROUTINE fn, WrUpdateStage stage,
                  const char *status)
{
    SetStatus(stage, status, "");
    HANDLE h = CreateThread(NULL, 0, fn, NULL, 0, NULL);
    if (h)
    {
        CloseHandle(h);
        return;
    }
    Fail("could not start the worker", "", false);
    InterlockedExchange(&g_busy, 0);
}

// Every press goes through here: resolve the paths on the UI thread, make sure
// the struct the worker writes into exists, and claim the busy flag.
static bool Claim(void)
{
    EnsureCs();
    EnsurePaths();
    WrUpdateInfo scratch;
    WrUpdateGet(&scratch);                  // lazily fills g_info the first time
    return InterlockedCompareExchange(&g_busy, 1, 0) == 0;
}

void WrUpdateCheck(void)
{
    if (!Claim())
        return;

    EnterCriticalSection(&g_cs);
    g_info.antivirus = false;
    g_info.dllHex[0] = g_info.exeHex[0] = 0;
    LeaveCriticalSection(&g_cs);

    Start(CheckThread, WR_UPDATE_CHECKING, "asking GitHub");
}

void WrUpdateDownload(void)
{
    EnsureCs();
    EnsurePaths();
    WrUpdateInfo cur;
    WrUpdateGet(&cur);
    if (cur.stage != WR_UPDATE_AVAILABLE || !cur.loose)
        return;
    if (!Claim())
        return;
    Start(DownloadThread, WR_UPDATE_DOWNLOADING, "downloading");
}

void WrUpdateInstall(void)
{
    EnsureCs();
    EnsurePaths();
    WrUpdateInfo cur;
    WrUpdateGet(&cur);
    if (cur.stage != WR_UPDATE_STAGED)
        return;
    if (!Claim())
        return;
    Start(InstallThread, WR_UPDATE_INSTALLING, "installing");
}

// ---------------------------------------------------------------------------
// The sweep
// ---------------------------------------------------------------------------

void WrUpdateSweepOld(void)
{
    static const char *names[] = { WR_UPDATE_DLL, WR_UPDATE_EXE };
    const char *dir = WrModuleDir();

    for (int i = 0; i < 2; i++)
    {
        char old[MAX_PATH];
        _snprintf_s(old, sizeof(old), _TRUNCATE, "%s\\%s" WR_UPDATE_OLD_SUFFIX,
                    dir, names[i]);
        if (GetFileAttributesA(old) == INVALID_FILE_ATTRIBUTES)
            continue;
        if (DeleteFileA(old))
            WrLogf("[update] swept %s from a previous install", old);
        else
            WrLogf("[update] %s is still there and would not delete (%lu)", old,
                   GetLastError());
    }
}
