// wr_player.cpp  --  see wr_player.h for what this is and why it is allowed to
//                    exist. This file is the mechanism.
//
// THE SHAPE OF IT
//
// A background thread sweeps writable committed memory looking for float
// triples that could be the player origin THIS INSTANT, and hands the addresses
// to the render thread. The render thread then watches them: every frame each
// surviving candidate has to predict the camera again, and one that fails
// twice is dropped. A candidate that has survived long enough to be an accident
// is promoted.
//
// The sweep is cheap in a way the matrix sweep could not be, and the reason is
// worth naming: this search has a live reference signal. wr_scan.cpp had to
// decide whether sixteen floats were a projection matrix using only their own
// contents. Here the answer is already on screen -- the camera solve gives two
// coordinates that the thing being looked for must equal -- so the filter is
// a comparison against a moving target rather than a structural guess, and
// unrelated memory does not survive it even once.
//
// WHY THE SWEEP RE-READS THE CAMERA PER CHUNK
//
// A surfer covers 30-odd units a frame and a full sweep is not instant, so a
// position captured when the sweep began is stale within milliseconds. The
// camera is therefore re-solved before every chunk. A chunk is a quarter of a
// megabyte and takes microseconds to read, over which the player moves well
// under a unit -- so within one chunk the reference is effectively exact, and
// across chunks it simply moves along with the player.
//
// THREADING
//
// The scan thread reads g_camShared, which the render thread writes. That is a
// benign race by design: a torn read produces a position that matches nothing,
// which costs one missed candidate in one chunk of one sweep. Making it a lock
// would put the render thread behind a memory sweep, which is the one thing
// that must never happen here. Everything the scan thread PUBLISHES goes
// through the critical section, because a half-written address is not benign.

#include "wr_player.h"
#include "wr_engine.h"
#include "wr_energy.h"
#include "wr_log.h"

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Tuning, all of it stated rather than buried
// ---------------------------------------------------------------------------

// How far a candidate's x and y may sit from the camera's and still be believed
// to be the same point.
//
// In Source the view offset is vertical, so in principle these agree exactly.
// In practice two things blur it and neither is worth being brittle about: the
// camera origin is solved from a float32 matrix by a 3x3 inverse rather than
// read, and a game is free to move the eye laterally for bob or a lean. Six
// units is far wider than either and still a needle: an unrelated float has to
// land inside a twelve-unit window around a number that moves thirty units
// every frame, and then keep doing it. The per-frame watcher is what proves a
// candidate; this only has to be tight enough to keep the list short.
#define P_XY_SLACK 6.0f

// The view offset the third float has to sit inside. Source stands at 64 and
// ducks to 28; the bounds are wide enough that no game-specific value is being
// assumed, and narrow enough to exclude both the camera itself (offset 0) and
// anything a floor below.
#define P_EYE_MIN 8.0f
#define P_EYE_MAX 80.0f

// Frames of continuous agreement before a candidate is believed. At 300 fps
// this is under a second, over which a surfing player has moved thousands of
// units through several turns -- there is no such thing as passing this by
// coincidence.
#define P_PROVE_FRAMES 90

// ...and how many consecutive failures retire one. Two, not one: a single frame
// where the camera solve is momentarily poor should not cost the answer.
#define P_FORGIVE 2

#define P_MAX_CAND 96
#define P_SCAN_CHUNK (256 * 1024)
#define P_TRIPLE 12                 // three floats
#define P_BUDGET_MB 1536

// The velocity oracle's tolerance, taken from the extractor's OriginScore so
// that "this float triple is that velocity" means the same thing in both
// places: 25 u/s, or 5% of the figure, whichever is larger.
#define P_VEL_ABS 25.0f
#define P_VEL_FRAC 0.05f

// A velocity candidate has to be tested against motion big enough to mean
// something. Standing still, every triple of zeros predicts the origin
// perfectly.
#define P_VEL_MIN_SPEED 200.0f

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct Cand
{
    const unsigned char *addr;
    int hits;
    int misses;
};

static CRITICAL_SECTION g_cs;
static bool g_csReady = false;

static Cand g_org[P_MAX_CAND];
static int  g_orgCount = 0;
static Cand g_vel[P_MAX_CAND];
static int  g_velCount = 0;

static const unsigned char *g_orgAddr = NULL;   // proved, or NULL
static const unsigned char *g_velAddr = NULL;

static Vec3 g_origin, g_velocity;
static bool g_haveOrigin = false, g_haveVelocity = false;
static float g_eyeHeight = -1.0f;

// What the scan thread reads. See the threading note at the top.
static volatile float g_camShared[3] = { 0.0f, 0.0f, 0.0f };
static volatile LONG g_camLive = 0;

static HANDLE g_thread = NULL;
static volatile LONG g_stop = 0;
static volatile LONG g_busy = 0;
static volatile LONG g_scanKind = 0;    // 0 = origin, 1 = velocity

// Seconds before another sweep may start. A sweep that found nothing must not
// immediately start again -- that would be a permanent background thread
// walking a gigabyte for the rest of the session. This is the throttle, and it
// is a plain timer rather than a request flag because a flag has to be armed
// from somewhere, and every place that could arm it is a place that can forget.
#define P_SCAN_COOLDOWN 4.0f
static float g_cooldown = 0.0f;

// ...and how many fruitless sweeps before it stops looking.
//
// A sweep reads up to P_BUDGET_MB of address space. Repeating that every few
// seconds for the rest of a session, on a machine where the search is never
// going to succeed, is hundreds of megabytes a second of background reads
// bought for nothing -- and this feature is an improvement to a number, not a
// requirement. Five attempts, then it stays quiet until the map changes or
// somebody asks again from the panel.
#define P_MAX_TRIES 5
static int g_tries[2] = { 0, 0 };

static char g_status[192] = "not started";

// The origin's own motion, for the velocity oracle. Kept here rather than asked
// of wr_energy.cpp because that one is smoothed and windowed, and what this
// needs is the rawest difference available.
static Vec3 g_prevOrigin;
static bool g_havePrev = false;
static Vec3 g_orgDelta;
static float g_orgDeltaDt = 0.0f;

// ---------------------------------------------------------------------------

static bool Finite(float f)
{
    return !(f != f) && f < 3.0e38f && f > -3.0e38f;
}

static size_t ReadChunk(const unsigned char *addr, unsigned char *buf, size_t want)
{
    SIZE_T got = 0;
    if (ReadProcessMemory(GetCurrentProcess(), addr, buf, want, &got) && got == want)
        return want;

    // One bad page inside the chunk should not cost the whole chunk -- the same
    // reasoning, and the same shape, as wr_scan.cpp's reader.
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

static bool ReadTriple(const unsigned char *addr, float out[3])
{
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), addr, out, P_TRIPLE, &got) ||
        got != P_TRIPLE)
        return false;
    return Finite(out[0]) && Finite(out[1]) && Finite(out[2]);
}

// Could this triple be the player origin, given where the camera is right now?
static bool LooksLikeOrigin(const float p[3], const float cam[3])
{
    const float dx = p[0] - cam[0];
    const float dy = p[1] - cam[1];
    if (dx > P_XY_SLACK || dx < -P_XY_SLACK) return false;
    if (dy > P_XY_SLACK || dy < -P_XY_SLACK) return false;
    const float h = cam[2] - p[2];
    return h >= P_EYE_MIN && h <= P_EYE_MAX;
}

// Could this triple be the velocity that produced `delta` over `dt`?
static bool LooksLikeVelocity(const float v[3], const Vec3 &delta, float dt)
{
    if (!(dt > 1e-6f))
        return false;
    const float want[3] = { delta.x / dt, delta.y / dt, delta.z / dt };
    for (int i = 0; i < 3; i++)
    {
        float tol = want[i] < 0.0f ? -want[i] : want[i];
        tol *= P_VEL_FRAC;
        if (tol < P_VEL_ABS) tol = P_VEL_ABS;
        float d = v[i] - want[i];
        if (d < 0.0f) d = -d;
        if (d > tol)
            return false;
    }
    return true;
}

static void AddCand(Cand *list, int *count, const unsigned char *addr)
{
    if (*count >= P_MAX_CAND)
        return;
    for (int i = 0; i < *count; i++)
        if (list[i].addr == addr)
            return;
    list[*count].addr = addr;
    list[*count].hits = 0;
    list[*count].misses = 0;
    (*count)++;
}

// ---------------------------------------------------------------------------
// The sweep
// ---------------------------------------------------------------------------

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

// A snapshot of what the render thread last published, taken whole so the three
// components at least came from one assignment in the common case.
static bool CamNow(float out[3])
{
    if (InterlockedCompareExchange(&g_camLive, 0, 0) == 0)
        return false;
    out[0] = g_camShared[0];
    out[1] = g_camShared[1];
    out[2] = g_camShared[2];
    return Finite(out[0]) && Finite(out[1]) && Finite(out[2]);
}

static void SweepRange(const unsigned char *base, size_t size, int kind,
                       const Vec3 &delta, float dt)
{
    static unsigned char buf[P_SCAN_CHUNK + P_TRIPLE];

    size_t off = 0;
    while (off < size && !g_stop)
    {
        size_t want = size - off;
        if (want > P_SCAN_CHUNK + P_TRIPLE)
            want = P_SCAN_CHUNK + P_TRIPLE;

        // The reference, refreshed for this chunk. See the header note: a chunk
        // is read in microseconds, so within one the player has not moved.
        float cam[3];
        if (kind == 0 && !CamNow(cam))
            return;

        // What the FIRST component has to be near, so the inner loop can reject
        // on one comparison. This is the whole cost of the sweep: everything
        // else runs on the handful of positions that get past it.
        float want0 = 0.0f, tol0 = 0.0f;
        if (kind == 0)
        {
            want0 = cam[0];
            tol0 = P_XY_SLACK;
        }
        else
        {
            if (!(dt > 1e-6f))
                return;
            want0 = delta.x / dt;
            tol0 = (want0 < 0.0f ? -want0 : want0) * P_VEL_FRAC;
            if (tol0 < P_VEL_ABS) tol0 = P_VEL_ABS;
        }

        const size_t got = ReadChunk(base + off, buf, want);
        if (got >= P_TRIPLE)
        {
            const size_t last = got - P_TRIPLE;
            // Four-byte stride: a float in a struct is aligned, and stepping by
            // one byte would multiply the work by four to find triples that
            // straddle a float boundary and cannot be real.
            for (size_t i = 0; i <= last && !g_stop; i += 4)
            {
                float x;
                memcpy(&x, buf + i, sizeof(x));
                const float d0 = x - want0;
                if (d0 > tol0 || d0 < -tol0)
                    continue;

                float p[3];
                memcpy(p, buf + i, P_TRIPLE);
                if (!Finite(p[0]) || !Finite(p[1]) || !Finite(p[2]))
                    continue;

                const bool hit = (kind == 0) ? LooksLikeOrigin(p, cam)
                                             : LooksLikeVelocity(p, delta, dt);
                if (!hit)
                    continue;

                EnterCriticalSection(&g_cs);
                if (kind == 0) AddCand(g_org, &g_orgCount, base + off + i);
                else           AddCand(g_vel, &g_velCount, base + off + i);
                const int n = (kind == 0) ? g_orgCount : g_velCount;
                LeaveCriticalSection(&g_cs);
                if (n >= P_MAX_CAND)
                    return;
            }
        }
        off += P_SCAN_CHUNK;
    }
}

static DWORD WINAPI ScanThread(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    InterlockedExchange(&g_busy, 1);

    const int kind = (int)InterlockedCompareExchange(&g_scanKind, 0, 0);

    Vec3 delta = { 0.0f, 0.0f, 0.0f };
    float dt = 0.0f;
    if (kind == 1)
    {
        EnterCriticalSection(&g_cs);
        delta = g_orgDelta;
        dt = g_orgDeltaDt;
        LeaveCriticalSection(&g_cs);
    }

    LONG64 bytes = 0;
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

        if (WritableCommitted(mbi))
        {
            SweepRange((const unsigned char *)mbi.BaseAddress, mbi.RegionSize,
                       kind, delta, dt);
            bytes += (LONG64)mbi.RegionSize;
        }
        if (bytes > (LONG64)P_BUDGET_MB * 1024 * 1024)
            break;
        p = next;
    }

    EnterCriticalSection(&g_cs);
    const int n = (kind == 0) ? g_orgCount : g_velCount;
    LeaveCriticalSection(&g_cs);
    WrLogf("player: %s sweep found %d candidate%s in %.0f MB",
           kind == 0 ? "origin" : "velocity", n, n == 1 ? "" : "s",
           bytes / (1024.0 * 1024.0));

    InterlockedExchange(&g_busy, 0);
    return 0;
}

static void StartScan(int kind)
{
    if (InterlockedCompareExchange(&g_busy, 0, 0) != 0)
        return;
    if (g_thread)
    {
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    InterlockedExchange(&g_stop, 0);
    InterlockedExchange(&g_scanKind, kind);
    g_thread = CreateThread(NULL, 0, ScanThread, NULL, 0, NULL);
}

// ---------------------------------------------------------------------------
// Per frame
// ---------------------------------------------------------------------------

static void Forget(void)
{
    EnterCriticalSection(&g_cs);
    g_orgCount = g_velCount = 0;
    g_orgAddr = g_velAddr = NULL;
    LeaveCriticalSection(&g_cs);
    g_haveOrigin = g_haveVelocity = false;
    g_eyeHeight = -1.0f;
    g_havePrev = false;
    g_orgDeltaDt = 0.0f;
}

// Watch the candidates for one frame and promote or retire them.
//
// `hits` counts frames in a row, not frames total, because a stale pointer into
// a freed object can go on holding believable values for a while and then stop.
// Retiring on P_FORGIVE consecutive misses rather than on the first is the
// other half of the same judgement: the camera solve occasionally has a poor
// frame, and one poor frame should not cost an answer proved over ninety.
static const unsigned char *Watch(Cand *list, int *count, int kind,
                                  const float cam[3], const Vec3 &delta,
                                  float dt, Vec3 *valueOut)
{
    const unsigned char *won = NULL;
    int keep = 0;
    for (int i = 0; i < *count; i++)
    {
        float v[3];
        bool ok = ReadTriple(list[i].addr, v);
        if (ok)
            ok = (kind == 0) ? LooksLikeOrigin(v, cam)
                             : LooksLikeVelocity(v, delta, dt);

        if (ok)
        {
            list[i].hits++;
            list[i].misses = 0;
            if (!won && list[i].hits >= P_PROVE_FRAMES)
            {
                won = list[i].addr;
                if (valueOut) *valueOut = WrVec(v[0], v[1], v[2]);
            }
        }
        else
        {
            list[i].misses++;
            if (list[i].misses >= P_FORGIVE)
                continue;               // dropped
        }
        list[keep++] = list[i];
    }
    *count = keep;
    return won;
}

void WrPlayerTick(const Vec3 &cam, float dt)
{
    if (!g_csReady)
    {
        InitializeCriticalSection(&g_cs);
        g_csReady = true;
    }

    const float c[3] = { cam.x, cam.y, cam.z };
    if (!Finite(c[0]) || !Finite(c[1]) || !Finite(c[2]))
        return;

    g_camShared[0] = c[0];
    g_camShared[1] = c[1];
    g_camShared[2] = c[2];
    InterlockedExchange(&g_camLive, 1);

    // ---- the origin -------------------------------------------------------
    if (g_orgAddr)
    {
        float v[3];
        if (ReadTriple(g_orgAddr, v) && LooksLikeOrigin(v, c))
        {
            g_origin = WrVec(v[0], v[1], v[2]);
            g_haveOrigin = true;
            g_eyeHeight = c[2] - v[2];
        }
        else
        {
            // It died -- a level change, an entity respawn, a heap move. That
            // is expected, not exceptional, and the answer is to look again
            // rather than to go on reading a pointer that has stopped meaning
            // anything.
            WrLogf("player: the origin at %p stopped tracking the camera; "
                   "looking again", g_orgAddr);
            Forget();
        }
    }

    EnterCriticalSection(&g_cs);
    if (!g_orgAddr && g_orgCount > 0)
    {
        Vec3 val;
        const unsigned char *won = Watch(g_org, &g_orgCount, 0, c,
                                         WrVec(0, 0, 0), 0.0f, &val);
        if (won)
        {
            g_orgAddr = won;
            g_origin = val;
            g_haveOrigin = true;
            g_eyeHeight = c[2] - val.z;
            // The rest of the list has done its job. Leaving it would keep
            // ninety-odd ReadProcessMemory calls a frame running for nothing,
            // and would also make "origin candidates" a number that never went
            // back down.
            g_orgCount = 0;
            WrLogf("player: origin proved at %p over %d frames -- eye height "
                   "reads %.1f", won, P_PROVE_FRAMES, g_eyeHeight);
        }
    }
    LeaveCriticalSection(&g_cs);

    // ---- the origin's own motion, which is the velocity oracle ------------
    if (g_haveOrigin)
    {
        if (g_havePrev && dt > 1e-6f)
        {
            const Vec3 d = WrVec(g_origin.x - g_prevOrigin.x,
                                 g_origin.y - g_prevOrigin.y,
                                 g_origin.z - g_prevOrigin.z);
            EnterCriticalSection(&g_cs);
            g_orgDelta = d;
            g_orgDeltaDt = dt;
            LeaveCriticalSection(&g_cs);
        }
        g_prevOrigin = g_origin;
        g_havePrev = true;
    }
    else
    {
        g_havePrev = false;
    }

    // ---- the velocity -----------------------------------------------------
    Vec3 delta;
    float ddt;
    EnterCriticalSection(&g_cs);
    delta = g_orgDelta;
    ddt = g_orgDeltaDt;
    LeaveCriticalSection(&g_cs);

    const float moved = (ddt > 1e-6f)
        ? WrLength(WrVec(delta.x / ddt, delta.y / ddt, delta.z / ddt)) : 0.0f;
    const bool velTestable = (moved >= P_VEL_MIN_SPEED);

    if (g_velAddr)
    {
        float v[3];
        if (!ReadTriple(g_velAddr, v))
        {
            g_velAddr = NULL;
            g_haveVelocity = false;
        }
        else if (velTestable && !LooksLikeVelocity(v, delta, ddt))
        {
            WrLogf("player: the velocity at %p stopped predicting the origin; "
                   "looking again", g_velAddr);
            g_velAddr = NULL;
            g_haveVelocity = false;
            EnterCriticalSection(&g_cs);
            g_velCount = 0;
            LeaveCriticalSection(&g_cs);
        }
        else
        {
            g_velocity = WrVec(v[0], v[1], v[2]);
            g_haveVelocity = true;
        }
    }
    else if (velTestable)
    {
        EnterCriticalSection(&g_cs);
        if (g_velCount > 0)
        {
            Vec3 val;
            const unsigned char *won = Watch(g_vel, &g_velCount, 1, c, delta,
                                             ddt, &val);
            if (won)
            {
                g_velAddr = won;
                g_velocity = val;
                g_haveVelocity = true;
                g_velCount = 0;
                WrLogf("player: velocity proved at %p over %d frames", won,
                       P_PROVE_FRAMES);
            }
        }
        LeaveCriticalSection(&g_cs);
    }

    // ---- do we need to go looking? ----------------------------------------
    //
    // One sweep at a time, origin first. The velocity sweep cannot run before
    // the origin is proved, because the origin's motion IS the velocity's
    // oracle -- searching for it first would be searching without a test.
    if (g_cooldown > 0.0f)
        g_cooldown -= dt;

    if (InterlockedCompareExchange(&g_busy, 0, 0) == 0 && g_cooldown <= 0.0f)
    {
        int wantKind = -1;
        EnterCriticalSection(&g_cs);
        if (!g_orgAddr && g_orgCount == 0 && g_tries[0] < P_MAX_TRIES)
            wantKind = 0;
        else if (g_orgAddr && !g_velAddr && g_velCount == 0 && velTestable &&
                 g_tries[1] < P_MAX_TRIES)
            wantKind = 1;
        LeaveCriticalSection(&g_cs);

        if (wantKind >= 0)
        {
            g_tries[wantKind]++;
            if (g_tries[wantKind] == P_MAX_TRIES)
                WrLogf("player: %s -- last attempt of %d; if this one finds "
                       "nothing it will stop looking until the map changes",
                       wantKind == 0 ? "origin" : "velocity", P_MAX_TRIES);
            StartScan(wantKind);
            g_cooldown = P_SCAN_COOLDOWN;
        }
    }

    // ---- status -----------------------------------------------------------
    EnterCriticalSection(&g_cs);
    const int no = g_orgCount, nv = g_velCount;
    LeaveCriticalSection(&g_cs);

    if (g_haveOrigin && g_haveVelocity)
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "origin and velocity read from the game (eye %.1f)",
                    g_eyeHeight);
    else if (g_haveOrigin)
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "origin read (eye %.1f); %d velocity candidate%s",
                    g_eyeHeight, nv, nv == 1 ? "" : "s");
    else if (InterlockedCompareExchange(&g_busy, 0, 0) != 0)
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE, "looking...");
    else if (no > 0)
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "watching %d origin candidate%s", no, no == 1 ? "" : "s");
    else if (g_tries[0] >= P_MAX_TRIES)
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "not found in %d sweeps -- using the camera estimate",
                    P_MAX_TRIES);
    else
        _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                    "nothing found yet -- using the camera estimate");
}

void WrPlayerOnMapChanged(void)
{
    if (!g_csReady)
        return;
    // Ask the sweep to stop and leave the flag SET. StartScan clears it before
    // it launches, and it refuses to launch while the old thread is still
    // running -- so the flag stays raised for exactly as long as there is a
    // thread that ought to see it, which is the whole of its job.
    InterlockedExchange(&g_stop, 1);
    Forget();
    g_cooldown = 0.0f;
    g_tries[0] = g_tries[1] = 0;
}

void WrPlayerRescan(void) { WrPlayerOnMapChanged(); }

bool WrPlayerOrigin(Vec3 *out)
{
    if (!g_haveOrigin)
        return false;
    if (out) *out = g_origin;
    return true;
}

bool WrPlayerVelocity(Vec3 *out)
{
    if (!g_haveVelocity)
        return false;
    if (out) *out = g_velocity;
    return true;
}

float WrPlayerEyeHeight(void) { return g_haveOrigin ? g_eyeHeight : -1.0f; }
const char *WrPlayerStatus(void) { return g_status; }
bool WrPlayerBusy(void) { return InterlockedCompareExchange(&g_busy, 0, 0) != 0; }
int WrPlayerCandidates(void) { return g_orgCount; }
int WrPlayerVelCandidates(void) { return g_velCount; }
