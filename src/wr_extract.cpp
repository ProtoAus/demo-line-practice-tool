// wr_extract.cpp  --  see wr_extract.h.

#include "wr_extract.h"
#include "wr_api.h"
#include "wr_demo.h"
#include "wr_dp.h"
#include "wr_fetch.h"
#include "wr_jobs.h"
#include "wr_maps.h"
#include "wr_mtv.h"
#include "wr_path.h"
#include "wr_log.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WR_EXTRACT_LINES 400

// 400 and it used to be 200, which was enough for every line the fetchers
// print and not for the one extraction prints when a demo fails: a failure
// reason can run to two hundred and fifty characters -- "no chain reproduced
// the recorded max speed (best ... vs ...), none was confirmed as the origin
// stream, and the longest covers only ...% of ticks" -- on top of a
// forty-four-character file name. Truncating that in the ring would have been
// cosmetic; truncating it in Emitf, which formats before it emits, would have
// truncated tests\wrextract.exe's stdout too, and stdout is a compared
// artefact.
#define WR_EXTRACT_LINE_MAX 400

#define WR_FAILED_FILE "_failed.txt"

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
static volatile LONG g_stopped = 0;     // this run ended because Stop was pressed
static volatile LONG g_cancel = 0;      // asked to stop; every worker polls it
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

// ONE reader, for the counter and for the extractor, and it is UNCAPPED.
//
// There were two: load_failures in the reference and a fixed 512-entry array
// here, which was fine while this side only counted. It is not fine now that
// this side WRITES the file: flushing a failure is a read-modify-write, so a
// capped read would silently drop every record past the 513th the next time
// anything failed on a busy map. The old cap was a display limit that would
// have become data loss.
//
// The name field is 128 rather than the old 80 for the reason wr_fetch.h
// records at length: a downloaded demo's basename is a 40-character replay
// hash, but momtv\local\ holds the player's OWN recordings under whatever the
// game called them, and those reach 64 characters on this machine.
struct WrFailRec
{
    char base[128];
    long long size;
    char why[256];
};

struct WrFailSet
{
    WrFailRec *v;
    int n, cap;
};

static void FailFree(WrFailSet *s)
{
    free(s->v);
    s->v = NULL;
    s->n = s->cap = 0;
}

// strcmp and not _stricmp. The reference looks these up in a dict, which is
// case-sensitive, and sorts them the same way when it writes the file back.
static int FailFind(const WrFailSet *s, const char *base)
{
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->v[i].base, base) == 0)
            return i;
    return -1;
}

static bool FailPut(WrFailSet *s, const char *base, long long size,
                    const char *why)
{
    int at = FailFind(s, base);
    if (at < 0)
    {
        if (s->n == s->cap)
        {
            int grown = s->cap ? s->cap * 2 : 32;
            WrFailRec *bigger = (WrFailRec *)realloc(
                s->v, sizeof(WrFailRec) * (size_t)grown);
            if (!bigger)
                return false;
            s->v = bigger;
            s->cap = grown;
        }
        at = s->n++;
    }
    strncpy_s(s->v[at].base, sizeof(s->v[at].base), base, _TRUNCATE);
    s->v[at].size = size;
    strncpy_s(s->v[at].why, sizeof(s->v[at].why), why ? why : "", _TRUNCATE);
    return true;
}

static void FailDrop(WrFailSet *s, const char *base)
{
    int at = FailFind(s, base);
    if (at < 0)
        return;
    s->v[at] = s->v[--s->n];
}

static void FailPath(char *out, int cap, const char *mapDir)
{
    _snprintf_s(out, (size_t)cap, _TRUNCATE, "%s\\%s", mapDir, WR_FAILED_FILE);
}

// Records written by a different EXTRACTOR_REVISION are ignored rather than
// deleted: they cost nothing to leave in place and the next write rewrites the
// file anyway.
static void FailLoad(WrFailSet *s, const char *mapDir)
{
    FailFree(s);

    char path[MAX_PATH];
    FailPath(path, sizeof(path), mapDir);

    FILE *f = NULL;
    if (fopen_s(&f, path, "r") != 0 || !f)
        return;

    char line[1024];
    while (fgets(line, sizeof(line), f))
    {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (!line[0] || line[0] == '#')
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

        FailPut(s, tab2 + 1, _atoi64(tab1 + 1), tab3 + 1);
    }
    fclose(f);
}

static int CompareFailRec(const void *a, const void *b)
{
    return strcmp(((const WrFailRec *)a)->base, ((const WrFailRec *)b)->base);
}

// An empty set REMOVES the file rather than writing a header with nothing under
// it, which is what makes a rescue actually forget a failure.
//
// Text mode on purpose: the reference opens this with open(..., "w") and gets
// CRLF, and the file is compared byte for byte by the parity driver.
static void FailSave(WrFailSet *s, const char *mapDir)
{
    char path[MAX_PATH];
    FailPath(path, sizeof(path), mapDir);

    if (s->n == 0)
    {
        DeleteFileA(path);
        return;
    }

    if (!WrMakeTree(mapDir))
        return;

    char tmp[MAX_PATH];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s.tmp", path);

    FILE *f = NULL;
    if (fopen_s(&f, tmp, "w") != 0 || !f)
        return;

    fprintf(f, "# WrLines: demos on this map that could not be extracted.\n");
    fprintf(f, "# Re-running the extractor skips these, because deriving the\n");
    fprintf(f, "# same failure again costs the same minutes it cost the first\n");
    fprintf(f, "# time. Pass --retry-failed to try them anyway; delete this\n");
    fprintf(f, "# file to forget them entirely.\n");
    fprintf(f, "# extractor-revision <TAB> bytes <TAB> demo <TAB> why\n");

    qsort(s->v, (size_t)s->n, sizeof(WrFailRec), CompareFailRec);
    for (int i = 0; i < s->n; i++)
    {
        // Tabs and newlines would make the record unreadable by its own parser.
        char why[256];
        strcpy_s(why, sizeof(why), s->v[i].why);
        for (char *p = why; *p; p++)
            if (*p == '\t' || *p == '\n')
                *p = ' ';
        fprintf(f, "%d\t%lld\t%s\t%s\n", WR_EXTRACTOR_REVISION, s->v[i].size,
                s->v[i].base, why);
    }
    fclose(f);
    MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
}

// The counter's copy, held for the length of one count.
static WrFailSet g_failed = {NULL, 0, 0};

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

// The size is checked as well as the name so that a re-downloaded demo -- a
// different file that happens to have the same name -- is not written off on
// the strength of the old one's failure.
static bool IsKnownBad(const WrFailSet *s, const char *base, long long size)
{
    int at = FailFind(s, base);
    return at >= 0 && s->v[at].size == size;
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
            // Not through a junction. The counter does not care about os.walk's
            // ORDER -- see WalkDemos, which does -- but it cares about the same
            // cycle: a junction pointing at one of its own ancestors would
            // recurse here until the stack was gone, and this runs on a
            // background thread inside the game.
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                continue;
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
                 IsKnownBad(&g_failed, base,
                            ((long long)fd.nFileSizeHigh << 32) |
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

        FailLoad(&g_failed, pathsDir);

        char root[MAX_PATH];
        _snprintf_s(root, sizeof(root), _TRUNCATE, "%s\\momentum\\momtv", WrGameDir());
        CountInTree(root, map, pathsDir);

        // Demos we fetched ourselves. They live under wrlines_data because
        // nothing here writes into the game install, so they are a second tree
        // rather than more files in momtv.
        CountInTree(WrDataPath("demos"), map, pathsDir);

        FailFree(&g_failed);
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
// Running
// ---------------------------------------------------------------------------

// There used to be a FindInterpreter here: fifty lines that searched PATH for
// py.exe, then python.exe, then python3.exe, and had a separate message for
// finding none of them inside a Wine prefix. It went with the last verb of the
// port, along with the child process, the pipe, the reader thread and the job
// object that Stop used to terminate.
//
// This string is what the panel shows under the buttons when nothing has run
// yet, and it is no longer a diagnosis of the machine, because there is nothing
// left to diagnose.
const char *WrExtractStatus(void)
{
    return "Extraction runs inside the DLL. Nothing else needs installing.";
}

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

// ReadThread and Abandon lived here: a thread that read a pipe, reassembled
// lines, waited on a child process and turned its exit code into the panel's
// last line. EndRun above is its tail, hoisted out at P0 so that both backends
// could share one ending -- and now there is one backend and this is all that
// remains of the other.

// Stop is one flag, and there is nothing left to kill.
//
// It used to be TerminateJobObject on a job containing the interpreter and its
// worker POOL -- grandchildren this side held no handles for, each of which
// would otherwise burn a core until its queue pipe broke. That was instant and
// total, and nothing replaces it: TerminateThread on a thread of the game's own
// process leaks whatever locks it held, the CRT heap lock among them, and the
// game would deadlock on its next malloc.
//
// So this sets a flag that every worker polls, and the panel says exactly that
// rather than claiming a clean stop it cannot deliver. See WrJobsAbort in
// wr_jobs.h for what the wait actually costs.
void WrExtractStop(void)
{
    InterlockedExchange(&g_cancel, 1);
    if (!g_running)
        return;                 // the flag is cleared by the next EndRun

    InterlockedExchange(&g_stopped, 1);
    Emit("--- stopping; the work in flight has to reach its next checkpoint ---");
    WrLogf("extract: stop -- cooperative");
}

static int g_timeout = WR_EXTRACT_TIMEOUT_DEFAULT;

void WrExtractSetTimeout(int seconds) { g_timeout = seconds < 0 ? 0 : seconds; }
int WrExtractTimeout(void) { return g_timeout; }

// ---------------------------------------------------------------------------
// Stopping
// ---------------------------------------------------------------------------

// The one flag, read through two shapes of predicate because the layers below
// want different answers from it.
//
// A board fetch is up to a hundred and seventy requests with a four-hundred
// millisecond pause between them, so Stop has to be able to land inside the
// pause and not only between pages. wr_api.cpp polls this eight times per
// pause; what that costs is that a request already in flight still has to
// finish or time out, which is the network timeout in wr_http.h and not this.
static bool NativeAbort(void *)
{
    return g_cancel != 0;
}

// The pool's, which is the same question with an int for an answer.
static int JobsAbort(void *)
{
    return g_cancel != 0;
}

// ---------------------------------------------------------------------------
// Extraction
// ---------------------------------------------------------------------------
//
// cmd_extract, and the last verb of the port. Everything above this point is
// bookkeeping the panel already did; this is the thing the tool exists for.
//
// The python backend lived here: WritePickFile, BuildPythonArgs and
// StartPythonChild, plus a pipe, a reader thread and a kill-on-close job
// object. They went together, in one commit, with tests\test_seam.exe -- which
// was written at P0 to hold the typed request to the exact command line the
// nine call sites used to format by hand, and which had no subject left once
// there was no command line.
//
// WHAT THE COORDINATOR DOES AND WHAT A WORKER DOES
//
// A worker takes one demo, start to finish, and writes one file: its own
// <basename>.wrpath, through a temp name and a rename, which cannot collide
// with anything. It emits nothing and it resolves no paths.
//
// The coordinator does everything else -- the progress line, the failure
// record, the removal of stale output. In the reference the consume loop runs
// in the PARENT process, so _flush_failures is serialised for free; two workers
// failing on the same map would otherwise race the same temp file and one of
// the records would be lost.
//
// WHY THE FAILURE RECORD IS WRITTEN AS FAILURES HAPPEN
//
// A recorded failure is what stops the next run paying the same timeout again,
// and the epilogue is not reached if the run is stopped. Recording forty demos'
// worth of expensive failures and then throwing all of them away because
// somebody pressed Stop is the worst of both. The write is a temp file and a
// rename over a set of tens of entries, so per failure costs nothing worth
// measuring.
//
// It is done at the END as well, because that is where RESCUES land: a demo
// that failed before and worked this time has to have its old record dropped,
// and that is only known once it has succeeded.

struct ExtractItem
{
    char path[MAX_PATH];
    char map[72];
    char name[144];             // the file name, with its extension
    char base[128];             // ... without
    long long size;

    // Filled in on a worker thread and read on the coordinator's, after the
    // pool has established the happens-before between the two.
    int outcome;                // WrDemoOutcome
    char message[256];
    double runTime;
    int samples;
    double coverage, matchError, scanSeconds;
    int markers;
    bool markersOk, flagged;
};

// The reference's now_failed / now_ok / seen_maps, per map, so the record can be
// updated in place: newly failed demos are added, and any that succeed this
// time are dropped.
struct MapState
{
    char map[72];
    WrFailSet nowFailed;
    char **nowOk;
    int okN, okCap;
};

struct ExtractJob
{
    ExtractItem *items;
    int count;
    char outDir[MAX_PATH];
    bool verify;
    int timeout;

    // Coordinator only.
    int done, ok, skipped, failed, lowconf, removed;
    double *cov;
    int covN;
    MapState *maps;
    int mapN, mapCap;
};

static MapState *MapStateFor(ExtractJob *j, const char *map)
{
    for (int i = 0; i < j->mapN; i++)
        if (strcmp(j->maps[i].map, map) == 0)
            return &j->maps[i];
    if (j->mapN == j->mapCap)
    {
        int grown = j->mapCap ? j->mapCap * 2 : 8;
        MapState *bigger = (MapState *)realloc(j->maps,
                                               sizeof(MapState) * (size_t)grown);
        if (!bigger)
            return NULL;
        j->maps = bigger;
        j->mapCap = grown;
    }
    MapState *m = &j->maps[j->mapN++];
    memset(m, 0, sizeof(*m));
    strcpy_s(m->map, sizeof(m->map), map);
    return m;
}

static void MapStateOk(MapState *m, const char *base)
{
    if (m->okN == m->okCap)
    {
        int grown = m->okCap ? m->okCap * 2 : 32;
        char **bigger = (char **)realloc(m->nowOk, sizeof(char *) * (size_t)grown);
        if (!bigger)
            return;
        m->nowOk = bigger;
        m->okCap = grown;
    }
    m->nowOk[m->okN] = _strdup(base);
    if (m->nowOk[m->okN])
        m->okN++;
}

// _flush_failures: read, merge this run's failures, drop this run's rescues,
// write. Read-modify-write rather than an append, because the file also has to
// LOSE entries -- a demo that failed last time and worked this time must have
// its record dropped, or it is skipped for ever.
static void FlushFailures(const ExtractJob *j, MapState *m)
{
    char dir[MAX_PATH];
    _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\%s", j->outDir, m->map);

    WrFailSet recs;
    memset(&recs, 0, sizeof(recs));
    FailLoad(&recs, dir);
    for (int i = 0; i < m->nowFailed.n; i++)
        FailPut(&recs, m->nowFailed.v[i].base, m->nowFailed.v[i].size,
                m->nowFailed.v[i].why);
    for (int i = 0; i < m->okN; i++)
        FailDrop(&recs, m->nowOk[i]);
    FailSave(&recs, dir);
    FailFree(&recs);
}

// ---------------------------------------------------------------------------
// Finding the demos
// ---------------------------------------------------------------------------

typedef void (*DemoVisit)(void *user, const char *path,
                          const WIN32_FIND_DATAA *fd);

// os.walk's order, and it is not the obvious one.
//
// A DIRECTORY'S FILES COME FIRST, ALL OF THEM, AND ONLY THEN ITS
// SUBDIRECTORIES. Recursing the moment a directory turns up in the enumeration
// -- which is what the counter next door does, because the counter does not
// care -- interleaves the two and produces a different order. That matters
// here and nowhere else: at --jobs 1 the reference processes targets in
// iteration order and prints a line each, so the order IS the compared
// artefact.
//
// AND IT DOES NOT GO THROUGH A JUNCTION, which is os.walk's followlinks=False
// and is load-bearing twice over.
//
// The parity half: since CPython 3.8 os.lstat reports any name-surrogate
// reparse point as a link -- a junction as much as a symlink -- so os.walk
// lists such a directory and then declines to descend into it. A port that
// descended would find demos the reference never sees, which is a different
// target list and therefore a different everything.
//
// The half that is not about parity at all: a junction pointing at one of its
// own ancestors is a cycle, and this function has no depth limit and no
// visited set. The path stops growing once _snprintf_s starts truncating at
// MAX_PATH, so the recursion stops even getting deeper -- it just repeats the
// same directory until the stack is gone, inside the game's process. The
// reference cannot reach that state; without this check we could.
//
// The ROOT is exempt by construction: we are handed it rather than finding it,
// and os.walk descends into the root it is given whatever it is. tests\
// parity.ps1 depends on exactly that -- it links the fetched demo tree into
// the staging directory with mklink /J and both sides walk it.
static void WalkDemos(const char *root, DemoVisit visit, void *user)
{
    char pattern[MAX_PATH];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*", root);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;

    char **dirs = NULL;
    int nd = 0, cd = 0;

    do {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == '\0' ||
             (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
            continue;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            // Listed, not descended into. See the essay above.
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                continue;
            if (nd == cd)
            {
                int grown = cd ? cd * 2 : 16;
                char **bigger = (char **)realloc(dirs, sizeof(char *) * (size_t)grown);
                if (!bigger)
                    continue;
                dirs = bigger;
                cd = grown;
            }
            dirs[nd] = _strdup(fd.cFileName);
            if (dirs[nd])
                nd++;
            continue;
        }

        const char *dot = strrchr(fd.cFileName, '.');
        if (!dot || _stricmp(dot, ".mtv") != 0)
            continue;

        char full[MAX_PATH];
        _snprintf_s(full, sizeof(full), _TRUNCATE, "%s\\%s", root, fd.cFileName);
        visit(user, full, &fd);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    for (int i = 0; i < nd; i++)
    {
        char sub[MAX_PATH];
        _snprintf_s(sub, sizeof(sub), _TRUNCATE, "%s\\%s", root, dirs[i]);
        WalkDemos(sub, visit, user);
        free(dirs[i]);
    }
    free(dirs);
}

// The per-map failure records, cached across the walk. The reference caches
// them in a dict for the same reason: --all crosses hundreds of maps and each
// record file would otherwise be re-read once per demo.
struct MapFails
{
    char map[72];
    WrFailSet set;
};

struct Collect
{
    ExtractJob *job;
    int cap;

    const char *wantMap;        // "" is --all
    bool skipExisting, retryFailed;

    int already, stale, knownBad;

    MapFails *cache;
    int cacheN, cacheCap;
};

static WrFailSet *CachedFailures(Collect *c, const char *map)
{
    for (int i = 0; i < c->cacheN; i++)
        if (strcmp(c->cache[i].map, map) == 0)
            return &c->cache[i].set;
    if (c->cacheN == c->cacheCap)
    {
        int grown = c->cacheCap ? c->cacheCap * 2 : 8;
        MapFails *bigger = (MapFails *)realloc(c->cache,
                                               sizeof(MapFails) * (size_t)grown);
        if (!bigger)
            return NULL;
        c->cache = bigger;
        c->cacheCap = grown;
    }
    MapFails *e = &c->cache[c->cacheN++];
    memset(e, 0, sizeof(*e));
    strcpy_s(e->map, sizeof(e->map), map);

    char dir[MAX_PATH];
    _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\%s", c->job->outDir, map);
    FailLoad(&e->set, dir);
    return &e->set;
}

// Was a local strrchr copy. It is WrFileStem now, for the reason its header
// comment gives: this side keys _failed.txt and the --skip-existing lookup on
// the stem, wr_demo.cpp names the output file with it, and the two disagreeing
// about a filename would break both silently.

static bool ItemPush(Collect *c, const char *path, const char *map,
                     const char *name, long long size)
{
    ExtractJob *j = c->job;
    if (j->count == c->cap)
    {
        int grown = c->cap ? c->cap * 2 : 256;
        ExtractItem *bigger = (ExtractItem *)realloc(
            j->items, sizeof(ExtractItem) * (size_t)grown);
        if (!bigger)
            return false;
        j->items = bigger;
        c->cap = grown;
    }
    ExtractItem *it = &j->items[j->count++];
    memset(it, 0, sizeof(*it));
    strncpy_s(it->path, sizeof(it->path), path, _TRUNCATE);
    strncpy_s(it->map, sizeof(it->map), map, _TRUNCATE);
    strncpy_s(it->name, sizeof(it->name), name, _TRUNCATE);
    WrFileStem(name, it->base, sizeof(it->base));
    it->size = size;
    return true;
}

static void CollectVisit(void *user, const char *path,
                         const WIN32_FIND_DATAA *fd)
{
    Collect *c = (Collect *)user;

    // peek_map, and deliberately NOT the sanity gates. The reference's peek_map
    // checks the magic and reads the name at 0x10 and nothing else, so a demo
    // whose tick interval is not a tick interval is still SELECTED here and
    // fails later with the gate's own wording -- which is the string that goes
    // into the failure record and therefore the string that has to match.
    // WrMtvPeek fills in every field it managed to read before the gate that
    // refused it, which is what makes borrowing it for this correct.
    WrMtvHeader hdr;
    char why[128];
    WrMtvPeek(path, &hdr, why, sizeof(why));

    if (c->wantMap[0] && _stricmp(hdr.map, c->wantMap) != 0)
        return;

    const long long size = ((long long)fd->nFileSizeHigh << 32) | fd->nFileSizeLow;

    char base[128];
    WrFileStem(fd->cFileName, base, sizeof(base));

    bool staleOut = false;
    if (c->skipExisting && hdr.map[0])
    {
        char out[MAX_PATH];
        _snprintf_s(out, sizeof(out), _TRUNCATE, "%s\\%s\\%s.wrpath",
                    c->job->outDir, hdr.map, base);
        if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES)
        {
            if (WrpathRevision(out) == WR_EXTRACTOR_REVISION)
            {
                c->already++;
                return;
            }
            c->stale++;         // written by an older extractor; redo it
            staleOut = true;
        }
    }

    // Demos that failed before, at this revision, with this exact file size. A
    // re-download that changed the file is not the same demo and gets another
    // go.
    //
    // Not applied when there is an out-of-date output on disk. That combination
    // is real -- a demo can have succeeded under the old extractor and fail
    // under the new one -- and skipping it would leave the old file in place
    // forever, still loaded, still drawn, derived from an assumption we have
    // since established was wrong.
    if (c->skipExisting && hdr.map[0] && !c->retryFailed && !staleOut)
    {
        const WrFailSet *fs = CachedFailures(c, hdr.map);
        if (fs && IsKnownBad(fs, base, size))
        {
            c->knownBad++;
            return;
        }
    }

    ItemPush(c, path, hdr.map, fd->cFileName, size);
}

// The same three trees, for a caller that wants the paths and not the filter
// rules. See the header.
struct PlainWalk
{
    WrDemoWalkFn visit;
    void *user;
};

static void PlainVisit(void *user, const char *path, const WIN32_FIND_DATAA *fd)
{
    const PlainWalk *w = (const PlainWalk *)user;
    w->visit(w->user, path,
             ((long long)fd->nFileSizeHigh << 32) | fd->nFileSizeLow);
}

void WrExtractWalkDemos(const char *gameDir, WrDemoWalkFn visit, void *user)
{
    PlainWalk w;
    w.visit = visit;
    w.user = user;

    char root[MAX_PATH];
    _snprintf_s(root, sizeof(root), _TRUNCATE, "%s\\momentum\\momtv\\online",
                gameDir);
    WalkDemos(root, PlainVisit, &w);
    _snprintf_s(root, sizeof(root), _TRUNCATE, "%s\\momentum\\momtv\\local",
                gameDir);
    WalkDemos(root, PlainVisit, &w);
    strcpy_s(root, sizeof(root), WrDataPath("demos"));
    WalkDemos(root, PlainVisit, &w);
}

// ---------------------------------------------------------------------------
// One demo, on a worker thread
// ---------------------------------------------------------------------------

// The deadline lives HERE, in a stack frame of the worker that owns it.
//
// The reference's is a module global and each of its workers is a separate
// process, so each gets its own copy for free. Transcribing that shape as a
// static would silently give N threads one shared clock: the first demo to
// start would set a deadline the fourth demo, beginning thirty seconds later,
// would trip almost immediately.
struct WorkerCtx
{
    ULONGLONG deadline;         // 0 is --timeout 0
};

static int ExtractAbort(void *user)
{
    const WorkerCtx *c = (const WorkerCtx *)user;
    // Stop first. A run that is being cancelled should not also record
    // timeouts for the demos that were in flight when it happened.
    if (g_cancel)
        return WR_DP_STOP_CANCEL;
    if (c->deadline && GetTickCount64() > c->deadline)
        return WR_DP_STOP_TIMEOUT;
    return WR_DP_GO;
}

static void ExtractRunOne(void *user, int index)
{
    ExtractJob *j = (ExtractJob *)user;
    ExtractItem *it = &j->items[index];

    WorkerCtx ctx;
    ctx.deadline = j->timeout > 0
                 ? GetTickCount64() + (ULONGLONG)j->timeout * 1000ull
                 : 0;

    WrDemoArgs a;
    memset(&a, 0, sizeof(a));
    a.outDir = j->outDir;       // absolute, resolved once, before any worker ran
    a.verify = j->verify;
    a.timeoutSeconds = (double)j->timeout;
    a.abort = ExtractAbort;
    a.abortUser = &ctx;

    WrDemoResult r;
    it->outcome = (int)WrDemoProcess(it->path, &a, &r);
    strncpy_s(it->message, sizeof(it->message), r.message, _TRUNCATE);
    it->runTime = r.h.runTime;
    it->samples = r.dp.info.samples;
    it->coverage = r.dp.info.coverage;
    it->matchError = r.dp.info.matchError;
    it->scanSeconds = r.dp.info.scanSeconds;
    it->markers = r.markerCount;
    it->markersOk = r.markersOk;
    it->flagged = r.flagged;
    WrDemoFree(&r);
}

static unsigned long long ExtractCost(void *user, int index)
{
    const ExtractJob *j = (const ExtractJob *)user;
    return WrJobsCost(j->items[index].size);
}

// fmt_time: "1:03.250" once there is a minute in it, "3.250" before that.
static void FmtTime(char *out, int cap, double t)
{
    const int m = (int)floor(t / 60.0);
    if (m)
        _snprintf_s(out, (size_t)cap, _TRUNCATE, "%d:%06.3f", m, t - m * 60.0);
    else
        _snprintf_s(out, (size_t)cap, _TRUNCATE, "%.3f", t);
}

// ---------------------------------------------------------------------------
// One result, on the coordinator's thread
// ---------------------------------------------------------------------------

static void ExtractDone(void *user, int index)
{
    ExtractJob *j = (ExtractJob *)user;
    ExtractItem *it = &j->items[index];

    // A demo the user stopped is not a demo that failed. Nothing was learned
    // about it, so it gets no record and no line -- recording it would skip it
    // for ever on the strength of a keypress. The reference has no equivalent
    // case because Stop killed the process mid-sentence.
    if (it->outcome == WR_DEMO_CANCELLED)
        return;

    j->done++;
    char pre[32];
    _snprintf_s(pre, sizeof(pre), _TRUNCATE, "[%d/%d]", j->done, j->count);

    // seen_maps takes every demo with a map name, whatever became of it.
    MapState *ms = it->map[0] ? MapStateFor(j, it->map) : NULL;

    if (it->outcome == WR_DEMO_ERROR)
    {
        j->failed++;
        if (ms)
        {
            FailPut(&ms->nowFailed, it->base, it->size, it->message);
            if (!j->verify)
            {
                FlushFailures(j, ms);

                // An older extractor may have left an output for this demo. We
                // have just established the current one cannot produce it, so
                // that file is a path derived from an assumption we no longer
                // trust -- remove it rather than go on drawing it.
                // Current-revision files are never touched.
                char out[MAX_PATH];
                _snprintf_s(out, sizeof(out), _TRUNCATE, "%s\\%s\\%s.wrpath",
                            j->outDir, it->map, it->base);
                if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES &&
                    WrpathRevision(out) != WR_EXTRACTOR_REVISION &&
                    DeleteFileA(out))
                    j->removed++;
            }
        }
        Emitf("%s FAIL %-44.44s %s", pre, it->name, it->message);
        return;
    }

    if (it->outcome == WR_DEMO_SKIP)
    {
        j->skipped++;
        Emitf("%s SKIP %-44.44s %s", pre, it->name, it->message);
        return;
    }

    j->ok++;
    if (ms)
        MapStateOk(ms, it->base);       // rescued: forget any old failure
    j->cov[j->covN++] = it->coverage;
    // What actually got written to the file, not just the speed oracle's
    // verdict -- low coverage flags a run too, and reporting those as clean is
    // how two 4%-coverage runs went unnoticed.
    if (it->flagged)
        j->lowconf++;

    char mk[16];
    if (it->markers)
        _snprintf_s(mk, sizeof(mk), _TRUNCATE, "%d%s", it->markers,
                    it->markersOk ? "" : "!");
    else
        strcpy_s(mk, sizeof(mk), "-");

    char tm[32];
    FmtTime(tm, sizeof(tm), it->runTime);

    Emitf("%s %-4s %-44.44s %-9s %5d pts %5.1f%%  err %7.4f  mk %-4s %.1fs",
          pre, it->flagged ? "OK?" : "OK", it->name, tm, it->samples,
          100.0 * it->coverage, it->matchError, mk, it->scanSeconds);
}

static int CompareDouble(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void JobFree(ExtractJob *j)
{
    for (int i = 0; i < j->mapN; i++)
    {
        FailFree(&j->maps[i].nowFailed);
        for (int k = 0; k < j->maps[i].okN; k++)
            free(j->maps[i].nowOk[k]);
        free(j->maps[i].nowOk);
    }
    free(j->maps);
    free(j->items);
    free(j->cov);
    memset(j, 0, sizeof(*j));
}

// ---------------------------------------------------------------------------
// cmd_extract
// ---------------------------------------------------------------------------

static DWORD RunExtract(const WrExtractRequest *req)
{
    ExtractJob j;
    memset(&j, 0, sizeof(j));
    j.verify = req->verify;
    j.timeout = req->timeoutSeconds;

    // Resolved ONCE, here, before any worker exists. See WrDataPath in
    // wr_log.cpp: its four rotating buffers have no lock, and this is the rule
    // that makes that safe rather than lucky.
    strcpy_s(j.outDir, sizeof(j.outDir), WrDataPath("paths"));

    // The one place the game directory is resolved for this run. Empty means
    // WrGameDir(); see the field's comment in wr_extract.h for what filling it
    // in is for.
    const char *gameDir = req->gameDir[0] ? req->gameDir : WrGameDir();

    // Refuse, and SAY WHY, rather than walk a tree that cannot be opened and
    // report a clean run over nothing. See WrPathIsAscii in wr_common.h: this
    // build reads files with the -A calls and a path holding a byte >= 0x80 is
    // not nameable through them. Until v0.7.0 the extractor was Python, which
    // opens files with wide paths and did not have this problem, so this is the
    // one place the port is a step backwards -- which is exactly why it is
    // stated in full rather than left to be inferred from an empty result.
    //
    // A documented divergence from the reference, which has no such check and
    // needs none. Parity-safe by construction: the driver runs both sides out
    // of Temp and Program Files, so this branch is unreachable there, and an
    // extra refusal can only ever turn a wrong answer into no answer.
    if (!WrPathIsAscii(gameDir) || !WrPathIsAscii(j.outDir))
    {
        Emitf("[!] this build cannot read a path with non-ASCII characters in "
              "it, and one of these has them:");
        Emitf("      game   %s", gameDir);
        Emitf("      output %s", j.outDir);
        Emitf("[!] every file call here is the -A Windows form. Moving the game "
              "install, or this folder, somewhere ASCII is the fix.");
        Emitf("[!] nothing was read and nothing was written.");
        return 1;
    }

    Collect c;
    memset(&c, 0, sizeof(c));
    c.job = &j;
    c.wantMap = req->map;
    c.skipExisting = req->skipExisting;
    c.retryFailed = req->retryFailed;

    if (req->file[0])
    {
        // --file takes the demo as given: no skip rules, no failure record, no
        // revision check. It is the "do this one, now, whatever you think you
        // know about it" path.
        WrMtvHeader hdr;
        char why[128];
        WrMtvPeek(req->file, &hdr, why, sizeof(why));

        const char *name = req->file;
        for (const char *p = req->file; *p; p++)
            if (*p == '\\' || *p == '/')
                name = p + 1;

        WIN32_FILE_ATTRIBUTE_DATA ad;
        long long size = -1;
        if (GetFileAttributesExA(req->file, GetFileExInfoStandard, &ad))
            size = ((long long)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;

        ItemPush(&c, req->file, hdr.map, name, size);
    }
    else
    {
        char root[MAX_PATH];
        _snprintf_s(root, sizeof(root), _TRUNCATE, "%s\\momentum\\momtv\\online",
                    gameDir);
        WalkDemos(root, CollectVisit, &c);
        _snprintf_s(root, sizeof(root), _TRUNCATE, "%s\\momentum\\momtv\\local",
                    gameDir);
        WalkDemos(root, CollectVisit, &c);

        // Anything --fetch downloaded. It goes under wrlines_data rather than
        // into momtv because nothing here writes to the game install, so it
        // needs its own pass.
        strcpy_s(root, sizeof(root), WrDataPath("demos"));
        WalkDemos(root, CollectVisit, &c);
    }

    for (int i = 0; i < c.cacheN; i++)
        FailFree(&c.cache[i].set);
    free(c.cache);

    if (c.already)
        Emitf("%d already extracted, skipping them", c.already);
    if (c.stale)
        Emitf("%d were extracted by an older version and are being redone",
              c.stale);
    if (c.knownBad)
        Emitf("%d failed before and are being skipped (--retry-failed to try "
              "them again)", c.knownBad);

    if (req->limit > 0 && j.count > req->limit)
        j.count = req->limit;

    j.cov = (double *)malloc(sizeof(double) * (size_t)(j.count > 0 ? j.count : 1));
    if (!j.cov)
    {
        Emit("out of memory building the work list");
        JobFree(&j);
        return 1;
    }

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const int workers = WrJobsWorkerCount(req->jobs, (int)si.dwNumberOfProcessors,
                                          j.count);
    // The reference's own guard means reaching its print implies jobs >= 2, so
    // its `"" if jobs == 1` arm is dead and the line is always "workers".
    //
    // THE NUMBER CAN DIFFER FROM THE REFERENCE'S, AND DELIBERATELY. _job_count
    // clamps by nothing: with four demos to do it still says "14 workers",
    // because a Python worker that finds no work is an idle process and costs
    // nothing much. Here every worker holds a scan arena, so WrJobsWorkerCount
    // clamps by the item count and by WR_JOBS_MAX_WORKERS -- and this line
    // reports what was actually started, which is the true statement.
    //
    // It is unreachable on the run that matters: a full-corpus pass has six
    // thousand items, far above either clamp, so both sides print the same
    // number and stdout compares equal. It shows up only on a small run, where
    // the reference's number would be a claim about threads that do not exist.
    if (workers > 1 && j.count >= 2)
        Emitf("%d workers", workers);

    const ULONGLONG t0 = GetTickCount64();

    WrJobsConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.itemCount = j.count;
    cfg.requestedWorkers = workers;
    cfg.run = ExtractRunOne;
    cfg.done = ExtractDone;
    cfg.cost = ExtractCost;
    cfg.user = &j;
    WrJobsRunAll(&cfg, JobsAbort, NULL, NULL);

    const double elapsed = (double)(GetTickCount64() - t0) / 1000.0;

    Emit("");
    Emitf("%d processed in %.1fs: %d ok (%d low-confidence), %d skipped, "
          "%d failed", j.done, elapsed, j.ok, j.lowconf, j.skipped, j.failed);
    if (j.covN)
    {
        qsort(j.cov, (size_t)j.covN, sizeof(double), CompareDouble);
        Emitf("coverage: median %.1f%%  min %.1f%%  max %.1f%%",
              100.0 * j.cov[j.covN / 2], 100.0 * j.cov[0],
              100.0 * j.cov[j.covN - 1]);
    }
    if (!j.verify && j.ok)
        Emitf("wrote %d .wrpath files under %s", j.ok, j.outDir);
    if (j.removed)
        Emitf("removed %d out-of-date .wrpath file%s whose demo can no longer "
              "be extracted", j.removed, j.removed == 1 ? "" : "s");

    // --verify writes nothing, and that has to include this.
    if (!j.verify)
    {
        int recorded = 0;
        for (int i = 0; i < j.mapN; i++)
        {
            FlushFailures(&j, &j.maps[i]);
            recorded += j.maps[i].nowFailed.n;
        }
        if (recorded)
            Emitf("recorded %d failure%s so re-running this map skips them",
                  recorded, recorded == 1 ? "" : "s");
    }

    const DWORD code = j.failed == 0 ? 0u : 1u;
    JobFree(&j);
    return code;
}

// ---------------------------------------------------------------------------
// The one slot
// ---------------------------------------------------------------------------

// NativeHandles lived here: one function that said how far the port had got,
// gaining a WR_JOB_ kind per phase and deleting the branch of BuildPythonArgs
// that used to serve it. It returns true for all four now, which is the same
// thing as not existing.

// The work, on the calling thread, with no latch and no panel.
//
// Three lines of separation that buy the only thing making this code checkable
// from outside the game: WrExtractSubmit is this plus the one slot and a
// thread, and tests\wrextract.exe is this plus stdout. The alternative is a
// console front end with its own copy of the dispatch, which would agree with
// itself and say nothing about what ships.
//
// The exit code is synthesised to match what the reference implementation would
// have returned -- cmd_index_maps answers 1 when there is no cache to read, and
// cmd_extract answers 1 if anything failed -- because the panel prints it and a
// user comparing two runs should not be able to tell which one they got.
int WrExtractRunRequest(const WrExtractRequest *reqIn)
{
    const WrExtractRequest req = *reqIn;
    DWORD code = 1;
    switch (req.kind)
    {
    case WR_JOB_EXTRACT:
        code = RunExtract(&req);
        break;

    case WR_JOB_INDEX_MAPS:
        code = (WrMapsWriteIndex(req.gameDir[0] ? req.gameDir : WrGameDir(),
                                 Emit) > 0) ? 0 : 1;
        break;

    case WR_JOB_BOARD:
    {
        WrApiBoardArgs a;
        memset(&a, 0, sizeof(a));
        a.gameDir = req.gameDir[0] ? req.gameDir : WrGameDir();
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

    case WR_JOB_FETCH:
    {
        WrFetchArgs a;
        memset(&a, 0, sizeof(a));
        a.gameDir = req.gameDir[0] ? req.gameDir : WrGameDir();
        a.map = req.map;
        a.mapId = req.mapId;
        a.gamemode = req.gamemode;
        a.trackType = req.trackType;
        a.trackNum = req.trackNum;
        // Straight through. This is the pay-off the request struct was
        // built for at P0: the selection used to travel through a FILE
        // because a command line is 2048 bytes and "tick all" is not bounded
        // by anything. In process it is a pointer and a count.
        a.ranks = req.ranks;
        a.rankCount = req.rankCount;
        a.fromRank = req.fromRank;
        a.count = req.count;
        a.top = req.top;
        a.dryRun = req.dryRun;
        a.intoGame = req.intoGame;
        code = (DWORD)WrFetchRun(&a, Emit, NativeAbort, NULL);
        break;
    }

    default:
        Emit("nothing to run");
        break;
    }
    return (int)code;
}

// One thread per job. Nothing else emits, and every path ends with the same
// EndRun, so the panel's terminal line is identical whichever verb ran.
static DWORD WINAPI NativeThread(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    // A copy, taken once, so the work below never reads the request while the
    // UI thread could be filling in the next one.
    WrExtractRequest req;
    EnterCriticalSection(&g_cs);
    req = g_req;
    LeaveCriticalSection(&g_cs);

    EndRun((DWORD)WrExtractRunRequest(&req));
    return 0;
}

void WrExtractSubmit(const WrExtractRequest *req)
{
    EnsureCs();
    if (!req || req->kind == WR_JOB_NONE)
        return;

    // The latch is claimed BEFORE any work starts. It was written that way so a
    // ported verb and an unported one could share one slot, one Stop and one
    // pane while the port was in progress; there is one backend now and the
    // shape is still right, because the failure paths below all have to end the
    // run through the same EndRun.
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
}

void WrExtractRun(bool retryFailed)
{
    EnsureCs();
    WrExtractRequest req = {WR_JOB_EXTRACT};
    req.retryFailed = retryFailed;
    req.timeoutSeconds = g_timeout;
    req.jobs = 0;                       // decide for me
    // The Runs tab is about the map you are standing in, and the button that
    // starts this is only drawn in one. An empty map means "every map", which
    // is a thing the console front end can ask for and the panel cannot.
    req.skipExisting = true;

    EnterCriticalSection(&g_cs);
    strcpy_s(req.map, sizeof(req.map), g_map);
    LeaveCriticalSection(&g_cs);

    if (!req.map[0])
    {
        // Nearly unreachable, and it used to return in silence, which is the
        // worst possible answer to a press.
        Emit("no map loaded, so there is nothing to extract for");
        return;
    }
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

// WrExtractShutdown is gone, and it is worth saying why rather than leaving a
// gap. It was an empty body with zero call sites, and its comment gave the
// reason: the work was a separate process doing something useful, and nothing
// in here needed it dead. That reasoning did stop being true when the workers
// became threads of this process -- a thread still running when this code is
// unmapped faults on its next instruction.
//
// What replaces it is nothing at all, deliberately, because the hazard cannot
// occur here. This DLL has no unload path by design (see dllmain.cpp), and the
// one Windows provides is the wrong place to wait: by the time
// DLL_PROCESS_DETACH runs on process exit, ExitProcess has already terminated
// every other thread, so waiting for a worker would be waiting -- under the
// loader lock -- for a thread that will never run again. Nothing is lost by not
// waiting either: every file a worker writes goes through a temp name and a
// rename, so the worst a killed worker can leave behind is a stray .tmp.
//
// A function with a body, no caller and no way to acquire one is worse than the
// absence of it: it reads as a thing that happens.
