// wr_scan.cpp  --  read-only search for the world->screen matrix.
//
// Two phases, cheapest and most likely first:
//
//   1. The writable data of the game's own modules (engine.dll, client.dll and
//      friends). In stock Source the engine's renderer is a file-static global,
//      so its cached view-projection matrix lives in .data/.bss. A few MB.
//
//   2. Everything else committed and writable in the process. Bigger, slower,
//      capped, and only reached if phase 1 came up empty -- but it covers the
//      case where the matrix is a member of a heap-allocated object.
//
// Only *writable* memory is considered: this matrix is rewritten every frame, so
// it cannot be in a read-only section, and skipping read-only pages removes most
// of the address space for free.
//
// The scan runs on a background thread at below-normal priority. It never writes
// to the game and never calls into it, so there is nothing to synchronise beyond
// the candidate list itself.

#include "wr_scan.h"
#include "wr_matrixlife.h"
#include "wr_engine.h"
#include "wr_probe.h"
#include "wr_hook.h"
#include "wr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_CANDIDATES 192
#define SCAN_CHUNK (256 * 1024)
#define MATRIX_BYTES 64
#define LOG_FIRST_N 24          // log this many candidates, then just count

// Phase 2 is bounded. A game process can map several GB; walking all of it would
// take long enough to look like a hang in the Diagnostics tab, and if the matrix
// is not in the first half-gig of writable private memory it is not going to be
// found by brute force at all.
#define PHASE2_BUDGET_MB 768

// A candidate has to survive this many frames of re-validation before it can be
// picked, and lose this many in a row before it is dropped.
#define MIN_HITS_TO_PICK 90
#define MAX_MISSES 90

// When a chosen address has died, and how that is told apart from a level load,
// live in wr_matrixlife.h -- pure arithmetic, so it can be tested against a
// script of frames instead of only against a running game.

// The live world->clip matrix is rewritten every frame; a constant projection
// matrix compiled into engine.dll's data is not. Structural checks cannot tell
// those apart -- only watching them can.
#define MIN_UPDATES_TO_PICK 30

// ...but "it changes" is not enough either, and the second wrong pick proved it.
// engine.dll holds an ARRAY of view matrices -- skybox, cubemap faces, shadow
// cascades, water reflections -- and they update too. What none of them do is
// follow the player: the one that got picked sat at a fixed (-11729, -13536,
// 13744) for its whole life and reported 0 units travelled.
//
// So the camera this matrix describes has to actually move through the map...
//
// 512, not 96. At 96 the log read "96 units travelled" -- exactly the threshold,
// meaning the winner was simply whichever candidate crossed the line first, on a
// frame where they all crossed it together, decided by array order. There is
// nothing to rank on until the candidates have had time to differ. 512 units is
// a couple of seconds of moving, once per session, and it buys a pick made on
// evidence instead of on a race.
#define MIN_TRAVEL_TO_PICK 512.0f

// ...and it has to move *continuously*. A slot shared between render passes
// jumps thousands of units between frames as different views are written into
// it, which is what made the line snap between world-locked and screen-locked.
// A real player camera cannot move more than a few hundred units in one frame.
//
// This one is only used for RANKING candidates, where a per-frame threshold is
// fine because every candidate is judged over the same frames. The per-frame
// acceptance gate uses a speed instead -- see MAX_CAMERA_SPEED.
#define JUMP_UNITS 900.0f
#define MAX_JUMP_FRACTION 0.05f

// The acceptance gate, in units per SECOND.
//
// A fixed per-frame distance cannot work here: 900 units at 300 fps is 270,000
// u/s, but a single 250 ms hitch at surf speed covers 1000 units and reads as a
// teleport. That is exactly what made the lines freeze to the screen once enough
// runs were loaded to cause hitching. Dividing by real elapsed time makes the
// test independent of frame rate. 12,000 u/s is far above any surf speed --
// surf_demise peaks near 4,900 -- and far below a teleport.
#define MAX_CAMERA_SPEED 12000.0f

// Even the right address can be caught mid-overwrite or hold another pass's view
// on a given frame. Rather than draw with it, hold the previous good matrix --
// unless the new position persists, which means the player genuinely teleported.
#define TELEPORT_CONFIRM_FRAMES 6

// ...but hold it for this long and no longer. A stray pass lasts a frame or two;
// beyond that, continuing to draw the world through a matrix we have decided is
// wrong is worse than drawing nothing, because it renders as lines welded to the
// screen while the player moves.
#define MAX_HELD_FRAMES 3

struct Candidate
{
    const unsigned char *addr;
    bool transposed;
    bool alive;
    int hits;
    int misses;
    int consecutiveMisses;
    int changes;
    VMatrix last;
    bool haveLast;
    // How far the solved camera origin has travelled while we watched. The live
    // matrix tracks the player through the map; anything else sits still.
    Vec3 lastOrigin;
    bool haveOrigin;
    float travel;
    int jumps;              // frames the origin moved implausibly far
    bool changedThisFrame;  // did the bytes differ from last time we looked

    // The optics this matrix describes. Two candidates can be equally valid
    // world->clip matrices for this viewport and still disagree about the field
    // of view, because they belong to different render passes -- and picking the
    // wrong one draws lines that are right at the crosshair and wrong at the
    // edges. Recorded so the difference is visible instead of invisible.
    float fov;              // horizontal, degrees
    float aspect;

    bool pinned;            // the user vouched for this address, see the pin file
    char note[128];
};

static CRITICAL_SECTION g_cs;
static bool g_csReady = false;

static Candidate g_cand[MAX_CANDIDATES];
static int g_candCount = 0;

static HANDLE g_thread = NULL;
static volatile LONG g_stop = 0;
static volatile LONG g_busy = 0;
static volatile LONG g_phase = 0;
static volatile LONG64 g_bytes = 0;
static volatile LONG g_restartPending = 0;

static int g_frames = 0;
static int g_chosen = -1;
static int g_staleFrames = 0;
static int g_restarts = 0;
static int g_idleFrames = 0;
static bool g_everStarted = false;

// If the scan has finished and nothing usable turned up, try again rather than
// sitting there forever. The common case is injecting at the main menu, where
// there is no camera for the oracle to confirm against and half the candidates
// belong to memory that gets freed on map load.
#define IDLE_RESCAN_FRAMES 1800
#define MAX_AUTO_RESCANS 5

// How many non-chosen candidates to re-check per frame once a winner is picked.
// They are no longer consulted for anything, so this exists purely to keep the
// alive count in Diagnostics truthful.
#define REVALIDATE_PER_FRAME 2

// Validations a candidate may accumulate without its camera going anywhere
// before it is written off as a fixed viewpoint (skybox, cubemap face, shadow
// cascade). Deliberately large: see the guard where this is used.
#define FIXED_VIEWPOINT_HITS 1200

static VMatrix g_matrix;
static bool g_matrixValid = false;

// Frame-to-frame continuity state for the chosen address; see AcceptForFrame.
static Vec3 g_lastOrigin;
static bool g_haveLastOrigin = false;
static Vec3 g_pendingOrigin;
static int g_pendingFrames = 0;
static long long g_lastAcceptTicks = 0;
static int g_heldFrames = 0;        // consecutive frames drawing a held matrix
static int g_roundRobin = 0;        // where the slow re-check left off

// How long the chosen matrix has been byte-identical. See WrScanTick.
static long long g_frozenSince = 0;
static float g_frozenSeconds = 0.0f;
static float g_frozenLimit = 1.5f;

// Death detection for the chosen address; see wr_matrixlife.h.
static WrMatrixLife g_life = {0.0f, 0.0f, false};
static long long g_tickTicks = 0;        // for this function's own dt
static long long g_othersMovedTicks = 0; // last time any other candidate changed
static long long g_lastLiveTicks = 0;    // last frame we had a usable matrix

// Freeze detection versus the panel.
//
// Standing perfectly still in a map produces a byte-identical matrix, so the
// freeze cutoff fires and the lines stop. The most common way to stand perfectly
// still is to open this panel -- input goes to the panel, not the game -- so
// ticking runs on and off made the very lines you were ticking disappear a
// second and a half later. Exactly backwards.
//
// So while the panel is open the cutoff is suspended and the last good matrix is
// held. Not unconditionally: it is only held if the matrix was live shortly
// before the panel was opened, which is what stops it from painting a dead
// route over the main menu if you open the panel there.
static bool g_menuWasOpen = false;
static bool g_holdForPanel = false;
#define PANEL_HOLD_GRACE 10.0f      // seconds since live, at the moment it opens

// Only two non-chosen candidates are re-checked per frame, so "did another
// candidate change on this exact frame" is mostly a question about which two
// came up in the round robin. Remembering the last time one did, and treating
// the world as live for a short while afterwards, turns that into a signal that
// tracks real time instead of sampling luck.
#define WORLD_ALIVE_MEMORY 0.5f

// The performance counter frequency is a constant for the life of the process.
static long long QpcFreq(void)
{
    static long long f = 0;
    if (f == 0)
    {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        f = q.QuadPart ? q.QuadPart : 1;
    }
    return f;
}

static inline long long Now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static bool AcceptForFrame(const VMatrix &m);

static char g_status[192] = "not started";
static char g_note[128] = {0};

// ---------------------------------------------------------------------------
// The pinned address
// ---------------------------------------------------------------------------
//
// Written only when the user presses "Remember this one". Stored as module +
// offset, never as an absolute address: ASLR moves engine.dll every launch, so
// an absolute address remembered from last session would point at something
// else entirely -- which is precisely the failure this is meant to prevent.
//
// It is a hint, not an override. The pinned address is added as a candidate and
// still has to pass the full oracle every frame like any other; if a game update
// moves it, it simply never validates and the ordinary scan takes over.

static char g_pinModule[64] = {0};
static unsigned long long g_pinOffset = 0;
static bool g_pinTransposed = false;
static bool g_pinValid = false;
static char g_pinDesc[128] = {0};

static const char *PinPath(void) { return WrDataPath("wrlines_matrix.ini"); }

static bool AddrToModule(const void *p, char *mod, int modLen,
                         unsigned long long *off)
{
    HMODULE h = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)p, &h) || !h)
        return false;

    char path[MAX_PATH];
    if (!GetModuleFileNameA(h, path, sizeof(path)))
        return false;
    const char *slash = strrchr(path, '\\');
    strcpy_s(mod, modLen, slash ? slash + 1 : path);
    *off = (unsigned long long)((const unsigned char *)p -
                                (const unsigned char *)h);
    return true;
}

static void LoadPin(void)
{
    static bool loaded = false;
    if (loaded)
        return;
    loaded = true;

    FILE *f = NULL;
    if (fopen_s(&f, PinPath(), "r") != 0 || !f)
        return;

    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        if (line[0] == ';' || line[0] == '#' || line[0] == '[')
            continue;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *val = eq + 1;
        while (*val == ' ' || *val == '\t')
            val++;
        for (int i = (int)strlen(line) - 1; i >= 0 &&
             (line[i] == ' ' || line[i] == '\t'); i--)
            line[i] = '\0';

        if (_stricmp(line, "module") == 0)
        {
            strcpy_s(g_pinModule, sizeof(g_pinModule), val);
            for (int i = (int)strlen(g_pinModule) - 1; i >= 0 &&
                 (g_pinModule[i] == '\n' || g_pinModule[i] == '\r' ||
                  g_pinModule[i] == ' '); i--)
                g_pinModule[i] = '\0';
        }
        else if (_stricmp(line, "offset") == 0)
            g_pinOffset = _strtoui64(val, NULL, 0);
        else if (_stricmp(line, "transposed") == 0)
            g_pinTransposed = (atoi(val) != 0);
    }
    fclose(f);

    if (g_pinModule[0] && g_pinOffset)
    {
        g_pinValid = true;
        _snprintf_s(g_pinDesc, sizeof(g_pinDesc), _TRUNCATE, "%s+0x%llX%s",
                    g_pinModule, g_pinOffset,
                    g_pinTransposed ? " (transposed)" : "");
        WrLogf("scan: remembered matrix %s (will still be validated)", g_pinDesc);
    }
}

static void SavePin(void)
{
    FILE *f = NULL;
    if (fopen_s(&f, PinPath(), "w") != 0 || !f)
        return;
    fprintf(f, "; WrLines remembered world->screen matrix.\n");
    fprintf(f, "; Module-relative on purpose: ASLR moves the module every launch.\n");
    fprintf(f, "; This is only a hint -- it is re-validated every frame, so a game\n");
    fprintf(f, "; update that moves it just falls back to scanning.\n");
    fprintf(f, "; Delete this file, or press Forget in Diagnostics, to clear it.\n\n");
    fprintf(f, "module     = %s\n", g_pinModule);
    fprintf(f, "offset     = 0x%llX\n", g_pinOffset);
    fprintf(f, "transposed = %d\n", g_pinTransposed ? 1 : 0);
    fclose(f);
}

// Resolve the pin against this launch's module base. NULL if the module is not
// loaded or the offset is past the end of its image.
static const unsigned char *ResolvePin(void)
{
    if (!g_pinValid)
        return NULL;
    HMODULE h = GetModuleHandleA(g_pinModule);
    if (!h)
        return NULL;

    IMAGE_DOS_HEADER dos;
    if (!WrSafeReadBytes(h, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE)
        return NULL;
    IMAGE_NT_HEADERS64 nt;
    if (!WrSafeReadBytes((const unsigned char *)h + dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE)
        return NULL;
    if (g_pinOffset + MATRIX_BYTES > nt.OptionalHeader.SizeOfImage)
        return NULL;
    return (const unsigned char *)h + g_pinOffset;
}

// ---------------------------------------------------------------------------
// Candidate bookkeeping
// ---------------------------------------------------------------------------

static void BuildMatrix(const unsigned char *src, bool transposed, VMatrix *out)
{
    const float *f = (const float *)src;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out->m[r][c] = transposed ? f[c * 4 + r] : f[r * 4 + c];
}

// Called from the scan thread when a 64-byte window passes the full oracle.
static void AddCandidate(const unsigned char *addr, bool transposed,
                         const char *note)
{
    EnterCriticalSection(&g_cs);
    bool dup = false;
    for (int i = 0; i < g_candCount; i++)
        if (g_cand[i].addr == addr && g_cand[i].transposed == transposed)
        {
            dup = true;
            break;
        }
    if (!dup && g_candCount < MAX_CANDIDATES)
    {
        // Fill it in first and publish the count last. The Diagnostics list
        // reads this array from the render thread without taking the lock, and
        // incrementing first would let it read a half-written entry.
        Candidate *c = &g_cand[g_candCount];
        memset(c, 0, sizeof(*c));
        c->addr = addr;
        c->transposed = transposed;
        c->alive = true;
        if (note)
            strcpy_s(c->note, sizeof(c->note), note);
        int index = g_candCount;
        g_candCount++;
        if (g_candCount <= LOG_FIRST_N)
            WrLogf("scan: candidate %d @ %p%s  %s", index, addr,
                   transposed ? " (transposed)" : "", note ? note : "");
    }
    LeaveCriticalSection(&g_cs);
}

// ---------------------------------------------------------------------------
// Reading memory without trusting it
// ---------------------------------------------------------------------------

// ReadProcessMemory on our own process: returns false for an unmapped or
// freed page instead of raising, which is exactly what we want while walking
// memory another thread is busy reallocating.
static size_t ReadChunk(const unsigned char *addr, unsigned char *buf, size_t want)
{
    SIZE_T got = 0;
    if (ReadProcessMemory(GetCurrentProcess(), addr, buf, want, &got) && got == want)
        return want;

    // A single bad page inside the chunk shouldn't cost us the whole chunk.
    size_t total = 0;
    while (total < want)
    {
        size_t step = want - total;
        if (step > 4096)
            step = 4096;
        SIZE_T n = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), addr + total, buf + total,
                               step, &n) || n != step)
            break;
        total += step;
    }
    return total;
}

// ---------------------------------------------------------------------------
// The scan itself
// ---------------------------------------------------------------------------

// Cheap structural pre-filter, applied at every 4-byte offset. In a
// view-projection matrix the w row is the (unit-length) view forward vector, so
// its squared length is 1. The window is generous; the point is that random
// float32 bit patterns almost never land in it, so the expensive oracle runs on
// a tiny fraction of offsets.
static inline bool PlausibleWRow(float a, float b, float c)
{
    float l2 = a * a + b * b + c * c;
    return l2 >= 0.09f && l2 <= 9.0f;
}

static void ScanBuffer(const unsigned char *realBase, const unsigned char *buf,
                       size_t n)
{
    if (n < MATRIX_BYTES)
        return;
    size_t last = n - MATRIX_BYTES;
    char note[128];

    for (size_t off = 0; off <= last; off += 4)
    {
        if (g_stop)
            return;

        const float *f = (const float *)(buf + off);

        // Row-major: the w row is m[3][*], i.e. floats 12..15.
        if (PlausibleWRow(f[12], f[13], f[14]))
        {
            VMatrix m;
            BuildMatrix(buf + off, false, &m);
            note[0] = '\0';
            if (WrValidateW2S(m, note, sizeof(note)))
                AddCandidate(realBase + off, false, note);
        }

        // Column-major: the same row is the 4th column, floats 3, 7, 11, 15.
        // Costs one extra check per offset and covers a whole class of "Strata
        // stores it the other way round" that would otherwise look like the
        // matrix simply not being there.
        if (PlausibleWRow(f[3], f[7], f[11]))
        {
            VMatrix m;
            BuildMatrix(buf + off, true, &m);
            note[0] = '\0';
            if (WrValidateW2S(m, note, sizeof(note)))
                AddCandidate(realBase + off, true, note);
        }
    }
}

static bool WritableCommitted(const MEMORY_BASIC_INFORMATION &mbi)
{
    if (mbi.State != MEM_COMMIT)
        return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
        return false;
    DWORD p = mbi.Protect & 0xFF;
    return p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
           p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

// Scan one contiguous run of memory, in overlapping chunks so a matrix that
// straddles a chunk boundary is still seen.
static void ScanRange(const unsigned char *base, size_t size)
{
    static unsigned char buf[SCAN_CHUNK + MATRIX_BYTES];

    size_t off = 0;
    while (off < size && !g_stop)
    {
        size_t want = size - off;
        if (want > SCAN_CHUNK + MATRIX_BYTES)
            want = SCAN_CHUNK + MATRIX_BYTES;

        size_t got = ReadChunk(base + off, buf, want);
        if (got >= MATRIX_BYTES)
            ScanBuffer(base + off, buf, got);

        InterlockedAdd64(&g_bytes, (LONG64)want);
        off += SCAN_CHUNK;
    }
}

// Walk [begin, end) with VirtualQuery and scan every writable committed region.
// Returns bytes of address space covered.
static size_t ScanSpan(const unsigned char *begin, const unsigned char *end)
{
    const unsigned char *p = begin;
    size_t covered = 0;

    while (p < end && !g_stop)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi))
            break;

        const unsigned char *regionBase = (const unsigned char *)mbi.BaseAddress;
        size_t regionSize = mbi.RegionSize;
        const unsigned char *next = regionBase + regionSize;
        if (next <= p)
            break;                          // no forward progress; bail out

        if (WritableCommitted(mbi))
        {
            const unsigned char *from = regionBase > begin ? regionBase : begin;
            const unsigned char *to = next < end ? next : end;
            if (to > from)
            {
                ScanRange(from, (size_t)(to - from));
                covered += (size_t)(to - from);
            }
        }
        p = next;
    }
    return covered;
}

static bool ModuleRange(const char *name, const unsigned char **base, size_t *size)
{
    HMODULE h = GetModuleHandleA(name);
    if (!h)
        return false;

    IMAGE_DOS_HEADER dos;
    if (!WrSafeReadBytes(h, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    IMAGE_NT_HEADERS64 nt;
    if (!WrSafeReadBytes((const unsigned char *)h + dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE)
        return false;

    *base = (const unsigned char *)h;
    *size = nt.OptionalHeader.SizeOfImage;
    return true;
}

// The modules whose data is worth looking at before resorting to the heap.
static const char *kPriorityModules[] = {
    "engine.dll",
    "client.dll",
    "materialsystem.dll",
    "shaderapidx11.dll",
    "server.dll",
};

static DWORD WINAPI ScanThread(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    InterlockedExchange(&g_busy, 1);
    InterlockedExchange(&g_phase, 1);

    // ---- Phase 1: the game's own module data -------------------------------
    const unsigned char *modBase[ARRAYSIZE(kPriorityModules)];
    size_t modSize[ARRAYSIZE(kPriorityModules)];
    int modCount = 0;

    for (int i = 0; i < ARRAYSIZE(kPriorityModules) && !g_stop; i++)
    {
        const unsigned char *b = NULL;
        size_t s = 0;
        if (!ModuleRange(kPriorityModules[i], &b, &s))
            continue;
        modBase[modCount] = b;
        modSize[modCount] = s;
        modCount++;
        WrLogf("scan: %s data @ %p (%.1f MB image)", kPriorityModules[i], b,
               s / (1024.0 * 1024.0));
        ScanSpan(b, b + s);
    }

    if (g_stop)
    {
        InterlockedExchange(&g_busy, 0);
        return 0;
    }

    EnterCriticalSection(&g_cs);
    int found = g_candCount;
    LeaveCriticalSection(&g_cs);

    WrLogf("scan: phase 1 done, %.1f MB, %d candidate%s",
           InterlockedCompareExchange64(&g_bytes, 0, 0) / (1024.0 * 1024.0),
           found, found == 1 ? "" : "s");

    // ---- Phase 2: the rest of the writable address space -------------------
    //
    // Only if module data came up empty. If phase 1 found anything at all, one
    // of those is the matrix and there is no reason to walk a gigabyte of heap.
    if (found == 0)
    {
        InterlockedExchange(&g_phase, 2);
        WrLogf("scan: nothing in module data, sweeping process memory");

        LONG64 startBytes = InterlockedCompareExchange64(&g_bytes, 0, 0);
        const unsigned char *p = (const unsigned char *)0x10000;
        const unsigned char *limit = (const unsigned char *)0x00007FFFFFFF0000ULL;

        while (p < limit && !g_stop)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi))
                break;
            const unsigned char *next =
                (const unsigned char *)mbi.BaseAddress + mbi.RegionSize;
            if (next <= p)
                break;

            // Module data was already covered in phase 1.
            bool skip = false;
            for (int i = 0; i < modCount; i++)
                if (p >= modBase[i] && p < modBase[i] + modSize[i])
                {
                    skip = true;
                    break;
                }

            if (!skip && WritableCommitted(mbi))
                ScanRange((const unsigned char *)mbi.BaseAddress, mbi.RegionSize);

            if ((InterlockedCompareExchange64(&g_bytes, 0, 0) - startBytes) >
                (LONG64)PHASE2_BUDGET_MB * 1024 * 1024)
            {
                WrLogf("scan: phase 2 hit its %d MB budget, stopping",
                       PHASE2_BUDGET_MB);
                break;
            }
            p = next;
        }
    }

    EnterCriticalSection(&g_cs);
    found = g_candCount;
    LeaveCriticalSection(&g_cs);

    WrLogf("scan: finished, %.1f MB scanned, %d candidate%s",
           InterlockedCompareExchange64(&g_bytes, 0, 0) / (1024.0 * 1024.0),
           found, found == 1 ? "" : "s");

    InterlockedExchange(&g_phase, 3);
    InterlockedExchange(&g_busy, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

static void EnsureCs(void)
{
    if (!g_csReady)
    {
        InitializeCriticalSection(&g_cs);
        g_csReady = true;
    }
}

// Seed the candidate list with the address the user vouched for, so a launch
// with a pin drawn lines within a frame or two instead of after a full sweep.
static void AddPinnedCandidate(void)
{
    const unsigned char *p = ResolvePin();
    if (!p)
        return;
    AddCandidate(p, g_pinTransposed, "remembered from last session");
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_candCount; i++)
        if (g_cand[i].addr == p && g_cand[i].transposed == g_pinTransposed)
            g_cand[i].pinned = true;
    LeaveCriticalSection(&g_cs);
    WrLogf("scan: seeded with remembered matrix %s @ %p", g_pinDesc, p);
}

void WrScanStart(void)
{
    if (g_thread)
        return;
    EnsureCs();

    // The oracle reprojects through the backbuffer dimensions, so there is no
    // point starting before we know them.
    int bw = 0, bh = 0;
    WrBackbufferSize(&bw, &bh);
    if (bw <= 0 || bh <= 0)
        return;

    LoadPin();
    AddPinnedCandidate();

    g_stop = 0;
    g_thread = CreateThread(NULL, 0, ScanThread, NULL, 0, NULL);
    if (g_thread)
    {
        g_everStarted = true;
        WrLogf("scan: started (read-only search for the world->screen matrix)");
    }
    else
        strcpy_s(g_status, sizeof(g_status), "could not start scan thread");
}

// A level load used to throw everything away and scan again. That was wrong in
// two separate ways, and between them they are most of "switching maps breaks
// the lines".
//
//   It stalls. The restart ran from inside Present and waited up to three
//   seconds for the scan thread to notice it should stop -- on the render
//   thread, during a level load.
//
//   It re-rolls the choice. Several different matrices in a frame pass the
//   oracle, and which one wins depends on the order candidates happened to be
//   found and how the camera happened to move in the first second. Rescanning
//   on every map change means re-entering that lottery every time, so the tool
//   can come back from a map change using a DIFFERENT matrix than it went in
//   with -- lines correct at the crosshair and progressively wrong towards the
//   edges of the screen, which is exactly what a mismatched field of view looks
//   like.
//
// The chosen address is re-validated in full every frame anyway. If the load
// really did move it, it stops validating and the stale-address path below
// rescans on its own. So the right thing on a map change is to keep the pick
// and only reset the per-frame continuity state, which the level load genuinely
// does invalidate: the camera teleports, and the matrix is frozen while the
// loading screen is up.
void WrScanOnMapChanged(void)
{
    if (!g_everStarted)
        return;

    g_haveLastOrigin = false;   // the camera legitimately jumps across a load
    g_pendingFrames = 0;
    g_lastAcceptTicks = 0;
    g_heldFrames = 0;
    g_frozenSince = 0;
    g_frozenSeconds = 0.0f;
    g_staleFrames = 0;
    WrMatrixLifeOnMapChange(&g_life);

    // A new level is a fresh situation: re-arm the automatic rescan budget that
    // earlier fumbling in this session may have used up. Without this, a chosen
    // address that dies later in the session is silently replaced from the
    // leftover candidate list instead of being scanned for -- which is exactly
    // what the log showed happening, three times in 1.5 seconds.
    g_restarts = 0;

    if (g_chosen >= 0)
    {
        // Keeping it is right when it is the matrix -- the address does not move
        // and rescanning would only re-roll the choice. But it is not proof, so
        // it goes on probation: see the death check in WrScanTick.
        WrLogf("scan: map changed -- keeping matrix @ %p, on probation until it "
               "is seen moving again", g_cand[g_chosen].addr);
        return;
    }

    // Nothing picked yet. If the sweep already finished empty, the candidates it
    // found at the menu are largely meaningless -- there was no player camera
    // for the oracle to confirm against -- so this is the moment to try again.
    // If it is still running, let it finish.
    if (InterlockedCompareExchange(&g_phase, 0, 0) >= 3)
    {
        g_restarts = 0;
        WrLogf("scan: map changed with nothing resolved, scanning again");
        WrScanRestart();
    }
}

// Asks for a restart and returns immediately. WrScanTick starts the new thread
// once the old one has actually exited.
void WrScanRestart(void)
{
    EnsureCs();
    InterlockedExchange(&g_stop, 1);
    InterlockedExchange(&g_restartPending, 1);

    g_chosen = -1;
    g_frames = 0;
    g_staleFrames = 0;
    g_idleFrames = 0;
    g_matrixValid = false;
    g_haveLastOrigin = false;
    g_pendingFrames = 0;
    g_lastAcceptTicks = 0;
    g_heldFrames = 0;
    g_roundRobin = 0;
    g_frozenSince = 0;
    g_frozenSeconds = 0.0f;
    WrMatrixLifeReset(&g_life);
    g_note[0] = '\0';
    strcpy_s(g_status, sizeof(g_status), "restarting the scan...");
    WrLogf("scan: restart requested");
}

// Called at the top of every tick. Does nothing until the old scan thread has
// exited on its own, so nothing here ever blocks the render thread.
static void ServiceRestart(void)
{
    if (!InterlockedCompareExchange(&g_restartPending, 0, 0))
        return;
    if (g_thread && WaitForSingleObject(g_thread, 0) != WAIT_OBJECT_0)
        return;                 // still winding down; try again next frame

    if (g_thread)
    {
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    EnterCriticalSection(&g_cs);
    g_candCount = 0;
    LeaveCriticalSection(&g_cs);

    // g_chosen is an INDEX into the list we just emptied. Leaving it set would
    // point it at whatever candidate the new scan happens to put in that slot.
    g_chosen = -1;
    g_matrixValid = false;
    g_staleFrames = 0;
    g_roundRobin = 0;
    WrMatrixLifeReset(&g_life);

    InterlockedExchange64(&g_bytes, 0);
    InterlockedExchange(&g_phase, 0);
    InterlockedExchange(&g_restartPending, 0);
    InterlockedExchange(&g_stop, 0);
    WrScanStart();
}

// Re-read one candidate and score it. Returns true if it validated this frame.
static bool ValidateCandidate(Candidate *c, VMatrix *out)
{
    unsigned char raw[MATRIX_BYTES];
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), c->addr, raw, sizeof(raw), &got) ||
        got != sizeof(raw))
        return false;

    VMatrix m;
    BuildMatrix(raw, c->transposed, &m);
    if (!WrValidateW2S(m, c->note, sizeof(c->note)))
        return false;

    bool changed = (!c->haveLast || memcmp(&c->last, &m, sizeof(m)) != 0);
    if (c->haveLast && changed)
        c->changes++;
    c->last = m;
    c->haveLast = true;
    c->changedThisFrame = changed;

    // What this matrix thinks the camera's optics are. |row0| is 1/tan(fovx/2)
    // and |row1|/|row0| is the aspect ratio -- the same two quantities the
    // oracle checks, kept here so the Diagnostics list can show why two equally
    // valid candidates disagree.
    float l0 = WrLength(WrVec(m.m[0][0], m.m[0][1], m.m[0][2]));
    float l1 = WrLength(WrVec(m.m[1][0], m.m[1][1], m.m[1][2]));
    if (l0 > 1e-6f)
    {
        c->fov = 2.0f * atanf(1.0f / l0) * 57.2957795f;
        c->aspect = l1 / l0;
    }

    // Track how the camera this matrix describes behaves over time. Smooth
    // travel means it is following the player; teleport-sized steps every frame
    // mean the slot is shared between render passes and we are seeing a
    // different view each time we look.
    Vec3 o;
    if (WrSolveCameraOrigin(m, &o))
    {
        if (c->haveOrigin)
        {
            float d = WrDist(c->lastOrigin, o);
            if (d > JUMP_UNITS)
                c->jumps++;
            else
                c->travel += d;
        }
        c->lastOrigin = o;
        c->haveOrigin = true;
    }

    if (out)
        *out = m;
    return true;
}

// Frame-to-frame continuity gate for the chosen address.
//
// Even the correct slot can be caught holding another render pass's view on a
// given frame, and drawing world-space lines through a skybox or reflection
// matrix is exactly what looked like "the line flickers into screen space".
// A real camera cannot cross most of a map in one frame, so a step that large
// means the contents are not ours this frame.
//
// The exception is a genuine teleport, which every staged surf map is full of.
// That is distinguished by persistence: a stray pass's view is gone next frame,
// whereas a teleport puts the camera somewhere new and it stays there. So a
// rejected position is remembered, and if it holds for a few frames it is
// accepted as real -- costing about a tenth of a second after a teleport rather
// than losing the line entirely.
static bool AcceptForFrame(const VMatrix &m)
{
    Vec3 o;
    if (!WrSolveCameraOrigin(m, &o))
        return false;

    // Elapsed time since the last accepted frame, so the test is a speed rather
    // than a distance. Clamped: a multi-second stall (alt-tab, loading) would
    // otherwise permit an arbitrarily large step.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = 0.0f;
    if (g_haveLastOrigin && g_lastAcceptTicks != 0)
        dt = (float)((double)(now.QuadPart - g_lastAcceptTicks) / (double)QpcFreq());
    g_lastAcceptTicks = now.QuadPart;
    dt = WrClampF(dt, 1.0f / 1000.0f, 0.5f);

    if (!g_haveLastOrigin)
    {
        g_lastOrigin = o;
        g_haveLastOrigin = true;
        return true;
    }

    float budget = MAX_CAMERA_SPEED * dt;
    if (budget < 64.0f)         // floor, so a 300 fps frame is not absurdly tight
        budget = 64.0f;

    if (WrDist(g_lastOrigin, o) <= budget)
    {
        g_lastOrigin = o;
        g_pendingFrames = 0;
        return true;
    }

    // Too far to be movement. Is it a teleport, or a stray frame?
    if (g_pendingFrames > 0 && WrDist(g_pendingOrigin, o) <= budget)
    {
        if (++g_pendingFrames >= TELEPORT_CONFIRM_FRAMES)
        {
            WrLogf("scan: camera jumped %.0f units in %.1f ms and stayed -- teleport",
                   WrDist(g_lastOrigin, o), dt * 1000.0f);
            g_lastOrigin = o;
            g_pendingFrames = 0;
            return true;
        }
    }
    else
    {
        g_pendingOrigin = o;
        g_pendingFrames = 1;
    }
    return false;
}

void WrScanTick(void)
{
    if (!g_csReady)
        return;

    ServiceRestart();

    // A restart that has been asked for but not yet serviced -- the scan thread
    // is still winding down. The candidates still in the list are about to be
    // thrown away, so do not pick one and do not draw through one.
    if (InterlockedCompareExchange(&g_restartPending, 0, 0))
    {
        g_matrixValid = false;
        return;
    }

    g_frames++;

    EnterCriticalSection(&g_cs);
    int n = g_candCount;
    LeaveCriticalSection(&g_cs);

    // Nothing usable yet and the sweep has finished? Try again after a while.
    if (g_chosen < 0 && InterlockedCompareExchange(&g_phase, 0, 0) >= 3)
    {
        if (++g_idleFrames > IDLE_RESCAN_FRAMES && g_restarts < MAX_AUTO_RESCANS)
        {
            g_restarts++;
            g_idleFrames = 0;
            WrLogf("scan: nothing usable after %d frames, re-scanning (%d/%d)",
                   IDLE_RESCAN_FRAMES, g_restarts, MAX_AUTO_RESCANS);
            WrScanRestart();
            return;
        }
    }
    else
    {
        g_idleFrames = 0;
    }

    if (n == 0)
    {
        g_matrixValid = false;
        if (InterlockedCompareExchange(&g_busy, 0, 0))
            _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                        "scanning (phase %d), %.0f MB so far",
                        (int)InterlockedCompareExchange(&g_phase, 0, 0),
                        WrScanMegabytes());
        else if (InterlockedCompareExchange(&g_phase, 0, 0) >= 3)
            strcpy_s(g_status, sizeof(g_status),
                     "scan found no world->screen matrix -- load into a map");
        return;
    }

    // Re-validate. Which candidates, and how often, depends entirely on whether
    // we have already picked a winner.
    //
    // Before we have: every candidate, every frame. That is the whole selection
    // process -- hits, changes and travel only accumulate by watching.
    //
    // After we have: the chosen one every frame, and a couple of the others in
    // round-robin. Re-running the full oracle on all 192 forever was pure waste:
    // once a winner is picked the others are never consulted again, because a
    // chosen address going stale triggers a complete re-scan rather than a
    // promotion. Each one is a ReadProcessMemory syscall plus a 3x3 solve, and
    // at 300 fps that was the single largest thing this tool did while idle.
    // Nothing is dropped, only re-checked slowly, so Diagnostics stays honest.
    int alive = 0;
    bool selecting = (g_chosen < 0);

    // This function's own elapsed time, for the death checks below.
    long long nowTicks = Now();
    float tickDt = 0.0f;
    if (g_tickTicks != 0)
        tickDt = (float)((double)(nowTicks - g_tickTicks) / (double)QpcFreq());
    g_tickTicks = nowTicks;
    tickDt = WrClampF(tickDt, 0.0f, 0.25f);

    bool chosenMoved = false;
    bool chosenValidated = false;
    bool othersMoved = false;

    // Decide the panel hold once, on the frame it opens. Deciding it every frame
    // would be circular: holding keeps the matrix live, which would then justify
    // continuing to hold it.
    bool menuOpen = WrMenuOpen();
    if (menuOpen && !g_menuWasOpen)
    {
        float sinceLive = (g_lastLiveTicks == 0) ? 1e9f
            : (float)((double)(nowTicks - g_lastLiveTicks) / (double)QpcFreq());
        g_holdForPanel = (sinceLive < PANEL_HOLD_GRACE);
        if (!g_holdForPanel)
            WrLogf("scan: panel opened with no live matrix in the last %.0fs -- "
                   "not holding one", PANEL_HOLD_GRACE);
    }
    else if (!menuOpen)
    {
        g_holdForPanel = false;
    }
    g_menuWasOpen = menuOpen;

    if (!selecting)
    {
        Candidate *c = &g_cand[g_chosen];
        VMatrix m;
        if (ValidateCandidate(c, &m))
        {
            c->hits++;
            c->consecutiveMisses = 0;
            chosenValidated = true;
            chosenMoved = c->changedThisFrame;

            // --- have we left the map? ---------------------------------------
            //
            // Disconnecting to the main menu does not clear this memory or make
            // it stop looking like a view matrix. It simply stops being written,
            // so the last in-game camera sits there: structurally valid, and
            // trivially past the continuity gate because it has not moved. The
            // result was the whole route still drawn over the menu, frozen.
            //
            // Byte-identical for a sustained stretch is the tell. Timed rather
            // than counted in frames so the behaviour does not change with the
            // frame rate.
            long long tick = Now();
            if (c->changedThisFrame || g_frozenSince == 0)
                g_frozenSince = tick;
            g_frozenSeconds = (float)((double)(tick - g_frozenSince) / (double)QpcFreq());

            if (g_frozenSeconds > g_frozenLimit && !g_holdForPanel)
            {
                // Standing perfectly still in a map produces an identical matrix
                // too, so this can fire while genuinely in the world. It costs a
                // paused line that returns on the first movement, which is a far
                // better failure than drawing a dead camera's route over a menu.
                if (g_matrixValid)
                    WrLogf("scan: matrix unchanged for %.1fs -- treating as "
                           "\"not in a map\" and drawing nothing", g_frozenSeconds);
                g_matrixValid = false;
                strcpy_s(g_note, sizeof(g_note), "frozen (not in a map?)");
            }
            else if (AcceptForFrame(m))
            {
                g_matrix = m;
                g_matrixValid = true;
                g_lastLiveTicks = tick;
                g_staleFrames = 0;
                g_heldFrames = 0;
                strcpy_s(g_note, sizeof(g_note), c->note);
            }
            else if (++g_heldFrames > MAX_HELD_FRAMES)
            {
                // A stray render pass lasts a frame or two. Past that, drawing
                // the world through a matrix we have decided is wrong renders as
                // lines welded to the screen while the player moves -- which is
                // the exact bug this gate exists to prevent. Draw nothing.
                g_matrixValid = false;
            }
        }
        else
        {
            c->misses++;
            c->consecutiveMisses++;
            g_matrixValid = false;
            g_staleFrames++;
        }
    }

    for (int pass = 0; pass < n; pass++)
    {
        int i;
        if (selecting)
        {
            i = pass;
        }
        else
        {
            if (pass >= REVALIDATE_PER_FRAME)
                break;
            i = (g_roundRobin + pass) % n;
            if (i == g_chosen)
                continue;           // already done above, every frame
        }

        Candidate *c = &g_cand[i];
        if (!c->alive)
            continue;

        VMatrix m;
        if (ValidateCandidate(c, &m))
        {
            c->hits++;
            c->consecutiveMisses = 0;
            if (c->changedThisFrame)
                othersMoved = true;     // something is rendering a live world
            if (selecting && i == g_chosen && AcceptForFrame(m))
            {
                g_matrix = m;
                g_matrixValid = true;
                g_staleFrames = 0;
                strcpy_s(g_note, sizeof(g_note), c->note);
            }
        }
        else
        {
            c->misses++;
            c->consecutiveMisses++;
            // A candidate that stops validating for a long stretch is either
            // freed memory or was never the matrix. Either way, drop it -- but
            // not so eagerly that a loading screen kills the real one.
            if (c->consecutiveMisses > MAX_MISSES && i != g_chosen)
                c->alive = false;
        }
    }
    if (!selecting)
        g_roundRobin = (g_roundRobin + REVALIDATE_PER_FRAME) % (n > 0 ? n : 1);

    // Retire fixed viewpoints: a candidate that has validated for a long time
    // and never moved, while some other candidate covered real ground over the
    // same stretch. The second clause matters -- a player standing still makes
    // the REAL matrix report zero travel too, and without it a quiet moment on
    // the start pad would kill the only good candidate we have.
    float maxTravel = 0.0f;
    for (int i = 0; i < n; i++)
        if (g_cand[i].alive && g_cand[i].travel > maxTravel)
            maxTravel = g_cand[i].travel;

    for (int i = 0; i < n; i++)
    {
        Candidate *c = &g_cand[i];
        if (!c->alive)
            continue;
        if (i != g_chosen && c->hits > FIXED_VIEWPOINT_HITS &&
            c->travel < MIN_TRAVEL_TO_PICK &&
            maxTravel > MIN_TRAVEL_TO_PICK * 4.0f)
            c->alive = false;
        if (c->alive)
            alive++;
    }

    if (othersMoved)
        g_othersMovedTicks = nowTicks;
    bool worldAlive =
        (g_othersMovedTicks != 0) &&
        ((float)((double)(nowTicks - g_othersMovedTicks) / (double)QpcFreq()) <
         WORLD_ALIVE_MEMORY);

    if (g_chosen >= 0)
    {
        const char *why = WrMatrixLifeTick(&g_life, tickDt, chosenValidated,
                                           chosenMoved, worldAlive);
        if (why)
        {
            WrLogf("[!] scan: matrix @ %p %s -- it did not survive the level "
                   "load. Choosing again from scratch.",
                   g_cand[g_chosen].addr, why);

            // Make every candidate earn it again. Their hit counts, updates and
            // travel were all accumulated on the PREVIOUS level, so leaving them
            // in place means the next pick is made instantly on evidence that no
            // longer applies -- which is how three duds got chosen in a second
            // and a half. Dead candidates stay dead: retiring a fixed viewpoint
            // was a correct conclusion and does not need re-deriving.
            for (int i = 0; i < n; i++)
            {
                Candidate *c = &g_cand[i];
                if (!c->alive)
                    continue;
                c->hits = c->misses = c->consecutiveMisses = 0;
                c->changes = c->jumps = 0;
                c->travel = 0.0f;
                c->haveLast = c->haveOrigin = false;
            }

            g_chosen = -1;
            g_staleFrames = 0;
            g_matrixValid = false;
            g_haveLastOrigin = false;
            g_pendingFrames = 0;
            g_heldFrames = 0;
            // Deliberately no immediate re-scan. The candidate list is almost
            // certainly still correct -- the matrix is in module data, at an
            // address that does not move -- so re-deriving it costs seconds for
            // nothing. If none of them can prove itself, the idle path above
            // starts a real scan on its own.
        }
        else
        {
            _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                        "matrix @ %p%s -- %s%s", g_cand[g_chosen].addr,
                        g_cand[g_chosen].transposed ? " (transposed)" : "",
                        g_matrixValid ? "live" : "idle (no world?)",
                        g_life.proving ? ", on probation" : "");
        }
        return;
    }

    // Pick a winner.
    //
    // Every candidate here already passes the structural oracle, so what decides
    // it is behaviour over time: the live matrix is rewritten every frame and
    // its camera origin follows the player around the map. A constant baked into
    // module data does neither, and refusing to pick one that has never updated
    // is what stops the earlier mistake from repeating.
    int best = -1;
    int updating = 0, moving = 0;

    int sw = 0, sh = 0;
    WrBackbufferSize(&sw, &sh);
    float screenAspect = (sh > 0) ? (float)sw / (float)sh : 0.0f;

    for (int i = 0; i < n; i++)
    {
        Candidate *c = &g_cand[i];
        if (!c->alive)
            continue;

        // An address the user vouched for wins outright, as soon as it has
        // proved it is still live. It does not have to demonstrate travel: the
        // whole reason for pinning one is that the automatic rules could not
        // tell it apart from its neighbours in the first place.
        if (c->pinned && c->hits >= 30 && c->changes >= 10)
        {
            best = i;
            break;
        }

        if (c->hits < MIN_HITS_TO_PICK)
            continue;
        if (c->changes < MIN_UPDATES_TO_PICK)
            continue;
        updating++;

        if (c->travel < MIN_TRAVEL_TO_PICK)
            continue;                       // a fixed viewpoint: skybox, cubemap
        if (c->jumps > (int)(c->hits * MAX_JUMP_FRACTION))
            continue;                       // a slot shared between passes
        moving++;

        if (best < 0)
        {
            best = i;
            continue;
        }

        // Among what is left, in order:
        //
        //   1. the aspect ratio closest to the actual backbuffer. The oracle
        //      allows 10% slack so an unusual render target cannot lock us out
        //      entirely, but 10% of the horizontal field of view is a visible
        //      error at the edges of the screen while being invisible at the
        //      crosshair -- so when there is a choice, take the exact one.
        //   2. fewest discontinuities.
        //   3. the most ground covered.
        //
        // Note what this deliberately does NOT claim to settle: two candidates
        // with the SAME aspect and different fields of view are both genuine
        // matrices for different passes of the same frame, and nothing
        // measurable from here says which one the world is drawn with. That is
        // what the Diagnostics list and the pin are for.
        Candidate *b = &g_cand[best];
        int ea = (screenAspect > 0.0f)
               ? (int)(fabsf(c->aspect - screenAspect) / screenAspect * 200.0f) : 0;
        int eb = (screenAspect > 0.0f)
               ? (int)(fabsf(b->aspect - screenAspect) / screenAspect * 200.0f) : 0;
        bool better = (ea != eb) ? (ea < eb)
                    : (c->jumps != b->jumps) ? (c->jumps < b->jumps)
                    : (c->travel > b->travel);
        if (better)
            best = i;
    }

    if (best >= 0)
    {
        g_chosen = best;
        g_staleFrames = 0;
        WrMatrixLifeReset(&g_life);   // a fresh pick starts with a clean clock
        strcpy_s(g_note, sizeof(g_note), g_cand[best].note);
        WrLogf("scan: using matrix @ %p%s  (%d hits, %d updates, %.0f units "
               "travelled, %d jumps, fov %.0f, aspect %.3f vs %.3f, "
               "%d other live candidate%s)%s",
               g_cand[best].addr, g_cand[best].transposed ? " (transposed)" : "",
               g_cand[best].hits, g_cand[best].changes, g_cand[best].travel,
               g_cand[best].jumps, g_cand[best].fov, g_cand[best].aspect,
               screenAspect, alive - 1, alive == 2 ? "" : "s",
               g_cand[best].pinned ? "  [remembered]" : "");
    }
    else if (alive > 0)
    {
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "watching %d candidate%s (%d update, %d follow you) -- "
                    "walk around for a second",
                    alive, alive == 1 ? "" : "s", updating, moving);
    }
}

bool WrScanResolved(void) { return g_chosen >= 0; }

bool WrScanMatrix(VMatrix *out)
{
    if (!g_matrixValid || !out)
        return false;
    *out = g_matrix;
    return true;
}

const char *WrScanStatus(void) { return g_status; }
const char *WrScanNote(void) { return g_note; }
const void *WrScanAddress(void)
{
    return g_chosen >= 0 ? (const void *)g_cand[g_chosen].addr : NULL;
}
bool WrScanTransposed(void)
{
    return g_chosen >= 0 ? g_cand[g_chosen].transposed : false;
}
bool WrScanBusy(void) { return InterlockedCompareExchange(&g_busy, 0, 0) != 0; }
int WrScanCandidateCount(void) { return g_candCount; }
int WrScanLiveCandidateCount(void)
{
    int n = 0;
    for (int i = 0; i < g_candCount; i++)
        if (g_cand[i].alive)
            n++;
    return n;
}
double WrScanMegabytes(void)
{
    return InterlockedCompareExchange64(&g_bytes, 0, 0) / (1024.0 * 1024.0);
}

float WrScanFrozenSeconds(void) { return g_frozenSeconds; }
float *WrScanFrozenLimit(void) { return &g_frozenLimit; }
bool WrScanHoldingForPanel(void) { return g_holdForPanel; }

bool WrScanCandidateAt(int i, WrScanCandidateInfo *out)
{
    if (!out || i < 0 || i >= g_candCount)
        return false;
    const Candidate *c = &g_cand[i];
    out->addr = c->addr;
    out->transposed = c->transposed;
    out->alive = c->alive;
    out->chosen = (i == g_chosen);
    out->pinned = c->pinned;
    out->hits = c->hits;
    out->changes = c->changes;
    out->jumps = c->jumps;
    out->travel = c->travel;
    out->fov = c->fov;
    out->aspect = c->aspect;
    return true;
}

void WrScanUseCandidate(int i)
{
    if (i < 0 || i >= g_candCount)
        return;
    g_cand[i].alive = true;         // an explicit choice un-retires it
    g_chosen = i;
    g_staleFrames = 0;
    g_heldFrames = 0;
    g_haveLastOrigin = false;       // the camera can be anywhere; start clean
    g_pendingFrames = 0;
    g_lastAcceptTicks = 0;
    g_frozenSince = 0;
    g_frozenSeconds = 0.0f;
    WrMatrixLifeReset(&g_life);
    strcpy_s(g_note, sizeof(g_note), g_cand[i].note);
    WrLogf("scan: matrix @ %p%s chosen by hand (fov %.0f, aspect %.3f)",
           g_cand[i].addr, g_cand[i].transposed ? " (transposed)" : "",
           g_cand[i].fov, g_cand[i].aspect);
}

bool WrScanPinChosen(void)
{
    if (g_chosen < 0)
        return false;

    char mod[64];
    unsigned long long off = 0;
    if (!AddrToModule(g_cand[g_chosen].addr, mod, sizeof(mod), &off))
    {
        WrLogf("[!] scan: matrix @ %p is not inside a loaded module, so its "
               "address is different every launch and there is nothing worth "
               "remembering", g_cand[g_chosen].addr);
        return false;
    }

    strcpy_s(g_pinModule, sizeof(g_pinModule), mod);
    g_pinOffset = off;
    g_pinTransposed = g_cand[g_chosen].transposed;
    g_pinValid = true;
    g_cand[g_chosen].pinned = true;
    _snprintf_s(g_pinDesc, sizeof(g_pinDesc), _TRUNCATE, "%s+0x%llX%s",
                g_pinModule, g_pinOffset,
                g_pinTransposed ? " (transposed)" : "");
    SavePin();
    WrLogf("scan: remembered matrix %s", g_pinDesc);
    return true;
}

void WrScanForgetPin(void)
{
    g_pinValid = false;
    g_pinModule[0] = '\0';
    g_pinOffset = 0;
    g_pinDesc[0] = '\0';
    for (int i = 0; i < g_candCount; i++)
        g_cand[i].pinned = false;
    DeleteFileA(PinPath());
    WrLogf("scan: forgot the remembered matrix");
}

const char *WrScanPinDescription(void) { return g_pinValid ? g_pinDesc : ""; }

int WrScanUpdatingCount(void)
{
    int n = 0;
    for (int i = 0; i < g_candCount; i++)
        if (g_cand[i].alive && g_cand[i].changes >= MIN_UPDATES_TO_PICK)
            n++;
    return n;
}

float WrScanTravel(void)
{
    return g_chosen >= 0 ? g_cand[g_chosen].travel : 0.0f;
}
