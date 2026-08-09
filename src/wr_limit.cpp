// wr_limit.cpp  --  see wr_limit.h for why this exists and how it paces.

#include "wr_limit.h"
#include "wr_pacing.h"
#include "wr_hook.h"
#include "wr_log.h"

#include <math.h>

// Not in every SDK's headers, and we resolve the function dynamically anyway so
// the binary still loads on a Windows without it.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

typedef HANDLE (WINAPI *CreateWaitableTimerExW_t)(LPSECURITY_ATTRIBUTES, LPCWSTR,
                                                  DWORD, DWORD);

WrLimitSettings g_limit;

static long long g_qpf = 0;
static WrPacing g_pace = {0, false};
static long long g_lastPresent = 0;
static HANDLE g_timer = NULL;
static bool g_timerHiRes = false;
static bool g_timerTried = false;

static float g_frameMs = 0.0f;
static float g_jitterMs = 0.0f;
static float g_jitterPeak = 0.0f;
static int g_jitterFrames = 0;
static float g_spinPercent = 0.0f;
static float g_refreshHz = 0.0f;
static int g_lateFrames = 0;
static int g_totalFrames = 0;
static bool g_cpuBound = false;

void WrLimitDefaults(void)
{
    // Off by default. This is somebody else's job on most machines, and turning
    // a frame limiter on behind the user's back is not a small thing to do.
    g_limit.enabled = false;
    g_limit.autoTarget = true;
    g_limit.targetFps = 160.0f;
    // Three below the refresh rate is the usual variable-refresh recommendation:
    // enough margin that a late frame never reaches the panel's ceiling and
    // falls out of the VRR window, small enough not to waste headroom.
    g_limit.headroomHz = 3.0f;
    g_limit.spinMs = 0.35f;
}

static long long Qpf(void)
{
    if (g_qpf == 0)
    {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        g_qpf = q.QuadPart ? q.QuadPart : 1;
    }
    return g_qpf;
}

static inline long long Now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// The refresh rate of the monitor the game is actually on, not the primary one.
// Re-read occasionally: people move windows between displays, and a borderless
// game following them would otherwise be paced for the wrong panel.
static void RefreshDisplayRate(void)
{
    static DWORD lastCheck = 0;
    DWORD now = GetTickCount();
    if (lastCheck != 0 && (now - lastCheck) < 2000)
        return;
    lastCheck = now;

    HWND wnd = WrGameWindow();
    if (!wnd)
        return;

    MONITORINFOEXW mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST);
    if (!mon || !GetMonitorInfoW(mon, &mi))
        return;

    DEVMODEW dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        return;
    if (dm.dmDisplayFrequency <= 1)     // 0 and 1 both mean "hardware default"
        return;

    float hz = (float)dm.dmDisplayFrequency;
    if (fabsf(hz - g_refreshHz) > 0.5f)
    {
        g_refreshHz = hz;
        WrLogf("limiter: display is %.0f Hz", hz);
    }
}

static void EnsureTimer(void)
{
    if (g_timerTried)
        return;
    g_timerTried = true;

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    CreateWaitableTimerExW_t create = k32
        ? (CreateWaitableTimerExW_t)GetProcAddress(k32, "CreateWaitableTimerExW")
        : NULL;
    if (!create)
    {
        WrLogf("[!] limiter: no CreateWaitableTimerExW; falling back to spinning");
        return;
    }

    // Auto-reset, high resolution. The high-resolution flag needs Windows 10
    // 1803 or newer; without it the wait quantises to the system timer tick,
    // which is far too coarse to pace frames with.
    g_timer = create(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                     TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (g_timer)
    {
        g_timerHiRes = true;
        WrLogf("limiter: high-resolution waitable timer ready");
        return;
    }

    g_timer = create(NULL, NULL, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (g_timer)
        WrLogf("limiter: only a coarse waitable timer is available; the spin "
               "window will do more of the work");
    else
        WrLogf("[!] limiter: could not create a waitable timer; spinning only");
}

float WrLimitRefreshHz(void) { return g_refreshHz; }

float WrLimitTargetFps(void)
{
    if (g_limit.autoTarget)
    {
        if (g_refreshHz <= 1.0f)
            return g_limit.targetFps;       // refresh unknown; use the manual value
        float t = g_refreshHz - g_limit.headroomHz;
        return t < WR_LIMIT_MIN_FPS ? WR_LIMIT_MIN_FPS : t;
    }
    return WrClampF(g_limit.targetFps, WR_LIMIT_MIN_FPS, WR_LIMIT_MAX_FPS);
}

// Record the interval between this present and the previous one, and how far it
// missed the target by. Called at the very end of the tick -- the same point in
// every frame -- so the number really is present-to-present and not "present to
// somewhere in the middle of the next frame's work".
static void RecordInterval(long long done, float targetMs)
{
    if (g_lastPresent != 0)
    {
        float ms = (float)((double)(done - g_lastPresent) * 1000.0 / (double)Qpf());
        if (ms > 0.0f && ms < 1000.0f)
        {
            g_frameMs = (g_frameMs <= 0.0f) ? ms : (g_frameMs * 0.95f + ms * 0.05f);

            if (targetMs > 0.0f)
            {
                // Worst deviation over a rolling window. A smoothed average
                // would hide exactly the spikes that read as judder, which is
                // the only thing this number is for.
                float dev = fabsf(ms - targetMs);
                if (dev > g_jitterPeak)
                    g_jitterPeak = dev;
                if (++g_jitterFrames >= 120)
                {
                    g_jitterMs = g_jitterPeak;
                    g_jitterPeak = 0.0f;
                    g_jitterFrames = 0;
                }
            }
        }
    }
    g_lastPresent = done;
}

void WrLimitTick(void)
{
    if (!g_limit.enabled)
    {
        WrPacingReset(&g_pace);
        g_cpuBound = false;
        g_spinPercent = 0.0f;
        g_jitterMs = 0.0f;
        RecordInterval(Now(), 0.0f);        // keep the frame time meter alive
        return;
    }

    RefreshDisplayRate();
    EnsureTimer();

    float targetFps = WrLimitTargetFps();
    long long period = (long long)((double)Qpf() / (double)targetFps);
    float targetMs = (float)((double)period * 1000.0 / (double)Qpf());
    if (period <= 0)
    {
        RecordInterval(Now(), 0.0f);
        return;
    }

    long long now = Now();
    long long target = WrPacingTargetFor(&g_pace, now, period);
    if (target == 0)
    {
        RecordInterval(now, targetMs);      // first frame: sets the phase only
        return;
    }

    // Did this frame's work already overrun the budget? If so no amount of
    // waiting helps, and the cap is not what is limiting the frame rate.
    g_totalFrames++;
    if (now > target)
        g_lateFrames++;
    if (g_totalFrames >= 120)
    {
        g_cpuBound = (g_lateFrames * 4 > g_totalFrames);   // late more than 25%
        g_totalFrames = 0;
        g_lateFrames = 0;
    }

    long long spinTicks = (long long)((double)Qpf() * (double)g_limit.spinMs / 1000.0);
    long long spinTotal = 0;

    for (;;)
    {
        long long t = Now();
        long long remaining = target - t;
        if (remaining <= 0)
            break;

        if (remaining > spinTicks && g_timer)
        {
            // Sleep everything except the spin tail. A negative due time is
            // relative, in 100 ns units.
            LARGE_INTEGER due;
            due.QuadPart = -((remaining - spinTicks) * 10000000LL / Qpf());
            if (due.QuadPart >= 0)
                continue;
            if (!SetWaitableTimer(g_timer, &due, 0, NULL, NULL, FALSE))
                break;                      // timer refused; do not spin forever
            // Bounded, so a lost timer cannot wedge the render thread.
            WaitForSingleObject(g_timer, 100);
        }
        else
        {
            long long spinFrom = t;
            YieldProcessor();
            spinTotal += Now() - spinFrom;
        }
    }

    float spinMs = (float)((double)spinTotal * 1000.0 / (double)Qpf());
    float pct = targetMs > 0.0f ? (spinMs / targetMs) * 100.0f : 0.0f;
    g_spinPercent = g_spinPercent * 0.95f + pct * 0.05f;

    long long done = Now();
    WrPacingAdvance(&g_pace, done, period);     // see wr_pacing.h
    RecordInterval(done, targetMs);
}

float WrLimitFrameMs(void) { return g_frameMs; }
float WrLimitJitterMs(void) { return g_jitterMs; }
float WrLimitSpinPercent(void) { return g_spinPercent; }
bool WrLimitCpuBound(void) { return g_cpuBound; }
