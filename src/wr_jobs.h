// wr_jobs.h  --  the worker pool extraction runs on.
//
// Each demo is completely independent -- separate file in, separate file out --
// so this is about as parallel as work gets. It matters because the cost per
// demo is wildly uneven: 0.7 s on a normal map, up to a minute on a bad one,
// which serially reads as "it did four quickly and then stopped".
//
// The pool arrives with extraction and not before. The map index, the
// leaderboard and the fetcher are all single-threaded, and building concurrency
// machinery before there is anything concurrent to run means designing the
// coexistence story in the abstract.
//
// ---------------------------------------------------------------------------
// WHAT CHANGED BY MOVING IN-PROCESS, AND WHY EACH OF THEM IS A REAL PROBLEM
// ---------------------------------------------------------------------------
//
// The reference runs a ProcessPoolExecutor. Four of its properties were free
// there and have to be paid for here.
//
// CRASH ISOLATION. A worker process that dies takes nothing with it and the
// parent reports an exit code. A worker THREAD that faults takes the game down
// with it, mid-frame, with no fallback switch to turn extraction off. See
// WrJobsGuard below, which is the only __try in this project and says why.
//
// MEMORY. A separate process has its own address space and its own OOM. Here a
// 48 MB demo's working set lives in the game's heap, times N workers, and an
// allocation failure is the game's allocation failure. Hence the budget: a
// worker waits for room before it starts, and the wait has one guard that makes
// it impossible to deadlock -- A WORKER ALWAYS PROCEEDS IF NOTHING ELSE IS IN
// FLIGHT, so the largest demo in the world eventually runs, alone.
//
// KILLABILITY. Stop was TerminateJobObject, which is instant and total. There
// is no equivalent: TerminateThread on a thread of somebody else's process
// leaks whatever locks it held, and the CRT heap lock is one of them, so the
// game would deadlock on its next malloc. Cancellation is therefore
// cooperative, through one predicate, and it is SLOWER than what it replaces --
// see the honesty note under WrJobsAbort.
//
// THE DEADLINE. The reference's is a module global, and each of its workers is
// a separate process, so each gets its own. Transcribing that shape as a static
// silently gives N threads one shared clock: the first demo to start would set
// a deadline that the fourth demo, starting thirty seconds later, would trip
// almost immediately. It lives in the per-worker context here, and reaches
// wr_dp.cpp through the abort callback's user pointer.
//
// ---------------------------------------------------------------------------
// WHAT THE COORDINATOR IS FOR
// ---------------------------------------------------------------------------
//
// Exactly one thread emits, and it is the caller's own. Workers compute; the
// coordinator prints, writes the failure record and decides what to do next.
//
// That is not tidiness. In the reference the consume loop runs in the PARENT
// process, so _flush_failures is serialised for free; two workers failing on
// the same map would otherwise race the same temp file and one of the records
// would be lost. Workers here write only their own <basename>.wrpath, through a
// temp file and a rename, which cannot collide with anything.
//
// It is also why WrDataPath's four unlocked rotating buffers are still safe.
// The rule is structural, not remembered: every path is resolved ONCE PER JOB,
// on the coordinator, before any worker starts, and workers are handed absolute
// paths with nothing left to look up.

#ifndef WR_JOBS_H
#define WR_JOBS_H

#include "wr_common.h"

// Never more than this many threads, whatever the machine has and whatever was
// asked for. The bound is memory rather than CPU: every worker holds a
// decompressed body and a candidate arena, and a 32-core machine running 30 of
// them would be several gigabytes inside a game's address space for no gain --
// the work is memory-bound long before it is core-bound.
#define WR_JOBS_MAX_WORKERS 16

// ---------------------------------------------------------------------------
// How many workers
// ---------------------------------------------------------------------------

// A pure function of three integers so that tests\test_jobs.exe can drive it as
// a table with no threads involved.
//
// `requested` > 0 is taken as given, which is what --jobs N means. Otherwise it
// is logical cores minus two, and the reason for the two has CHANGED in the
// move in-process. The reference held two back because its children inherited a
// below-normal priority class and priority does not help if every core is
// occupied. Here one of the cores we would take IS the game's render thread,
// and the other is its main thread, and the whole point of the panel is that
// you can keep playing while this runs.
//
// Two clamps the reference does not have. It spawns N processes regardless and
// lets the extras idle, which costs nothing there; here each thread holds a
// scan arena, so more workers than items is memory for nothing. And the cap
// above.
int WrJobsWorkerCount(int requested, int logicalCores, int itemCount);

// ---------------------------------------------------------------------------
// The memory budget
// ---------------------------------------------------------------------------

// Peak bytes one demo needs, per byte of the file on disk.
//
// MEASURED, not guessed -- this is the one constant in the project that could
// not be derived from anything, and a guessed memory budget inside somebody
// else's game is not a thing to guess. The measurement is
// tests\parity.ps1 -Verb budget, which runs --dump-info over a real library and
// prints the distribution; the peak it models is the larger of two moments:
//
//     the whole file and its decompressed body alive at once
//     the body, plus 70 bytes per candidate, plus 48 bytes per point
//
// The 70 is 16 for the candidate itself and 54 for the eight index arrays, two
// bitmaps and three scratch buffers the dynamic program needs beside it.
//
// Measured over the WHOLE library -- 6,062 demos that extract, across every map
// on this machine, not the 176 of one map the first number came from:
//
//     min 6.3   median 15.7   p95 16.9   p99 18.6   max 99.3
//
// 19 is that p99 rounded up. It was 17, taken from one map, and the wider sweep
// says 17 under-states 230 demos of 6,062 -- so the old number was not wrong
// about surf_demise, it was wrong about how much surf_demise resembled
// everything else.
//
// The p99 and not the mean, and the difference is not academic: a body of
// nothing but zeros makes a candidate of EVERY bit position it covers -- a zero
// word is plausible by the reference's own test and decodes to 0.0, which is
// inside the world limit -- so the tail of this distribution is not shaped like
// the middle of it. The max of 99.3 is one such body, and it is harmless
// because it is a RATIO: the file it belongs to is tiny, and this constant is
// only ever multiplied back into bytes.
//
// Worth knowing what the absolute numbers turned out to be, because they say
// the two constants below are not close to binding: the median demo peaks at
// about 1 MB, the p99 at 27 MB, and the largest single peak in the library is
// 42 MB. The very biggest files -- the 48 MB ones -- are all zstd and are
// skipped before any of this is allocated.
#define WR_JOBS_COST_MULT 19

// Total bytes of demo allowed in flight across all workers at once. Reaching it
// does not fail anything; it makes the next worker wait.
//
// At sixteen workers that is 48 MB of ESTIMATED PEAK each -- the units here are
// WrJobsCost's, not file bytes -- before anyone has to queue. The largest peak
// measured anywhere in this library is 42 MB, so the budget is set to let every
// worker hold a worst case at once and still not queue. In practice the median
// demo peaks at about 1 MB and this is never reached.
#define WR_JOBS_BUDGET_BYTES (768ull * 1024ull * 1024ull)

// Above this, one demo is refused outright and becomes an ordinary recorded
// failure with a reason naming its size. At the cost multiplier above it works
// out to an 81 MB demo.
//
// Nothing in this library comes near it, and the reason is worth stating
// because the obvious counter-example is not one: the biggest files here are
// 46.6 MB, and every single one of them is a zstd body, which is refused before
// a byte is decompressed and never reaches this test at all. The largest demo
// that actually gets extracted is 2.1 MB. So this refuses the absurd rather
// than the merely large, by a factor of forty.
//
// A deliberate improvement on the reference, which raises MemoryError out of
// fut.result() and aborts cmd_extract entirely -- losing the epilogue and every
// failure record the run had not yet flushed. One demo being too big is not a
// reason to throw away the run.
#define WR_JOBS_MAX_ONE (1536ull * 1024ull * 1024ull)

// What a demo of this size will want, in bytes.
unsigned long long WrJobsCost(long long fileBytes);

// True if it can never be admitted. The driver checks this while building its
// list, so the refusal is recorded like any other failure rather than being a
// worker that never starts.
bool WrJobsTooBig(long long fileBytes);

// ---------------------------------------------------------------------------
// Running a set of items
// ---------------------------------------------------------------------------

// Polled by the pool and passed down into wr_dp.cpp. Non-zero stops everything.
//
// HONEST ABOUT WHAT THIS COSTS. TerminateJobObject was instant. This is not:
// the candidate scan checks 32 times over what can be thirty seconds on a large
// body, so Stop can take a couple of seconds to land, and a demo already inside
// the dynamic program checks every 4096 candidates. The panel says so rather
// than claiming a clean stop, and WrJobsRunAll logs a line if the flag has been
// set for ten seconds and workers are still alive -- because at that point
// something is wrong and a silent wait is the worst way to present it.
typedef int (*WrJobsAbort)(void *user);

struct WrJobsConfig
{
    int itemCount;
    int requestedWorkers;       // 0 means decide

    // On a worker thread. Everything it needs must be reachable from `user`
    // and `index`; it is handed no shared mutable state and no paths to resolve.
    void (*run)(void *user, int index);

    // On the CALLER'S thread, once per finished item, in COMPLETION ORDER --
    // which is submit + as_completed and not pool.map. The reference had this
    // wrong once: its docstring said "in whatever order they finish" while the
    // code used pool.map, so one slow demo held back the progress line of every
    // finished demo behind it and the panel went silent for as long as that
    // demo took. The reported symptom was the extractor looking hung. It was
    // not hung, it was mute.
    void (*done)(void *user, int index);

    // Bytes this item will want, for admission. NULL means everything is free.
    unsigned long long (*cost)(void *user, int index);

    void *user;
    unsigned long long budgetBytes;     // 0 uses WR_JOBS_BUDGET_BYTES
};

// Returns the number of items that completed. Fewer than itemCount means the
// abort predicate fired.
//
// SERIAL WHEN THERE IS NO POINT NOT BEING. One worker, or one item, and this
// runs everything on the calling thread with no thread created and no line
// printed -- matching the reference's own `jobs <= 1 or len(targets) < 2`
// branch, which is also what makes --jobs 1 a deterministic, comparable order.
int WrJobsRunAll(const WrJobsConfig *cfg, WrJobsAbort abort, void *abortUser,
                 int *workersUsed);

// ---------------------------------------------------------------------------
// The one __try in this project
// ---------------------------------------------------------------------------
//
// Wraps the per-demo body so an access violation inside somebody else's
// netstream becomes one recorded failure instead of the game disappearing.
// SEH rather than C++ EH, so there is no unwind cost and no destructor in
// scope: six lines, one filter, no state.
//
// I will not pretend this is safe. Catching an AV leaves the heap in whatever
// state the fault left it, and the honest position is that a repeat is a bug to
// fix rather than a condition to live with -- so it is logged loudly, with the
// code and the demo, every single time. It earns its place because the
// alternative is worse: the reference could afford to let a worker die because
// the worker was a process.
//
// Returns 0 normally, or the exception code.
unsigned long WrJobsGuard(void (*fn)(void *user, int index), void *user,
                          int index);

#endif // WR_JOBS_H
