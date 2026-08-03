// wr_extract.cpp  --  see wr_extract.h.

#include "wr_extract.h"
#include "wr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Must match wrpath_extract.py: MMTV_MAGIC and OFF_MAPNAME.
#define MTV_MAGIC "MMTV"
#define MTV_MAPNAME_OFF 0x10
#define MTV_MAPNAME_MAX 64
#define MTV_PEEK_BYTES 0x50

#define WR_EXTRACT_LINES 400
#define WR_EXTRACT_LINE_MAX 200

// Recorded failures. Written by wrpath_extract.py, read here only.
#define WR_FAILED_FILE "_failed.txt"
#define WR_MAX_FAILED 512

static CRITICAL_SECTION g_cs;
static bool g_csReady = false;

static char g_map[72] = {0};

static volatile LONG g_counting = 0;
static bool g_haveCounts = false;
static int g_forThisMap = 0;
static int g_alreadyDone = 0;
static int g_notYetDone = 0;
static int g_knownBad = 0;

static volatile LONG g_running = 0;
static volatile LONG g_finished = 0;
static HANDLE g_proc = NULL;
static HANDLE g_readThread = NULL;
static HANDLE g_countThread = NULL;

static char g_lines[WR_EXTRACT_LINES][WR_EXTRACT_LINE_MAX];
static int g_lineCount = 0;

static char g_interp[MAX_PATH] = "not looked for yet";
static bool g_interpIsPy = false;
static bool g_interpFound = false;
static bool g_interpChecked = false;

static void EnsureCs(void)
{
    if (!g_csReady)
    {
        InitializeCriticalSection(&g_cs);
        g_csReady = true;
    }
}

static void PushLine(const char *s)
{
    EnterCriticalSection(&g_cs);
    if (g_lineCount >= WR_EXTRACT_LINES)
    {
        memmove(g_lines[0], g_lines[1],
                sizeof(g_lines[0]) * (WR_EXTRACT_LINES - 1));
        g_lineCount = WR_EXTRACT_LINES - 1;
    }
    strncpy_s(g_lines[g_lineCount], WR_EXTRACT_LINE_MAX, s, _TRUNCATE);
    g_lineCount++;
    LeaveCriticalSection(&g_cs);
}

// ---------------------------------------------------------------------------
// Counting
// ---------------------------------------------------------------------------

// Read the map name out of a demo's header without decompressing anything.
static bool PeekMap(const char *path, char *out, int outLen)
{
    HANDLE h = CreateFileA(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    unsigned char buf[MTV_PEEK_BYTES];
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf), &got, NULL);
    CloseHandle(h);
    if (!ok || got < MTV_MAPNAME_OFF + 8)
        return false;
    if (memcmp(buf, MTV_MAGIC, 4) != 0)
        return false;

    int n = 0;
    while (n < MTV_MAPNAME_MAX && (int)(MTV_MAPNAME_OFF + n) < (int)got &&
           buf[MTV_MAPNAME_OFF + n] != 0)
    {
        unsigned char c = buf[MTV_MAPNAME_OFF + n];
        if (c < 0x20 || c > 0x7E)
            return false;
        n++;
    }
    if (n <= 0 || n >= outLen)
        return false;
    memcpy(out, buf + MTV_MAPNAME_OFF, (size_t)n);
    out[n] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// The failure record
// ---------------------------------------------------------------------------
//
// paths\<map>\_failed.txt, one demo per line:
//
//     revision <TAB> bytes <TAB> basename <TAB> why
//
// Read on the counting thread and held for the length of one count. The size is
// checked as well as the name so that a re-downloaded demo -- a different file
// that happens to have the same name -- is not written off on the strength of
// the old one's failure.

struct FailedRec
{
    char base[80];
    long long size;
};

static FailedRec *g_failed = NULL;
static int g_failedCount = 0;

static void LoadFailures(const char *pathsDir)
{
    free(g_failed);
    g_failed = NULL;
    g_failedCount = 0;

    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", pathsDir, WR_FAILED_FILE);

    FILE *f = NULL;
    if (fopen_s(&f, path, "r") != 0 || !f)
        return;

    g_failed = (FailedRec *)malloc(sizeof(FailedRec) * WR_MAX_FAILED);
    if (!g_failed)
    {
        fclose(f);
        return;
    }

    char line[512];
    while (fgets(line, sizeof(line), f) && g_failedCount < WR_MAX_FAILED)
    {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;

        // revision \t size \t basename \t why
        char *tab1 = strchr(line, '\t');
        if (!tab1) continue;
        char *tab2 = strchr(tab1 + 1, '\t');
        if (!tab2) continue;
        char *tab3 = strchr(tab2 + 1, '\t');
        if (!tab3) continue;
        *tab1 = *tab2 = *tab3 = '\0';

        if (atoi(line) != WR_EXTRACTOR_REVISION)
            continue;           // written by a different extractor; retry it

        FailedRec *r = &g_failed[g_failedCount];
        r->size = _atoi64(tab1 + 1);
        strncpy_s(r->base, sizeof(r->base), tab2 + 1, _TRUNCATE);
        g_failedCount++;
    }
    fclose(f);
}

// Which extractor wrote this .wrpath. The revision sits at offset 0xFC of the
// 0x100-byte header; files written before that field existed read as 0.
//
// This is checked rather than mere existence because an out-of-date .wrpath is
// not "done" -- surf_colin_blaster_69000 had 75 of them, all extracted under a
// world limit half the size of the map, all present and mostly wrong. Reporting
// those as extracted is how they stayed wrong.
static unsigned int WrpathRevision(const char *path)
{
    HANDLE h = CreateFileA(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;

    unsigned int rev = 0;
    DWORD got = 0;
    if (SetFilePointer(h, 0xFC, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER)
    {
        if (!ReadFile(h, &rev, sizeof(rev), &got, NULL) || got != sizeof(rev))
            rev = 0;
    }
    CloseHandle(h);
    return rev;
}

static bool IsKnownBad(const char *base, long long size)
{
    for (int i = 0; i < g_failedCount; i++)
        if (g_failed[i].size == size && _stricmp(g_failed[i].base, base) == 0)
            return true;
    return false;
}

static void CountInTree(const char *root, const char *map, const char *pathsDir)
{
    char pattern[MAX_PATH];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*", root);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;

    do {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == '\0' ||
             (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
            continue;

        char full[MAX_PATH];
        _snprintf_s(full, sizeof(full), _TRUNCATE, "%s\\%s", root, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            CountInTree(full, map, pathsDir);
            continue;
        }

        const char *dot = strrchr(fd.cFileName, '.');
        if (!dot || _stricmp(dot, ".mtv") != 0)
            continue;

        char demoMap[80];
        if (!PeekMap(full, demoMap, sizeof(demoMap)))
            continue;
        if (_stricmp(demoMap, map) != 0)
            continue;

        g_forThisMap++;

        // The extractor writes <out>\<map>\<source basename>.wrpath, so the
        // question "has this one been done" is a single file-exists check.
        char base[MAX_PATH];
        strcpy_s(base, sizeof(base), fd.cFileName);
        char *bdot = strrchr(base, '.');
        if (bdot)
            *bdot = '\0';

        char wrp[MAX_PATH];
        _snprintf_s(wrp, sizeof(wrp), _TRUNCATE, "%s\\%s.wrpath", pathsDir, base);
        if (GetFileAttributesA(wrp) != INVALID_FILE_ATTRIBUTES)
        {
            if (WrpathRevision(wrp) == WR_EXTRACTOR_REVISION)
                g_alreadyDone++;
            else
                g_notYetDone++;     // out of date counts as work still to do
        }
        else if (IsKnownBad(base, ((long long)fd.nFileSizeHigh << 32) |
                                  fd.nFileSizeLow))
            g_knownBad++;
        else
            g_notYetDone++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static DWORD WINAPI CountThread(LPVOID)
{
    // Four thousand small reads, and it runs on every map change -- exactly when
    // the game is busiest. Get out of its way.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    char map[72];
    EnterCriticalSection(&g_cs);
    strcpy_s(map, sizeof(map), g_map);
    LeaveCriticalSection(&g_cs);

    g_forThisMap = g_alreadyDone = g_notYetDone = g_knownBad = 0;

    if (map[0] && WrGameDir()[0])
    {
        char rel[MAX_PATH];
        _snprintf_s(rel, sizeof(rel), _TRUNCATE, "paths\\%s", map);
        char pathsDir[MAX_PATH];
        strcpy_s(pathsDir, sizeof(pathsDir), WrDataPath(rel));

        LoadFailures(pathsDir);

        char root[MAX_PATH];
        _snprintf_s(root, sizeof(root), _TRUNCATE, "%s\\momentum\\momtv", WrGameDir());
        CountInTree(root, map, pathsDir);

        free(g_failed);
        g_failed = NULL;
        g_failedCount = 0;
    }

    g_haveCounts = true;
    InterlockedExchange(&g_counting, 0);
    if (g_notYetDone > 0 || g_knownBad > 0)
        WrLogf("extract: %s has %d demo%s, %d extracted, %d new, %d that failed "
               "before", map, g_forThisMap, g_forThisMap == 1 ? "" : "s",
               g_alreadyDone, g_notYetDone, g_knownBad);
    return 0;
}

void WrExtractOnMapChanged(const char *map)
{
    EnsureCs();
    EnterCriticalSection(&g_cs);
    strcpy_s(g_map, sizeof(g_map), map ? map : "");
    g_haveCounts = false;
    LeaveCriticalSection(&g_cs);

    if (InterlockedCompareExchange(&g_counting, 1, 0) != 0)
        return;     // one already in flight; it will finish with the old map
    if (g_countThread)
    {
        CloseHandle(g_countThread);
        g_countThread = NULL;
    }
    g_countThread = CreateThread(NULL, 0, CountThread, NULL, 0, NULL);
    if (!g_countThread)
        InterlockedExchange(&g_counting, 0);
}

bool WrExtractCounts(int *forThisMap, int *alreadyDone, int *notYetDone,
                     int *knownBad)
{
    if (!g_haveCounts)
        return false;
    if (forThisMap) *forThisMap = g_forThisMap;
    if (alreadyDone) *alreadyDone = g_alreadyDone;
    if (notYetDone) *notYetDone = g_notYetDone;
    if (knownBad) *knownBad = g_knownBad;
    return true;
}

// ---------------------------------------------------------------------------
// Running the script
// ---------------------------------------------------------------------------

static void FindInterpreter(void)
{
    if (g_interpChecked)
        return;
    g_interpChecked = true;

    char found[MAX_PATH];
    // The launcher first: it is what a normal Windows Python install puts on
    // PATH, and "py -3" picks a sane interpreter even with several installed.
    if (SearchPathA(NULL, "py.exe", NULL, MAX_PATH, found, NULL))
    {
        strcpy_s(g_interp, sizeof(g_interp), found);
        g_interpIsPy = true;
        g_interpFound = true;
    }
    else if (SearchPathA(NULL, "python.exe", NULL, MAX_PATH, found, NULL))
    {
        strcpy_s(g_interp, sizeof(g_interp), found);
        g_interpIsPy = false;
        g_interpFound = true;
    }
    else
    {
        strcpy_s(g_interp, sizeof(g_interp),
                 "no py.exe or python.exe on PATH -- install Python, or run the "
                 "script yourself");
        g_interpFound = false;
    }
    WrLogf("extract: interpreter %s", g_interp);
}

const char *WrExtractInterpreter(void) { return g_interp; }
bool WrExtractRunning(void) { return g_running != 0; }

struct ReadCtx { HANDLE pipe; };

static DWORD WINAPI ReadThread(LPVOID param)
{
    ReadCtx *ctx = (ReadCtx *)param;
    char partial[WR_EXTRACT_LINE_MAX];
    int used = 0;

    for (;;)
    {
        char buf[1024];
        DWORD got = 0;
        if (!ReadFile(ctx->pipe, buf, sizeof(buf), &got, NULL) || got == 0)
            break;
        for (DWORD i = 0; i < got; i++)
        {
            char c = buf[i];
            if (c == '\r')
                continue;
            if (c == '\n' || used >= WR_EXTRACT_LINE_MAX - 1)
            {
                partial[used] = '\0';
                if (used > 0)
                    PushLine(partial);
                used = 0;
                if (c != '\n')
                    partial[used++] = c;
            }
            else
            {
                partial[used++] = c;
            }
        }
    }
    if (used > 0)
    {
        partial[used] = '\0';
        PushLine(partial);
    }

    CloseHandle(ctx->pipe);
    free(ctx);

    if (g_proc)
    {
        WaitForSingleObject(g_proc, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(g_proc, &code);
        char msg[128];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "--- finished, exit code %lu ---", code);
        PushLine(msg);
        WrLogf("extract: finished, exit code %lu", code);
        CloseHandle(g_proc);
        g_proc = NULL;
    }

    InterlockedExchange(&g_finished, 1);
    InterlockedExchange(&g_running, 0);
    return 0;
}

void WrExtractRun(bool retryFailed)
{
    EnsureCs();
    FindInterpreter();
    if (!g_interpFound)
    {
        // Say so in the panel. Returning silently makes the button look broken,
        // which is the worst way to find out Python is not installed.
        PushLine(g_interp);
        return;
    }
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0)
        return;

    char map[72];
    EnterCriticalSection(&g_cs);
    strcpy_s(map, sizeof(map), g_map);
    g_lineCount = 0;
    LeaveCriticalSection(&g_cs);

    if (!map[0])
    {
        InterlockedExchange(&g_running, 0);
        return;
    }

    char script[MAX_PATH];
    _snprintf_s(script, sizeof(script), _TRUNCATE, "%s\\wrpath_extract.py",
                WrModuleDir());
    if (GetFileAttributesA(script) == INVALID_FILE_ATTRIBUTES)
    {
        PushLine("wrpath_extract.py is not next to wrlines.dll");
        InterlockedExchange(&g_running, 0);
        return;
    }

    // -u because Python block-buffers stdout when it is not a terminal, and
    // without it the panel would show nothing at all until the run ended.
    char cmd[2048];
    _snprintf_s(cmd, sizeof(cmd), _TRUNCATE,
                "\"%s\" %s-u \"%s\" --map \"%s\" --game \"%s\" --skip-existing%s",
                g_interp, g_interpIsPy ? "-3 " : "", script, map, WrGameDir(),
                retryFailed ? " --retry-failed" : "");

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 64 * 1024))
    {
        PushLine("could not create a pipe for the script's output");
        InterlockedExchange(&g_running, 0);
        return;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = NULL;

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    // BELOW_NORMAL so a long extraction does not fight the game for CPU, and
    // CREATE_NO_WINDOW so no console flashes up over a fullscreen game.
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                             NULL, WrModuleDir(), &si, &pi);
    CloseHandle(wr);        // ours must go, or the read never sees EOF

    if (!ok)
    {
        CloseHandle(rd);
        char msg[256];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "could not start the interpreter (error %lu)", GetLastError());
        PushLine(msg);
        WrLogf("[!] extract: CreateProcess failed (%lu)", GetLastError());
        InterlockedExchange(&g_running, 0);
        return;
    }

    CloseHandle(pi.hThread);
    g_proc = pi.hProcess;

    char msg[256];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE, "running: %s", cmd);
    PushLine(msg);
    WrLogf("extract: started for \"%s\"", map);

    ReadCtx *ctx = (ReadCtx *)malloc(sizeof(ReadCtx));
    if (!ctx)
    {
        CloseHandle(rd);
        InterlockedExchange(&g_running, 0);
        return;
    }
    ctx->pipe = rd;

    if (g_readThread)
    {
        CloseHandle(g_readThread);
        g_readThread = NULL;
    }
    g_readThread = CreateThread(NULL, 0, ReadThread, ctx, 0, NULL);
    if (!g_readThread)
    {
        CloseHandle(rd);
        free(ctx);
        InterlockedExchange(&g_running, 0);
    }
}

int WrExtractLineCount(void)
{
    if (!g_csReady)
        return 0;
    EnterCriticalSection(&g_cs);
    int n = g_lineCount;
    LeaveCriticalSection(&g_cs);
    return n;
}

const char *WrExtractLine(int i)
{
    static char buf[WR_EXTRACT_LINE_MAX];
    if (!g_csReady)
        return "";
    EnterCriticalSection(&g_cs);
    if (i >= 0 && i < g_lineCount)
        strcpy_s(buf, sizeof(buf), g_lines[i]);
    else
        buf[0] = '\0';
    LeaveCriticalSection(&g_cs);
    return buf;
}

bool WrExtractTakeFinished(void)
{
    return InterlockedExchange(&g_finished, 0) != 0;
}

void WrExtractShutdown(void)
{
    // Deliberately does not kill a running script. It writes .wrpath files, and
    // stopping it halfway through one is how you get a truncated file that the
    // loader then has to reject. It is a separate process; letting it finish
    // costs nothing.
}
