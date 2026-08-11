// test_jobs.cpp  --  the worker pool, and the four things about it that are
// not obvious.
//
// Most of this is WrJobsWorkerCount as a table, which needs no threads at all:
// it is a pure function of three integers precisely so that the interesting
// arithmetic -- cores minus two, capped at the item count, capped again at
// sixteen -- can be checked without any of the machinery it feeds.
//
// The rest genuinely starts threads, because what it checks is a property OF
// the threading and cannot be restated without it:
//
//   EVERY ITEM RUNS EXACTLY ONCE. The claim is atomic, and the failure mode of
//   getting it wrong is a demo silently skipped rather than a crash.
//
//   RESULTS COME BACK IN COMPLETION ORDER, ON THE CALLER'S THREAD. That is
//   submit + as_completed and not pool.map, and the difference is the reason
//   this exists: the reference once had a docstring saying "in whatever order
//   they finish" over code that yielded in SUBMISSION order, so one slow demo
//   held back the progress line of every finished demo behind it and the panel
//   went silent for as long as that demo took. The reported symptom was the
//   extractor looking hung. It was not hung, it was mute.
//
//   TWO CONCURRENT JOBS DO NOT SHARE A DEADLINE. The reference's is a module
//   global, and each of its workers is a separate process, so each gets its own
//   for free. Transcribed as a static here it would silently give N threads one
//   clock. There is no shared deadline to test directly -- it lives in a stack
//   frame -- so what is checked is that two pools running at once each see
//   their own, which is the observable consequence.
//
//   ADMISSION CANNOT DEADLOCK. A worker always proceeds if nothing else is in
//   flight, so an item larger than the entire budget runs on its own rather
//   than waiting for room that can never appear. Getting this wrong is a hang,
//   not a wrong answer, which is why it is worth a section of its own.
//
// Build:  tests\build.bat
// Run:    tests\test_jobs.exe

#include "wr_jobs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

static void Check(bool ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        g_failures++;
}

// ---------------------------------------------------------------------------

struct Counter
{
    volatile LONG ran[512];
    int done[512];
    int doneN;
    volatile LONG concurrent;
    volatile LONG peak;
    unsigned long long sizes[512];
    int slowItem;
};

static void CountRun(void *user, int index)
{
    Counter *c = (Counter *)user;
    InterlockedIncrement(&c->ran[index]);

    const LONG now = InterlockedIncrement(&c->concurrent);
    for (;;)
    {
        const LONG was = c->peak;
        if (now <= was || InterlockedCompareExchange(&c->peak, now, was) == was)
            break;
    }
    // One item takes noticeably longer, so completion order can differ from
    // submission order and be seen to. Sleep(0) for the rest and not Sleep(1):
    // the scheduler's tick is around fifteen milliseconds, so sixty-four
    // one-millisecond sleeps are a whole second and the "slow" item would
    // finish first.
    Sleep(index == c->slowItem ? 250 : 0);
    InterlockedDecrement(&c->concurrent);
}

static void CountDone(void *user, int index)
{
    Counter *c = (Counter *)user;
    c->done[c->doneN++] = index;        // the caller's thread; no lock needed
}

static unsigned long long CountCost(void *user, int index)
{
    return ((Counter *)user)->sizes[index];
}

// ---------------------------------------------------------------------------

struct DeadlineJob
{
    int ms;                             // this job's own budget
    volatile LONG overran;              // saw a deadline that was not its own
    ULONGLONG start;
};

static void DeadlineRun(void *user, int index)
{
    DeadlineJob *j = (DeadlineJob *)user;
    // Each item computes a deadline from ITS OWN job, in its own stack frame,
    // which is the shape wr_extract.cpp's ExtractRunOne uses.
    const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)j->ms;
    Sleep(20);
    if (GetTickCount64() > deadline)
        InterlockedIncrement(&j->overran);
    (void)index;
}

static void DeadlineDone(void *, int) {}

static DWORD WINAPI DeadlineThread(LPVOID p)
{
    DeadlineJob *j = (DeadlineJob *)p;
    WrJobsConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.itemCount = 8;
    cfg.requestedWorkers = 4;
    cfg.run = DeadlineRun;
    cfg.done = DeadlineDone;
    cfg.user = j;
    WrJobsRunAll(&cfg, NULL, NULL, NULL);
    return 0;
}

// ---------------------------------------------------------------------------

static int g_cancelAfter = 0;
static volatile LONG g_started = 0;

static int CancelAbort(void *)
{
    return g_started >= g_cancelAfter ? 1 : 0;
}

static void CancelRun(void *, int)
{
    InterlockedIncrement(&g_started);
    Sleep(5);
}

static void CancelDone(void *user, int)
{
    (*(int *)user)++;
}

int main(void)
{
    printf("\n=== wrlines job pool ===\n");

    // -----------------------------------------------------------------------
    printf("\nhow many workers, as a table\n");
    {
        struct Row { int requested, cores, items, want; const char *why; };
        static const Row rows[] = {
            {0,  8, 100, 6,  "eight cores, plenty of work: cores - 2"},
            {0,  4, 100, 2,  "four cores: still cores - 2"},
            {0,  2, 100, 1,  "two cores: one, not zero"},
            {0,  1, 100, 1,  "one core: one"},
            {0,  0, 100, 1,  "cores unknown reads as two, so one"},
            {0, 64, 100, 16, "sixty-four cores are capped at sixteen"},
            {0, 32,   5, 5,  "and never more workers than there are items"},
            {4,  8, 100, 4,  "--jobs 4 is taken as given"},
            {1, 32, 100, 1,  "--jobs 1 is one, whatever the machine has"},
            {99, 8, 100, 16, "--jobs 99 still meets the cap"},
            {8,  8,   3, 3,  "--jobs 8 with three demos is three"},
            {0,  8,   0, 6,  "no items: the item clamp does not apply"},
            {0,  8,   1, 1,  "one item is one worker, which runs serially"},
        };
        bool allRight = true;
        for (int i = 0; i < (int)(sizeof(rows) / sizeof(rows[0])); i++)
        {
            const int got = WrJobsWorkerCount(rows[i].requested, rows[i].cores,
                                              rows[i].items);
            if (got != rows[i].want)
            {
                printf("     %-52s %d, wanted %d\n", rows[i].why, got,
                       rows[i].want);
                allRight = false;
            }
        }
        Check(allRight, "every row of the worker-count table");

        // Idempotent, because WrJobsRunAll recomputes it from the number the
        // caller already printed.
        const int once = WrJobsWorkerCount(0, 12, 500);
        Check(WrJobsWorkerCount(once, 12, 500) == once,
              "feeding the answer back in gives the same answer");
    }

    // -----------------------------------------------------------------------
    printf("\nwhat one demo costs, and when it is refused outright\n");
    {
        Check(WrJobsCost(1000) == 1000ull * WR_JOBS_COST_MULT,
              "cost is the file size times the measured multiplier");
        Check(WrJobsCost(-1) == 0, "a size that could not be read costs nothing");
        Check(!WrJobsTooBig(48ull * 1024 * 1024),
              "the largest demo in this library is admissible");
        Check(WrJobsTooBig((long long)(WR_JOBS_MAX_ONE / WR_JOBS_COST_MULT) + 1),
              "and one over the single-item cap is not");
    }

    // -----------------------------------------------------------------------
    printf("\nevery item runs once, and results arrive as they finish\n");
    {
        Counter c;
        memset(&c, 0, sizeof(c));
        c.slowItem = 0;                 // the FIRST one is the slow one
        for (int i = 0; i < 64; i++)
            c.sizes[i] = 1024;

        WrJobsConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.itemCount = 64;
        cfg.requestedWorkers = 4;
        cfg.run = CountRun;
        cfg.done = CountDone;
        cfg.cost = CountCost;
        cfg.user = &c;

        int workers = 0;
        const int consumed = WrJobsRunAll(&cfg, NULL, NULL, &workers);

        bool exactlyOnce = true;
        for (int i = 0; i < 64; i++)
            if (c.ran[i] != 1)
                exactlyOnce = false;
        Check(exactlyOnce, "all 64 items ran exactly once");
        Check(consumed == 64 && c.doneN == 64,
              "and all 64 results came back");
        Check(workers == 4, "with the number of workers that was asked for");
        Check(c.peak > 1, "and they genuinely ran at the same time");

        // The slow item was submitted first and must not be reported first,
        // which is the whole difference between as_completed and pool.map.
        Check(c.done[0] != 0 && c.done[c.doneN - 1] == 0,
              "the slow item reports LAST, not first");
    }

    // -----------------------------------------------------------------------
    printf("\none item, or one worker, and no thread is created at all\n");
    {
        Counter c;
        memset(&c, 0, sizeof(c));
        c.slowItem = -1;
        WrJobsConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.itemCount = 1;
        cfg.requestedWorkers = 0;
        cfg.run = CountRun;
        cfg.done = CountDone;
        cfg.user = &c;
        int workers = 0;
        Check(WrJobsRunAll(&cfg, NULL, NULL, &workers) == 1 && c.ran[0] == 1,
              "a single item runs on the calling thread");
        Check(c.peak == 1, "and nothing was concurrent with it");

        memset(&c, 0, sizeof(c));
        c.slowItem = -1;
        cfg.itemCount = 20;
        cfg.requestedWorkers = 1;
        Check(WrJobsRunAll(&cfg, NULL, NULL, &workers) == 20 && workers == 1,
              "--jobs 1 runs everything serially");
        bool inOrder = true;
        for (int i = 0; i < 20; i++)
            if (c.done[i] != i)
                inOrder = false;
        Check(inOrder, "in submission order, which is what makes it comparable");
    }

    // -----------------------------------------------------------------------
    printf("\nadmission: a demo bigger than the whole budget still runs\n");
    {
        Counter c;
        memset(&c, 0, sizeof(c));
        c.slowItem = -1;
        // Item 3 wants ten times the budget. Without the "proceed if nothing
        // else is in flight" guard this waits for ever.
        for (int i = 0; i < 8; i++)
            c.sizes[i] = 1024;
        c.sizes[3] = 10ull * 1024 * 1024;

        WrJobsConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.itemCount = 8;
        cfg.requestedWorkers = 4;
        cfg.run = CountRun;
        cfg.done = CountDone;
        cfg.cost = CountCost;
        cfg.user = &c;
        cfg.budgetBytes = 1024ull * 1024;       // one megabyte, total

        const ULONGLONG t0 = GetTickCount64();
        const int consumed = WrJobsRunAll(&cfg, NULL, NULL, NULL);
        const ULONGLONG took = GetTickCount64() - t0;

        printf("     8 items, one of them ten times the budget, in %llu ms\n",
               took);
        Check(consumed == 8 && c.ran[3] == 1,
              "the oversized item runs rather than waiting for room");
        Check(took < 10000, "and the run finishes rather than hanging");
    }

    // -----------------------------------------------------------------------
    printf("\ntwo pools at once, each with its own deadline\n");
    {
        DeadlineJob tight, loose;
        memset(&tight, 0, sizeof(tight));
        memset(&loose, 0, sizeof(loose));
        tight.ms = 5;                   // every item overruns this
        loose.ms = 5000;                // and none overruns this

        HANDLE a = CreateThread(NULL, 0, DeadlineThread, &tight, 0, NULL);
        HANDLE b = CreateThread(NULL, 0, DeadlineThread, &loose, 0, NULL);
        HANDLE both[2] = {a, b};
        WaitForMultipleObjects(2, both, TRUE, 30000);
        CloseHandle(a);
        CloseHandle(b);

        Check(tight.overran == 8,
              "the job with a 5 ms budget saw all eight of its items overrun");
        Check(loose.overran == 0,
              "and the one running beside it saw none of its own overrun");
    }

    // -----------------------------------------------------------------------
    printf("\ncancellation stops claiming, and within a bound\n");
    {
        g_started = 0;
        g_cancelAfter = 4;
        int consumed = 0;

        WrJobsConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.itemCount = 400;
        cfg.requestedWorkers = 4;
        cfg.run = CancelRun;
        cfg.done = CancelDone;
        cfg.user = &consumed;

        const ULONGLONG t0 = GetTickCount64();
        const int ran = WrJobsRunAll(&cfg, CancelAbort, NULL, NULL);
        const ULONGLONG took = GetTickCount64() - t0;

        printf("     %d of 400 items ran before the flag landed, in %llu ms\n",
               ran, took);
        Check(ran < 400, "the remaining items are not claimed");
        Check(took < 5000, "and it unwinds promptly rather than draining");
        Check(ran == consumed, "every item that ran was still reported");
    }

    printf("\n%s\n\n", g_failures ? "SOME CHECKS FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
