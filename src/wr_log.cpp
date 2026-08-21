// wr_log.cpp  --  see wr_log.h.

#include "wr_log.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <share.h>

HMODULE g_wrSelf = NULL;

static CRITICAL_SECTION g_lock;
static bool g_lockReady = false;
static FILE *g_file = NULL;
static char g_ring[WR_LOG_LINES][WR_LOG_LINE_MAX];
static int g_head = 0;      // next slot to write
static int g_count = 0;
static DWORD g_startTick = 0;

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

static char g_moduleDir[MAX_PATH] = {0};
static char g_gameDir[MAX_PATH] = {0};

const char *WrModuleDir(void)
{
    if (g_moduleDir[0])
        return g_moduleDir;

    if (!GetModuleFileNameA(g_wrSelf, g_moduleDir, MAX_PATH))
    {
        strcpy_s(g_moduleDir, MAX_PATH, ".");
        return g_moduleDir;
    }
    char *slash = strrchr(g_moduleDir, '\\');
    if (slash)
        *slash = '\0';
    return g_moduleDir;
}

bool WrIsWine(void)
{
    // Cached: the answer cannot change inside a process, and this is read from
    // the panel every frame the Diagnostics tab is open.
    static int cached = -1;
    if (cached < 0)
    {
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        cached = (nt && GetProcAddress(nt, "wine_get_version")) ? 1 : 0;
    }
    return cached != 0;
}

const char *WrGameDir(void)
{
    if (g_gameDir[0])
        return g_gameDir;

    // <root>\bin\win64\momentum.exe  ->  <root>
    char exe[MAX_PATH];
    if (!GetModuleFileNameA(NULL, exe, MAX_PATH))
        return g_gameDir;

    strcpy_s(g_gameDir, MAX_PATH, exe);
    for (int i = 0; i < 3; i++)
    {
        char *slash = strrchr(g_gameDir, '\\');
        if (!slash)
        {
            g_gameDir[0] = '\0';
            return g_gameDir;
        }
        *slash = '\0';
    }
    return g_gameDir;
}

bool WrPathIsAscii(const char *s)
{
    if (!s)
        return true;
    // Unsigned, deliberately. A plain char is signed on MSVC, so `*p >= 0x80`
    // on the raw type is false for every byte that has the high bit set --
    // which is precisely the set this exists to find.
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p >= 0x80)
            return false;
    return true;
}

void WrFileStem(const char *path, char *out, int cap)
{
    if (!out || cap <= 0)
        return;
    out[0] = '\0';
    if (!path)
        return;

    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    strncpy_s(out, (size_t)cap, base, _TRUNCATE);

    char *dot = strrchr(out, '.');
    if (!dot)
        return;

    // Everything before the last dot being itself a dot is Python's "there is
    // no extension here" -- see the header. The loop, not a `dot != out` test,
    // because "..mtv" has a dot at index 1 and still must not be split.
    for (const char *p = out; p < dot; p++)
        if (*p != '.')
        {
            *dot = '\0';
            return;
        }
}

bool WrMakeTree(const char *dir)
{
    char buf[MAX_PATH];
    strcpy_s(buf, sizeof(buf), dir);
    for (char *p = buf; *p; p++)
    {
        if (*p != '\\' && *p != '/')
            continue;
        char was = *p;
        *p = '\0';
        // Not "C:", which is not a directory anybody can create, and not the
        // empty string a leading separator would produce.
        if (buf[0] && !(buf[1] == ':' && buf[2] == '\0'))
            CreateDirectoryA(buf, NULL);
        *p = was;
    }
    CreateDirectoryA(buf, NULL);
    return GetFileAttributesA(buf) != INVALID_FILE_ATTRIBUTES;
}

// Build the path and make sure every directory along it exists. Returns a
// pointer to a rotating set of static buffers so a couple of calls can be live
// in one expression without stomping each other.
//
// FOUR ROTATING BUFFERS AND NO LOCK, WHICH IS NOW A RULE RATHER THAN A RISK.
// Two threads calling this can be handed the same buffer. It was safe while
// only the UI thread and one background counter used it; extraction runs a
// worker pool. The rule that makes it safe again is in wr_jobs.h and it is
// structural rather than remembered: this is resolved ONCE PER JOB, on the
// coordinator, before any worker starts, and workers are handed absolute paths
// with nothing left to look up.
const char *WrDataPath(const char *rel)
{
    static char bufs[4][MAX_PATH];
    static int which = 0;
    char *out = bufs[which];
    which = (which + 1) & 3;

    _snprintf_s(out, MAX_PATH, _TRUNCATE, "%s\\wrlines_data%s%s",
                WrModuleDir(), (rel && *rel) ? "\\" : "", rel ? rel : "");

    // Create each directory component in turn, skipping the final element,
    // which is assumed to be a file name unless it ends in a separator.
    char tmp[MAX_PATH];
    strcpy_s(tmp, MAX_PATH, out);
    char *last = strrchr(tmp, '\\');
    if (last)
    {
        *last = '\0';
        WrMakeTree(tmp);
    }
    return out;
}

// ---------------------------------------------------------------------------
// The clock
// ---------------------------------------------------------------------------

// Every timestamp this program writes into a file goes through here, and there
// is exactly one reason for that: WRLINES_FAKE_NOW.
//
// Two stamps exist -- the board cache's "fetched" line and, once the extractor
// is native, the .wrpath header's 0xF4. Neither is read back for any decision.
// They are also the ONLY reason two runs over the same inputs do not produce
// identical bytes, which makes them the only thing standing between "did the
// port write the same file" and fc /b.
//
// So this is a feature before it is a test hook. Pinning it means you can
// re-fetch a board or re-extract a map and diff the result against what you
// had, and see only what actually changed. That it also makes the port
// checkable at all is the reason it exists, and there is no sense pretending
// otherwise -- but a user who sets it gets something real.
//
// Same environment variable and same semantics as the reference's _now(): a
// value that will not parse as an integer is ignored rather than treated as
// zero, because a typo should not silently backdate everything to 1970.
long long WrNowEpoch(void)
{
    char v[64];
    DWORD n = GetEnvironmentVariableA("WRLINES_FAKE_NOW", v, sizeof(v));
    if (n > 0 && n < sizeof(v))
    {
        // int("...") IGNORES SURROUNDING WHITESPACE, and this has to as well.
        // `set WRLINES_FAKE_NOW=1700000000` in a batch file keeps the trailing
        // space cmd puts before the newline, and a strict terminator check on
        // that silently fell through to the live clock -- which shows up as
        // four differing bytes at 0xF4 and a differing CRC, i.e. as the port
        // getting the file wrong, in the one variable whose entire job is to
        // make the two comparable. _strtoi64 already skips leading space; the
        // trailing end is ours to skip.
        char *end = NULL;
        long long pinned = _strtoi64(v, &end, 10);
        if (end && end != v)
        {
            while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n' ||
                   *end == '\v' || *end == '\f')
                end++;
            if (*end == '\0')
                return pinned;
        }
    }

    // int(time.time()), which truncates toward zero rather than rounds.
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    unsigned long long ticks =
        ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (long long)(ticks / 10000000ULL) - 11644473600LL;
}

// ---------------------------------------------------------------------------
// Saying what happened, when what happened is that it stopped
// ---------------------------------------------------------------------------
//
// This process has died twice without leaving a line behind, and there are two
// separate ways for that to happen. Both are covered here.
//
// A read of address zero inside our own code took a minidump and a hex editor
// to place. The exception went straight past us to the game's own reporter,
// which wrote a .dmp naming a module and an offset and nothing else; turning
// `wrlines.dll+0x19FBD` into a function meant parsing the PE exception table by
// hand. WrCrashFilter writes that line itself, with a walked stack, and then
// hands the exception on so the game's reporter still gets its dump.
//
// The other way is quieter and worse. The secure CRT calls the invalid-
// parameter handler for things like a strcpy_s whose source does not fit, and
// the DEFAULT HANDLER TERMINATES THE PROCESS -- no exception raised, so no
// filter runs, no unwind happens, and nothing is written anywhere. See the note
// above the truncation in EmitEnergyHud, which is where this was first noticed.
// A process that vanishes with a full log ending mid-frame looks exactly like a
// GPU hang, and is not one.
//
// Both handlers are process-wide, because that is the only kind there is. We
// are a guest here: neither swallows anything, and the crash filter chains to
// whatever was installed before us.

static LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = NULL;

// "wrlines.dll+0x19FBD" for a code address.
//
// GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS rather than psapi's EnumProcessModules:
// no extra import, and it does not walk the module list, which matters when the
// thing that just faulted may have been holding the loader lock.
static void WrDescribeAddress(const void *addr, char *out, int cap)
{
    HMODULE mod = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod) &&
        mod)
    {
        char path[MAX_PATH];
        if (GetModuleFileNameA(mod, path, MAX_PATH))
        {
            const char *base = strrchr(path, '\\');
            base = base ? base + 1 : path;
            _snprintf_s(out, cap, _TRUNCATE, "%s+0x%llX", base,
                        (unsigned long long)((const char *)addr -
                                             (const char *)mod));
            return;
        }
    }
    _snprintf_s(out, cap, _TRUNCATE, "0x%016llX (no module)",
                (unsigned long long)(uintptr_t)addr);
}

// x64 keeps no frame pointer to follow, so this walks the unwind data the ABI
// requires every non-leaf function to publish -- the same tables the PE's
// .pdata section holds. RtlLookupFunctionEntry returns null for a leaf, which
// is normal at the very top of a stack and is handled the way the ABI says: the
// return address is sitting at RSP.
static void WrLogBacktrace(const CONTEXT *from)
{
    CONTEXT c = *from;
    for (int frame = 0; frame < 24; frame++)
    {
        char where[192];
        WrDescribeAddress((const void *)c.Rip, where, (int)sizeof(where));
        WrLogf("crash:   [%2d] %s", frame, where);

        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &imageBase, NULL);
        if (fn)
        {
            PVOID handlerData = NULL;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fn, &c,
                             &handlerData, &establisher, NULL);
        }
        else
        {
            if (!c.Rsp)
                break;
            c.Rip = *(DWORD64 *)c.Rsp;
            c.Rsp += 8;
        }
        if (!c.Rip)
            break;
    }
}

static LONG CALLBACK WrCrashFilter(EXCEPTION_POINTERS *ep)
{
    // Once only. If describing the crash crashes, the second pass must not come
    // back through here and loop.
    static LONG entered = 0;
    if (InterlockedExchange(&entered, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    __try
    {
        const EXCEPTION_RECORD *er = ep->ExceptionRecord;
        char where[192];
        WrDescribeAddress(er->ExceptionAddress, where, (int)sizeof(where));

        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            er->NumberParameters >= 2)
        {
            const ULONG_PTR op = er->ExceptionInformation[0];
            const char *what = (op == 0) ? "reading"
                             : (op == 1) ? "writing"
                                         : "executing";
            WrLogf("[!] crash: 0x%08lX %s 0x%llX at %s",
                   (unsigned long)er->ExceptionCode, what,
                   (unsigned long long)er->ExceptionInformation[1], where);
        }
        else
        {
            WrLogf("[!] crash: 0x%08lX at %s",
                   (unsigned long)er->ExceptionCode, where);
        }
        WrLogBacktrace(ep->ContextRecord);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Nothing useful left to say, and saying it would fault again.
    }

    return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

static void __cdecl WrInvalidParameter(const wchar_t *expr, const wchar_t *func,
                                       const wchar_t *file, unsigned int line,
                                       uintptr_t)
{
    // The release CRT passes null for all four of these unless _DEBUG is on, so
    // usually the only thing recovered is the fact that it happened and where
    // the stack was. That is still the difference between a bug report and a
    // shrug.
    char e[160], f[160], fl[MAX_PATH];
    strcpy_s(e, sizeof(e), "?");
    strcpy_s(f, sizeof(f), "?");
    strcpy_s(fl, sizeof(fl), "?");
    if (expr) WideCharToMultiByte(CP_UTF8, 0, expr, -1, e, (int)sizeof(e), NULL, NULL);
    if (func) WideCharToMultiByte(CP_UTF8, 0, func, -1, f, (int)sizeof(f), NULL, NULL);
    if (file) WideCharToMultiByte(CP_UTF8, 0, file, -1, fl, (int)sizeof(fl), NULL, NULL);

    WrLogf("[!] CRT invalid parameter: %s in %s (%s:%u) -- the CRT is about to "
           "terminate the process", e, f, fl, line);

    CONTEXT c;
    RtlCaptureContext(&c);
    WrLogBacktrace(&c);
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

void WrLogInit(void)
{
    if (g_lockReady)
        return;
    InitializeCriticalSection(&g_lock);
    g_lockReady = true;
    g_startTick = GetTickCount();

    // Keep exactly one generation. The log opens "w" and is therefore truncated
    // on every injection -- which is fine until the run you actually want to
    // read is the one before this one, because the game died and you relaunched
    // it to find out why. That is not a hypothetical; it cost a crash.
    // Both copied out rather than held: WrDataPath rotates four static buffers,
    // so two live pointers happen to be safe today, and depending on that is how
    // you get a rename of a file onto itself the day it becomes three.
    char path[MAX_PATH], prev[MAX_PATH];
    strcpy_s(path, sizeof(path), WrDataPath("wrlines.log"));
    strcpy_s(prev, sizeof(prev), WrDataPath("wrlines.prev.log"));
    MoveFileExA(path, prev, MOVEFILE_REPLACE_EXISTING);

    // Shared, not exclusive. fopen would lock the file for as long as the game
    // runs, and the log is least useful exactly when you cannot restart to read
    // it. _SH_DENYNO lets you tail it live from outside.
    g_file = _fsopen(path, "w", _SH_DENYNO);

    g_prevFilter = SetUnhandledExceptionFilter(WrCrashFilter);
    _set_invalid_parameter_handler(WrInvalidParameter);

    SYSTEMTIME st;
    GetLocalTime(&st);
    WrLogf("=== WrLines " WRLINES_VERSION " === %04d-%02d-%02d %02d:%02d:%02d",
           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    WrLogf("module dir : %s", WrModuleDir());
    WrLogf("game dir   : %s", WrGameDir());
}

void WrLogf(const char *fmt, ...)
{
    char line[WR_LOG_LINE_MAX];
    DWORD ms = GetTickCount() - g_startTick;

    int n = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%3lu.%03lu] ",
                        ms / 1000, ms % 1000);
    if (n < 0)
        n = 0;

    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line + n, sizeof(line) - n, _TRUNCATE, fmt, ap);
    va_end(ap);

    if (!g_lockReady)
        return;

    EnterCriticalSection(&g_lock);
    strcpy_s(g_ring[g_head], WR_LOG_LINE_MAX, line);
    g_head = (g_head + 1) % WR_LOG_LINES;
    if (g_count < WR_LOG_LINES)
        g_count++;
    if (g_file)
    {
        fputs(line, g_file);
        fputc('\n', g_file);
        fflush(g_file);
    }
    LeaveCriticalSection(&g_lock);
}

int WrLogCount(void)
{
    return g_count;
}

const char *WrLogLine(int index)
{
    if (index < 0 || index >= g_count)
        return "";
    int start = (g_count < WR_LOG_LINES) ? 0 : g_head;
    return g_ring[(start + index) % WR_LOG_LINES];
}

// ---------------------------------------------------------------------------
// Small maths helpers (here rather than a whole translation unit of their own)
// ---------------------------------------------------------------------------

float WrLength(const Vec3 &v)
{
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

float WrDistSqr(const Vec3 &a, const Vec3 &b)
{
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

float WrDist(const Vec3 &a, const Vec3 &b)
{
    return sqrtf(WrDistSqr(a, b));
}

Vec3 WrNormalize(const Vec3 &v)
{
    float len = WrLength(v);
    if (len < 1e-6f)
        return WrVec(0.0f, 0.0f, 0.0f);
    return WrScale(v, 1.0f / len);
}

bool WrSaneFloat(float f)
{
    // The self-comparison catches NaN and the magnitude bound catches both
    // infinities and the merely absurd -- which is what a probe returns when it
    // called something that was not what we hoped.
    return (f == f) && f > -1e9f && f < 1e9f;
}

bool WrSaneVec(const Vec3 &v)
{
    return WrSaneFloat(v.x) && WrSaneFloat(v.y) && WrSaneFloat(v.z);
}
