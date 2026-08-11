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
// Logging
// ---------------------------------------------------------------------------

void WrLogInit(void)
{
    if (g_lockReady)
        return;
    InitializeCriticalSection(&g_lock);
    g_lockReady = true;
    g_startTick = GetTickCount();

    // Shared, not exclusive. fopen would lock the file for as long as the game
    // runs, and the log is least useful exactly when you cannot restart to read
    // it. _SH_DENYNO lets you tail it live from outside.
    const char *path = WrDataPath("wrlines.log");
    g_file = _fsopen(path, "w", _SH_DENYNO);

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
