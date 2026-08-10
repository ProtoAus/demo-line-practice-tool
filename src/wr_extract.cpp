// wr_extract.cpp  --  see wr_extract.h.

#include "wr_extract.h"
#include "wr_api.h"
#include "wr_maps.h"
#include "wr_mtv.h"
#include "wr_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static int g_headerBad = 0;     // of g_knownBad, how many failed the header gates

static volatile LONG g_running = 0;
static volatile LONG g_finished = 0;
// Both guarded by g_cs. The UI thread reads them to stop a run and ReadThread
// closes them when one ends; unsynchronised, those two interleave badly.
static HANDLE g_proc = NULL;
static HANDLE g_job = NULL;
static volatile LONG g_stopped = 0;     // this run ended because Stop was pressed
static volatile LONG g_cancel = 0;      // asked to stop; the native path polls it
static HANDLE g_readThread = NULL;
static HANDLE g_countThread = NULL;
static HANDLE g_nativeThread = NULL;

// The job in flight, and the one that most recently ended. Both guarded by g_cs.
// g_generation is bumped once per ending and read without the lock, which is all
// an edge detector needs.
static WrExtractRequest g_req = {WR_JOB_NONE};
static int *g_ranks = NULL;
static int g_rankCount = 0;
static WrJobKind g_lastKind = WR_JOB_NONE;
static volatile LONG g_generation = 0;

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

// Every progress line in this file goes through here rather than straight to
// PushLine. See WrExtractSetEmit in the header for why -- in short, it is what
// lets a console front end run the same code and have its output diffed against
// the reference implementation's.
static WrEmitFn g_emit = NULL;

static void Emit(const char *s)
{
    WrEmitFn fn = g_emit;
    if (fn)
        fn(s);
    else
        PushLine(s);
}

static void Emitf(const char *fmt, ...)
{
    char buf[WR_EXTRACT_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    Emit(buf);
}

void WrExtractSetEmit(WrEmitFn fn) { g_emit = fn; }

// ---------------------------------------------------------------------------
// Counting
// ---------------------------------------------------------------------------

// There used to be a PeekMap here: forty lines that opened a demo, checked the
// magic, and copied the map name out of offset 0x10. It was a partial, drifted
// copy of the reference implementation's header parser -- it read one field of
// eleven, applied none of the sanity gates, and had a printable-ASCII test the
// reference does not have. Two parsers for one format is one parser too many,
// and the one that was wrong was always going to be this one.
//
// WrMtvPeek is still one read and still no allocation -- 512 bytes now rather
// than 80, which is inside the same disk block either way -- and it parses all
// of it. What that buys the counter is the subject of the knownBad comment in
// wr_extract.h: a demo whose header does not survive the gates can never be
// extracted, and now says so at once instead of after a run has spent the time
// finding out.
//
// Measured over the 6249 demos on this machine, nothing changed hands: the same
// files are counted, under the same map names, and none fails a gate. That is
// the result to want. The gates are there for the day the format moves.

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

        // A failed peek still fills in every field it managed to read before
        // the gate that refused it -- see WrMtvPeek. So a demo with a broken
        // header is still a demo FOR a map, and is counted against that map
        // rather than vanishing from the total. An empty name is the other
        // case: the file is not an MMTV at all and there is nothing to count
        // it against.
        char why[128];
        WrMtvHeader hdr;
        bool headerOk = WrMtvPeek(full, &hdr, why, sizeof(why));
        if (!hdr.map[0])
            continue;
        if (_stricmp(hdr.map, map) != 0)
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
        else if (!headerOk ||
                 IsKnownBad(base, ((long long)fd.nFileSizeHigh << 32) |
                                  fd.nFileSizeLow))
        {
            g_knownBad++;
            // Nothing in this library has ever failed a header gate, so if one
            // does, the reason is worth having written down somewhere. Capped
            // because a genuinely broken install would otherwise put a line in
            // the log for every demo it owns, every time you change map.
            if (!headerOk)
            {
                g_headerBad++;
                if (g_headerBad <= 5)
                    WrLogf("[!] extract: %s -- %s", fd.cFileName, why);
            }
        }
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
    g_headerBad = 0;

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

        // Demos we fetched ourselves. They live under wrlines_data because
        // nothing here writes into the game install, so they are a second tree
        // rather than more files in momtv.
        CountInTree(WrDataPath("demos"), map, pathsDir);

        free(g_failed);
        g_failed = NULL;
        g_failedCount = 0;
    }

    g_haveCounts = true;
    InterlockedExchange(&g_counting, 0);
    if (g_notYetDone > 0 || g_knownBad > 0)
        WrLogf("extract: %s has %d demo%s, %d extracted, %d new, %d that failed "
               "before (%d of those on the header alone)",
               map, g_forThisMap, g_forThisMap == 1 ? "" : "s",
               g_alreadyDone, g_notYetDone, g_knownBad, g_headerBad);
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
    // python3.exe last, and it is here for Wine. A Proton prefix has no Windows
    // Python launcher in it, and whatever Python is reachable from inside the
    // prefix is as likely to be named this as anything else. It costs one more
    // PATH search on a machine that has neither of the two above.
    else if (SearchPathA(NULL, "python3.exe", NULL, MAX_PATH, found, NULL))
    {
        strcpy_s(g_interp, sizeof(g_interp), found);
        g_interpIsPy = false;
        g_interpFound = true;
    }
    else if (WrIsWine())
    {
        strcpy_s(g_interp, sizeof(g_interp),
                 "no Python inside this Wine prefix -- the game's prefix needs "
                 "its own; a Python installed on the Linux side is not on this "
                 "PATH. See the README's Linux section.");
        g_interpFound = false;
    }
    else
    {
        strcpy_s(g_interp, sizeof(g_interp),
                 "no py.exe or python.exe on PATH -- install Python, or run the "
                 "script yourself");
        g_interpFound = false;
    }
    WrLogf("extract: interpreter %s%s", g_interp, WrIsWine() ? " (under Wine)" : "");
}

const char *WrExtractStatus(void) { return g_interp; }
bool WrExtractRunning(void) { return g_running != 0; }

unsigned int WrExtractRunGeneration(void) { return (unsigned int)g_generation; }

WrJobKind WrExtractLastKind(void)
{
    if (!g_csReady)
        return WR_JOB_NONE;
    EnterCriticalSection(&g_cs);
    WrJobKind k = g_lastKind;
    LeaveCriticalSection(&g_cs);
    return k;
}

// The single exit. Every way a run can end -- clean, stopped, or failed before
// it ever started -- comes through here.
//
// It used to be five places, all of them clearing g_running and only one of them
// pushing a terminator or setting g_finished. That is why a launch that failed
// early left the panel with an error and no full stop, and why the UI's
// "did something just finish" edge could miss a job entirely: g_running was set
// and cleared inside one call, between frames.
static void EndRun(DWORD exitCode)
{
    char msg[128];
    if (InterlockedExchange(&g_stopped, 0))
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "--- stopped ---");
    else
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "--- finished, exit code %lu ---", exitCode);
    Emit(msg);
    WrLogf("extract: %s (exit code %lu)", msg, exitCode);

    EnterCriticalSection(&g_cs);
    g_lastKind = g_req.kind;
    free(g_ranks);
    g_ranks = NULL;
    g_rankCount = 0;
    LeaveCriticalSection(&g_cs);

    InterlockedExchange(&g_cancel, 0);
    InterlockedIncrement(&g_generation);
    InterlockedExchange(&g_finished, 1);
    InterlockedExchange(&g_running, 0);
}

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
                    Emit(partial);
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
        Emit(partial);
    }

    CloseHandle(ctx->pipe);
    free(ctx);

    // The handle is taken under the lock and nulled there before it is used, so
    // a Stop pressed on the UI thread can never be holding it at the moment
    // this closes it. Without that, the two threads race on g_proc and the
    // worst case is TerminateProcess on a REUSED handle -- something else's
    // process, killed by us, with no way to tell afterwards.
    HANDLE proc = NULL, job = NULL;
    EnterCriticalSection(&g_cs);
    proc = g_proc;
    job = g_job;
    g_proc = NULL;
    g_job = NULL;
    LeaveCriticalSection(&g_cs);

    DWORD code = 1;
    if (proc)
    {
        WaitForSingleObject(proc, INFINITE);
        GetExitCodeProcess(proc, &code);
        CloseHandle(proc);
    }
    if (job)
        CloseHandle(job);

    EndRun(code);
    return 0;
}

// A launch that got as far as a running child but no further. Kill it and let
// go of everything, rather than reporting a failure and orphaning both the
// handle and the process. The caller still has the latch and still ends the run.
static void Abandon(void)
{
    HANDLE proc = NULL, job = NULL;
    EnterCriticalSection(&g_cs);
    proc = g_proc;
    job = g_job;
    g_proc = NULL;
    g_job = NULL;
    LeaveCriticalSection(&g_cs);

    if (job)
    {
        TerminateJobObject(job, 1);
        CloseHandle(job);
    }
    else if (proc)
    {
        TerminateProcess(proc, 1);
    }
    if (proc)
        CloseHandle(proc);
}

void WrExtractStop(void)
{
    // The native path has no process to kill: it polls this and unwinds at the
    // next check. Set it first and unconditionally, so a Stop pressed in the
    // window between claiming the latch and the backend starting is not lost.
    InterlockedExchange(&g_cancel, 1);

    // Terminate the JOB, not the process.
    //
    // The extractor runs a worker pool -- cores minus two by default -- and
    // those are grandchildren we hold no handles for. Killing only the parent
    // leaves them burning a core each until their queue pipe breaks, and a
    // worker mid-demo finishes that demo first. A kill-on-close job takes the
    // whole tree at once.
    //
    // Everything after this is the ordinary path: the child's end of the pipe
    // closes, the blocked ReadFile returns, and ReadThread tears down exactly
    // as it would after a clean exit.
    HANDLE job = NULL, proc = NULL;
    EnterCriticalSection(&g_cs);
    job = g_job;
    proc = g_proc;
    LeaveCriticalSection(&g_cs);

    if (!proc && !job)
    {
        // No child. Either nothing is running -- the flag set above is cleared
        // by the next EndRun and does no harm -- or the job in flight is a
        // native one, which has nothing to kill and stops itself.
        if (g_running)
        {
            InterlockedExchange(&g_stopped, 1);
            Emit("--- stopping; the work in flight has to reach its next "
                 "checkpoint ---");
            WrLogf("extract: stop -- cooperative");
        }
        return;
    }

    InterlockedExchange(&g_stopped, 1);

    if (job && TerminateJobObject(job, 1))
    {
        Emit("--- stopping, and taking the worker processes with it ---");
        WrLogf("extract: stop -- TerminateJobObject");
        return;
    }

    // No job, or the job refused. Say which, rather than reporting a clean
    // stop and leaving python.exe processes running behind the panel.
    if (proc && TerminateProcess(proc, 1))
    {
        Emit("--- stopping the extractor; its worker processes may take a "
             "moment longer ---");
        WrLogf("extract: stop -- TerminateProcess only (no job object)");
    }
}

static int g_timeout = WR_EXTRACT_TIMEOUT_DEFAULT;

void WrExtractSetTimeout(int seconds) { g_timeout = seconds < 0 ? 0 : seconds; }
int WrExtractTimeout(void) { return g_timeout; }

// ---------------------------------------------------------------------------
// The python backend
// ---------------------------------------------------------------------------
//
// Everything from here to WrExtractSubmit goes when the last verb is ported.
// It is kept behind one function -- BuildPythonArgs -- so that the typed request
// and the command line it used to be can be checked against each other by
// tests\test_seam.exe for as long as both exist.

// The ticked places, through a file rather than argv.
//
// A selection has no natural bound -- "tick all" on a 20000-row board is a
// legitimate thing to press -- and a command line has a 2048-byte one. This is
// the workaround, and it exists only because the backend is a separate process;
// when the fetcher is C the ranks array goes straight through and this goes.
static bool WritePickFile(const WrExtractRequest *req, char *out, int cap)
{
    char rel[192];
    _snprintf_s(rel, sizeof(rel), _TRUNCATE, "boards\\%s_g%d_t%d%d.pick",
                req->map, req->gamemode, req->trackType, req->trackNum);
    strncpy_s(out, cap, WrDataPath(rel), _TRUNCATE);

    FILE *f = NULL;
    if (fopen_s(&f, out, "w") != 0 || !f)
        return false;
    fprintf(f, "# WrLines: the places ticked in the Board tab, for --ranks-file.\n");
    for (int i = 0; i < req->rankCount; i++)
        fprintf(f, "%d\n", req->ranks[i]);
    fclose(f);
    return req->rankCount > 0;
}

// The request, as the flags wrpath_extract.py expects.
//
// Byte-for-byte what the call sites used to format themselves -- that is the
// whole point of it being one function, and test_seam holds it to it. `needsMap`
// says whether the caller must also prepend --map for the map being played;
// extraction is about where you are standing, the other three name their map in
// the flags because the panel lets you point them at any map.
static bool BuildPythonArgs(const WrExtractRequest *req, char *out, int cap,
                            bool *needsMap)
{
    *needsMap = false;
    out[0] = '\0';

    switch (req->kind)
    {
    case WR_JOB_EXTRACT:
        *needsMap = true;
        _snprintf_s(out, cap, _TRUNCATE, "--skip-existing%s --timeout %d",
                    req->retryFailed ? " --retry-failed" : "",
                    req->timeoutSeconds);
        return true;

    case WR_JOB_INDEX_MAPS:
        // Ported: NativeHandles claims this one, so nothing in the DLL reaches
        // here any more. Kept because it is the recorded contract -- test_seam
        // is what proves the struct produces the argv the call sites used to
        // build, and deleting the line would delete the record for one saved
        // line of a function that goes whole at the end of the port.
        strncpy_s(out, cap, "--index-maps", _TRUNCATE);
        return true;

    case WR_JOB_BOARD:
    {
        // Ported at v0.6.0, and kept for the same reason --index-maps above is:
        // this function is the recorded contract that a typed request produces
        // the command line the nine call sites used to format by hand, and
        // test_seam is what holds it to that. It goes whole, with the python
        // backend, when the last verb is C.
        char verb[64];
        switch (req->boardMode)
        {
        case WR_BOARD_SLOWEST:
            _snprintf_s(verb, sizeof(verb), _TRUNCATE,
                        "--board --slowest --count %d", req->count);
            break;
        case WR_BOARD_SPREAD:
            _snprintf_s(verb, sizeof(verb), _TRUNCATE,
                        "--board --spread %d", req->spread);
            break;
        case WR_BOARD_FRIENDS:
            strcpy_s(verb, sizeof(verb), "--board --friends");
            break;
        default:
            // The "Fastest N" button is this with fromRank pinned to 1, which is
            // why there is one window mode and not two.
            _snprintf_s(verb, sizeof(verb), _TRUNCATE,
                        "--board --from-rank %d --count %d",
                        req->fromRank, req->count);
            break;
        }
        _snprintf_s(out, cap, _TRUNCATE,
                    "%s --map \"%s\" --gamemode %d --track-type %d --track-num %d",
                    verb, req->map, req->gamemode, req->trackType, req->trackNum);
        return true;
    }

    case WR_JOB_FETCH:
        // Two shapes, because they answer two different questions. The Board tab
        // asks for named places on a board it already has cached, so it passes a
        // gamemode and a selection. The Maps tab is browsing a map it may never
        // have loaded, so it passes an id and a count and lets the gamemode
        // default.
        if (req->rankCount > 0)
        {
            char pick[MAX_PATH];
            if (!WritePickFile(req, pick, sizeof(pick)))
                return false;
            _snprintf_s(out, cap, _TRUNCATE,
                        "--fetch --map \"%s\" --gamemode %d --track-type %d "
                        "--track-num %d --ranks-file \"%s\"%s",
                        req->map, req->gamemode, req->trackType, req->trackNum,
                        pick, req->intoGame ? " --into-game" : "");
        }
        else
        {
            _snprintf_s(out, cap, _TRUNCATE,
                        "--fetch --map \"%s\" --map-id %d --top %d "
                        "--track-type %d --track-num %d%s%s",
                        req->map, req->mapId, req->top, req->trackType,
                        req->trackNum, req->intoGame ? " --into-game" : "",
                        req->dryRun ? " --dry-run" : "");
        }
        return true;

    default:
        return false;
    }
}

bool WrExtractTestPythonArgs(const WrExtractRequest *req, char *out, int cap,
                             bool *needsMap)
{
    return BuildPythonArgs(req, out, cap, needsMap);
}

// Launch the script. Returns false having said why, in which case the caller
// ends the run; the latch is already held either way.
static bool StartPythonChild(const WrExtractRequest *req)
{
    FindInterpreter();
    if (!g_interpFound)
    {
        // Say so in the panel. Returning silently makes the button look broken,
        // which is the worst way to find out Python is not installed.
        Emit(g_interp);
        return false;
    }

    char extraArgs[1024];
    bool needsMap = false;
    if (!BuildPythonArgs(req, extraArgs, sizeof(extraArgs), &needsMap))
    {
        Emit("nothing to run");
        return false;
    }

    char map[72];
    EnterCriticalSection(&g_cs);
    strcpy_s(map, sizeof(map), g_map);
    LeaveCriticalSection(&g_cs);

    if (needsMap && !map[0])
    {
        // Extraction is the one verb about where you are standing, and the
        // button that starts it is only drawn in a map -- so this is nearly
        // unreachable. It used to return in silence anyway, which is the worst
        // possible answer to a press.
        Emit("no map loaded, so there is nothing to extract for");
        return false;
    }

    char script[MAX_PATH];
    _snprintf_s(script, sizeof(script), _TRUNCATE, "%s\\wrpath_extract.py",
                WrModuleDir());
    if (GetFileAttributesA(script) == INVALID_FILE_ATTRIBUTES)
    {
        Emit("wrpath_extract.py is not next to wrlines.dll");
        return false;
    }

    // -u because Python block-buffers stdout when it is not a terminal, and
    // without it the panel would show nothing at all until the run ended.
    char mapArg[96] = {0};
    if (needsMap)
        _snprintf_s(mapArg, sizeof(mapArg), _TRUNCATE, "--map \"%s\" ", map);

    char cmd[2048];
    _snprintf_s(cmd, sizeof(cmd), _TRUNCATE,
                "\"%s\" %s-u \"%s\" %s--game \"%s\" %s",
                g_interp, g_interpIsPy ? "-3 " : "", script, mapArg,
                WrGameDir(), extraArgs);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;

    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 64 * 1024))
    {
        Emit("could not create a pipe for the script's output");
        return false;
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
        DWORD err = GetLastError();
        CloseHandle(rd);
        Emitf("could not start the interpreter (error %lu)", err);
        WrLogf("[!] extract: CreateProcess failed (%lu)", err);
        return false;
    }

    CloseHandle(pi.hThread);

    // A kill-on-close job, so Stop can take the worker pool as well as the
    // interpreter that spawned it. Assignment can be refused when the game is
    // already inside a job that disallows nesting -- that is survivable, and
    // WrExtractStop says which case it is in rather than claiming a clean stop.
    HANDLE job = CreateJobObjectA(NULL, NULL);
    if (job)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
        memset(&jeli, 0, sizeof(jeli));
        jeli.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                &jeli, sizeof(jeli));
        if (!AssignProcessToJobObject(job, pi.hProcess))
        {
            WrLogf("extract: job assignment refused (%lu); Stop will reach the "
                   "interpreter but not its workers", GetLastError());
            CloseHandle(job);
            job = NULL;
        }
    }

    EnterCriticalSection(&g_cs);
    g_proc = pi.hProcess;
    g_job = job;
    LeaveCriticalSection(&g_cs);

    Emitf("running: %s", cmd);
    WrLogf("extract: started for \"%s\"", map[0] ? map : req->map);

    // From here on a failure has to tear the child down too, not just report
    // itself. Leaving g_proc set with no reader means the next launch overwrites
    // it and the old handle leaks while its process runs on.
    ReadCtx *ctx = (ReadCtx *)malloc(sizeof(ReadCtx));
    if (!ctx)
    {
        CloseHandle(rd);
        Abandon();
        return false;
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
        Abandon();
        return false;
    }

    // ReadThread owns the ending from here.
    return true;
}

// ---------------------------------------------------------------------------
// The one slot
// ---------------------------------------------------------------------------

// How far the port has got, and the only place that knows.
//
// Each phase adds one kind here and deletes the branch of BuildPythonArgs that
// used to serve it. When this returns true for all four, the python backend and
// this function go together.
static bool NativeHandles(WrJobKind kind)
{
    return kind == WR_JOB_INDEX_MAPS || kind == WR_JOB_BOARD;
}

// The one cancellation predicate the native side has.
//
// A board fetch is up to a hundred and seventy requests with a four-hundred
// millisecond pause between them, so Stop has to be able to land inside the
// pause and not only between pages. wr_api.cpp polls this eight times per
// pause; what it costs is that a request already in flight still has to finish
// or time out, which is the network timeout in wr_http.h and not this.
static bool NativeAbort(void *)
{
    return g_cancel != 0;
}

// The native side of the slot.
//
// One thread per job, and it does everything the ReadThread did for a child
// process: nothing else emits, and it ends with the same EndRun so the panel's
// terminal line is identical whichever backend ran.
//
// The exit code is synthesised to match what the script would have returned --
// cmd_index_maps answers 1 when there is no cache to read -- because the panel
// prints it and a user comparing two runs should not be able to tell which one
// they got.
static DWORD WINAPI NativeThread(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    // A copy, taken once, so the work below never reads the request while the
    // UI thread could be filling in the next one.
    WrExtractRequest req;
    EnterCriticalSection(&g_cs);
    req = g_req;
    LeaveCriticalSection(&g_cs);

    DWORD code = 1;
    switch (req.kind)
    {
    case WR_JOB_INDEX_MAPS:
        code = (WrMapsWriteIndex(WrGameDir(), Emit) > 0) ? 0 : 1;
        break;

    case WR_JOB_BOARD:
    {
        WrApiBoardArgs a;
        memset(&a, 0, sizeof(a));
        a.gameDir = WrGameDir();
        a.map = req.map;
        a.mapId = req.mapId;            // 0 from the panel: resolved by name,
                                        // the same way the reference does it
        a.gamemode = req.gamemode;
        a.trackType = req.trackType;
        a.trackNum = req.trackNum;
        a.mode = req.boardMode;
        a.fromRank = req.fromRank;
        a.count = req.count;
        a.spread = req.spread;
        code = (DWORD)WrApiBoard(&a, Emit, NativeAbort, NULL);
        break;
    }

    default:
        Emit("nothing to run");
        break;
    }

    EndRun(code);
    return 0;
}

void WrExtractSubmit(const WrExtractRequest *req)
{
    EnsureCs();
    if (!req || req->kind == WR_JOB_NONE)
        return;

    // The latch is claimed BEFORE the backend is chosen. That is what lets a
    // ported verb and an unported one share one slot, one Stop and one pane.
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0)
        return;

    int *ranks = NULL;
    if (req->rankCount > 0 && req->ranks)
    {
        ranks = (int *)malloc(sizeof(int) * (size_t)req->rankCount);
        if (ranks)
            memcpy(ranks, req->ranks, sizeof(int) * (size_t)req->rankCount);
    }

    EnterCriticalSection(&g_cs);
    g_req = *req;
    free(g_ranks);
    g_ranks = ranks;
    g_rankCount = ranks ? req->rankCount : 0;
    g_req.ranks = g_ranks;
    g_req.rankCount = g_rankCount;
    g_lineCount = 0;
    LeaveCriticalSection(&g_cs);

    InterlockedExchange(&g_stopped, 0);
    InterlockedExchange(&g_cancel, 0);

    if (NativeHandles(req->kind))
    {
        if (g_nativeThread)
        {
            CloseHandle(g_nativeThread);
            g_nativeThread = NULL;
        }
        g_nativeThread = CreateThread(NULL, 0, NativeThread, NULL, 0, NULL);
        if (!g_nativeThread)
        {
            Emit("could not start a worker thread");
            EndRun(1);
        }
        return;
    }

    if (!StartPythonChild(&g_req))
        EndRun(1);
}

void WrExtractRun(bool retryFailed)
{
    WrExtractRequest req = {WR_JOB_EXTRACT};
    req.retryFailed = retryFailed;
    req.timeoutSeconds = g_timeout;
    WrExtractSubmit(&req);
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
    // Deliberately does not kill a running script. Not for the reason this
    // comment used to give -- it claimed a kill could leave a truncated
    // .wrpath, and that has not been true since the writer became tmp +
    // os.replace, which is atomic. Every completed file survives any kill.
    //
    // The real reason is that this is a separate process doing useful work and
    // nothing here needs it dead. Stopping it is a decision for the person at
    // the keyboard, which is what WrExtractStop is for.
}
