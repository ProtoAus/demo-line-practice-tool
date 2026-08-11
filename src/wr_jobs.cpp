// wr_jobs.cpp  --  see wr_jobs.h.

#include "wr_jobs.h"
#include "wr_log.h"

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// How many workers, and what one costs
// ---------------------------------------------------------------------------

int WrJobsWorkerCount(int requested, int logicalCores, int itemCount)
{
    int n;
    if (requested > 0)
    {
        n = requested;
    }
    else
    {
        int cores = logicalCores > 0 ? logicalCores : 2;   // os.cpu_count() or 2
        n = cores - 2;
        if (n < 1)
            n = 1;
    }
    if (itemCount > 0 && n > itemCount)
        n = itemCount;
    if (n > WR_JOBS_MAX_WORKERS)
        n = WR_JOBS_MAX_WORKERS;
    if (n < 1)
        n = 1;
    return n;
}

unsigned long long WrJobsCost(long long fileBytes)
{
    if (fileBytes < 0)
        fileBytes = 0;
    return (unsigned long long)fileBytes * WR_JOBS_COST_MULT;
}

bool WrJobsTooBig(long long fileBytes)
{
    return WrJobsCost(fileBytes) > WR_JOBS_MAX_ONE;
}

// ---------------------------------------------------------------------------
// EcoQoS
// ---------------------------------------------------------------------------
//
// BELOW_NORMAL priority is what the reference got, by inheritance, from the
// process it was launched in. It is kept here -- but priority only decides who
// runs when two threads want the same core, and on a hybrid CPU there is a
// better answer available: ask the scheduler to treat these threads as
// background, which parks them on the efficiency cores and leaves the
// performance cores for the game.
//
// SetThreadInformation is in KERNEL32, which is already imported, so the CI
// import assert is unaffected -- and it is a declared import rather than a
// GetProcAddress, which the README's list of things this DLL does not do
// specifically promises.
//
// The struct is declared here rather than by raising _WIN32_WINNT for the whole
// project, which would change what every other Windows header exposes for the
// sake of three ULONGs.
#ifndef THREAD_POWER_THROTTLING_CURRENT_VERSION
typedef struct _WR_THREAD_POWER_THROTTLING_STATE
{
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} WR_THREAD_POWER_THROTTLING_STATE;
#define WR_TPT_VERSION 1
#define WR_TPT_EXECUTION_SPEED 0x1
#else
typedef THREAD_POWER_THROTTLING_STATE WR_THREAD_POWER_THROTTLING_STATE;
#define WR_TPT_VERSION THREAD_POWER_THROTTLING_CURRENT_VERSION
#define WR_TPT_EXECUTION_SPEED THREAD_POWER_THROTTLING_EXECUTION_SPEED
#endif

static void StandAside(void)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    WR_THREAD_POWER_THROTTLING_STATE st;
    memset(&st, 0, sizeof(st));
    st.Version = WR_TPT_VERSION;
    st.ControlMask = WR_TPT_EXECUTION_SPEED;
    st.StateMask = WR_TPT_EXECUTION_SPEED;
    // Failure is fine and needs no branch: on a version of Windows without
    // EcoQoS this returns FALSE and the thread is merely below-normal, which
    // is exactly what the reference had.
    SetThreadInformation(GetCurrentThread(), (THREAD_INFORMATION_CLASS)3,
                         &st, sizeof(st));
}

// ---------------------------------------------------------------------------
// The guard
// ---------------------------------------------------------------------------

unsigned long WrJobsGuard(void (*fn)(void *user, int index), void *user,
                          int index)
{
    __try
    {
        fn(user, index);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return (unsigned long)GetExceptionCode();
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The pool
// ---------------------------------------------------------------------------

struct Pool
{
    const WrJobsConfig *cfg;
    WrJobsAbort abort;
    void *abortUser;

    volatile LONG next;                 // the next item to claim

    CRITICAL_SECTION cs;
    CONDITION_VARIABLE room;            // admission
    unsigned long long budget;
    unsigned long long inFlight;
    int inFlightCount;

    int *finished;                      // completion queue, capacity itemCount
    int head, tail;
};

static bool Aborted(Pool *p)
{
    return p->abort && p->abort(p->abortUser) != 0;
}

// Wait for room. Returns false only when the run is being cancelled.
//
// THE GUARD IS `inFlightCount > 0`. Without it, a demo whose cost exceeds the
// whole budget would wait forever for room that can never appear, and it would
// do it holding a worker. With it, the worst case is that the biggest demo in
// the library runs on its own, which is slow and correct rather than fast and
// hung.
static bool Admit(Pool *p, unsigned long long cost)
{
    EnterCriticalSection(&p->cs);
    for (;;)
    {
        if (p->inFlightCount == 0 || p->inFlight + cost <= p->budget)
        {
            p->inFlight += cost;
            p->inFlightCount++;
            LeaveCriticalSection(&p->cs);
            return true;
        }
        // A timeout rather than a pure wait, so the cancel predicate is checked
        // by a worker that is asleep on the budget rather than on the work.
        SleepConditionVariableCS(&p->room, &p->cs, 50);
        if (Aborted(p))
        {
            LeaveCriticalSection(&p->cs);
            return false;
        }
    }
}

static void Release(Pool *p, unsigned long long cost)
{
    EnterCriticalSection(&p->cs);
    p->inFlight -= cost;
    p->inFlightCount--;
    LeaveCriticalSection(&p->cs);
    WakeAllConditionVariable(&p->room);
}

static DWORD WINAPI Worker(LPVOID param)
{
    Pool *p = (Pool *)param;
    StandAside();

    for (;;)
    {
        if (Aborted(p))
            break;
        const int i = (int)InterlockedIncrement(&p->next) - 1;
        if (i >= p->cfg->itemCount)
            break;

        const unsigned long long cost =
            p->cfg->cost ? p->cfg->cost(p->cfg->user, i) : 0;
        if (!Admit(p, cost))
            break;

        const unsigned long code = WrJobsGuard(p->cfg->run, p->cfg->user, i);
        Release(p, cost);

        if (code)
        {
            // Loudly, every time. See WrJobsGuard in the header: this is a bug
            // to fix, not a condition to live with, and a quiet recovery is how
            // it would go unfixed.
            WrLogf("[!] extract: worker faulted on item %d (0x%08lX) -- the "
                   "heap may not be in a good state; please report this",
                   i, code);
        }

        EnterCriticalSection(&p->cs);
        p->finished[p->tail++] = i;
        LeaveCriticalSection(&p->cs);
    }
    return 0;
}

// Called only on the coordinator's thread.
static int Drain(Pool *p)
{
    int n = 0;
    for (;;)
    {
        int i;
        EnterCriticalSection(&p->cs);
        const bool any = p->head < p->tail;
        i = any ? p->finished[p->head++] : -1;
        LeaveCriticalSection(&p->cs);
        if (!any)
            break;
        p->cfg->done(p->cfg->user, i);
        n++;
    }
    return n;
}

int WrJobsRunAll(const WrJobsConfig *cfg, WrJobsAbort abort, void *abortUser,
                 int *workersUsed)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const int workers = WrJobsWorkerCount(cfg->requestedWorkers,
                                          (int)si.dwNumberOfProcessors,
                                          cfg->itemCount);
    if (workersUsed)
        *workersUsed = workers;

    // The reference's own `jobs <= 1 or len(targets) < 2`. Submission order,
    // no thread, no line -- and the only mode in which two implementations can
    // be compared line for line.
    if (workers <= 1 || cfg->itemCount < 2)
    {
        int n = 0;
        for (int i = 0; i < cfg->itemCount; i++)
        {
            if (abort && abort(abortUser))
                break;
            const unsigned long code = WrJobsGuard(cfg->run, cfg->user, i);
            if (code)
                WrLogf("[!] extract: faulted on item %d (0x%08lX) -- the heap "
                       "may not be in a good state; please report this",
                       i, code);
            cfg->done(cfg->user, i);
            n++;
        }
        return n;
    }

    Pool p;
    memset(&p, 0, sizeof(p));
    p.cfg = cfg;
    p.abort = abort;
    p.abortUser = abortUser;
    p.budget = cfg->budgetBytes ? cfg->budgetBytes : WR_JOBS_BUDGET_BYTES;
    p.finished = (int *)malloc(sizeof(int) * (size_t)cfg->itemCount);
    if (!p.finished)
        return 0;
    InitializeCriticalSection(&p.cs);
    InitializeConditionVariable(&p.room);

    HANDLE threads[WR_JOBS_MAX_WORKERS];
    int started = 0;
    for (int i = 0; i < workers; i++)
    {
        threads[started] = CreateThread(NULL, 0, Worker, &p, 0, NULL);
        if (threads[started])
            started++;
    }
    if (started == 0)
    {
        // Every CreateThread refused. Do the work here rather than report
        // nothing: a machine that cannot start a thread can still extract.
        DeleteCriticalSection(&p.cs);
        free(p.finished);
        WrJobsConfig serial = *cfg;
        serial.requestedWorkers = 1;
        return WrJobsRunAll(&serial, abort, abortUser, workersUsed);
    }

    int consumed = 0;
    ULONGLONG cancelAt = 0;
    bool warned = false;
    for (;;)
    {
        consumed += Drain(&p);

        const DWORD r = WaitForMultipleObjects((DWORD)started, threads, TRUE, 20);
        if (r == WAIT_OBJECT_0)
        {
            consumed += Drain(&p);
            break;
        }

        // The watchdog. Cooperative cancellation has no upper bound that comes
        // from the data, so if it has not landed in ten seconds that is worth
        // saying out loud rather than sitting on.
        if (abort && abort(abortUser))
        {
            if (!cancelAt)
                cancelAt = GetTickCount64();
            else if (!warned && GetTickCount64() - cancelAt > 10000)
            {
                warned = true;
                WrLogf("extract: stop was asked for 10 s ago and %d worker%s "
                       "still running; each one finishes its current step "
                       "before it can look",
                       started, started == 1 ? " is" : "s are");
            }
        }
    }

    for (int i = 0; i < started; i++)
        CloseHandle(threads[i]);
    DeleteCriticalSection(&p.cs);
    free(p.finished);
    return consumed;
}
