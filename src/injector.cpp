// wrinject.exe  --  minimal LoadLibrary injector for wrlines.dll into momentum.exe.
//
// Classic remote-thread injection: write the DLL path into the target and
// CreateRemoteThread(LoadLibraryA). Must be built 64-bit, because Momentum Mod
// (Strata Source) is 64-bit only -- there is no 32-bit path in the install at
// all, and a 32-bit injector cannot write a usable kernel32 address into an
// x64 process.
//
// Run it any time after the game is up; you do not need to be in a map. WrLines
// waits for the swapchain itself. Re-run it after a rebuild -- but note the DLL
// never unloads by design (see dllmain.cpp), so injecting twice in one session
// gets you two copies. Restart the game between rebuilds.
//
// Usage:  wrinject.exe [--wait [seconds]] [path\to\wrlines.dll]
//         (the DLL defaults to wrlines.dll sitting next to this exe)
//
// --wait exists for Linux. Proton's PROTON_REMOTE_DEBUG_CMD starts this BEFORE
// the game, which is the only way to get a second Windows process into the same
// wineserver as a Steam game -- see docs\how-it-works.md. Without it the
// injector would look once, find nothing, and exit before the game existed.
// Nothing about the no-argument behaviour changes: on Windows this is the same
// program it has always been.

#ifndef WIN32_LEAN_AND_MEAN     // build.bat defines it too; a harness may not
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "wr_args.h"

// How often the wait loop looks, and how long it will keep waiting for the
// renderer after the process itself has appeared when the module list cannot be
// read at all. See WaitForGame.
#define WR_POLL_MS      500
#define WR_SETTLE_MS  20000

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------
//
// Under the Linux launch option Proton redirects this program's stdout into the
// Proton log, which is off unless the user set PROTON_LOG=1. And if the
// injection never happens there is no wrlines_data\wrlines.log either, because
// the DLL never loaded -- so on the one platform where this runs unattended, a
// failure would otherwise be completely silent.
//
// So in --wait mode every line also goes to a file beside the DLL. Without
// --wait no file is opened, no directory is created and this is the printf it
// always was, which is what keeps the Windows path unchanged to the byte.

static FILE *g_log = NULL;

static void Say(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);

    fputs(line, stdout);

    if (g_log)
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(g_log, "%02d:%02d:%02d  %s", t.wHour, t.wMinute, t.wSecond, line);
        fflush(g_log);      // the process may be killed by Proton at any moment
    }
}

// One run, one file. The previous attempt is not history worth keeping, and a
// log that grows without bound in somebody's game folder is a bug of its own.
static void OpenLog(const char *dllPath)
{
    char dir[MAX_PATH];
    strcpy_s(dir, sizeof(dir), dllPath);
    char *slash = strrchr(dir, '\\');
    if (!slash)
        return;
    *slash = '\0';

    char folder[MAX_PATH];
    if (_snprintf_s(folder, sizeof(folder), _TRUNCATE, "%s\\wrlines_data", dir) < 0)
        return;
    CreateDirectoryA(folder, NULL);     // already there in every normal case

    char path[MAX_PATH];
    if (_snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\wrinject.log", folder) < 0)
        return;
    fopen_s(&g_log, path, "w");         // failure is not worth reporting to a stdout nobody reads
}

static void Usage(void)
{
    printf("wrinject.exe -- loads wrlines.dll into a running Momentum Mod.\n\n");
    printf("  wrinject.exe                       inject now\n");
    printf("  wrinject.exe --wait [seconds]      wait for the game first (default %d)\n",
           WR_WAIT_DEFAULT_SECONDS);
    printf("  wrinject.exe path\\to\\wrlines.dll   use a DLL other than the one beside this exe\n");
    printf("  wrinject.exe --help                this\n\n");
    printf("On Linux, --wait is what the Proton launch option uses, because Proton\n");
    printf("starts this before the game. See the README.\n");
}

// ---------------------------------------------------------------------------
// Looking at the target
// ---------------------------------------------------------------------------

static DWORD FindPid(const char *exe)
{
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe))
    {
        do {
            if (_stricmp(pe.szExeFile, exe) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// Refuse to inject into a process of the wrong bitness rather than let
// CreateRemoteThread fail in a confusing way later.
static bool IsTarget64Bit(HANDLE proc, bool *ok)
{
    BOOL wow = FALSE;
    if (!IsWow64Process(proc, &wow)) { *ok = false; return false; }
    *ok = true;
    return !wow;    // on x64 Windows, "not WOW64" means a native 64-bit process
}

// Three answers, not two, and the third one matters.
//
// "I could not look" is a different fact from "it is not there", and the wait
// loop has to tell them apart: taking a snapshot of ANOTHER process's modules is
// exactly the kind of call Wine can implement partially, and treating a failed
// look as "not loaded yet" would leave every Linux user waiting for the whole
// deadline on a check that is only ever an optimisation.
enum WrModuleState { WR_MODULE_NO = 0, WR_MODULE_YES, WR_MODULE_UNKNOWN };

static WrModuleState ModuleState(DWORD pid, const char *dllName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE)
        return WR_MODULE_UNKNOWN;

    bool found = false;
    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    if (!Module32First(snap, &me))
    {
        // ERROR_BAD_LENGTH here means the module list changed underneath us,
        // which during startup it constantly is. The caller polls, so this
        // resolves itself.
        CloseHandle(snap);
        return WR_MODULE_UNKNOWN;
    }
    do {
        if (_stricmp(me.szModule, dllName) == 0) { found = true; break; }
    } while (Module32Next(snap, &me));

    CloseHandle(snap);
    return found ? WR_MODULE_YES : WR_MODULE_NO;
}

// ---------------------------------------------------------------------------
// --wait
// ---------------------------------------------------------------------------
//
// TWO gates, not one. Finding the pid is not enough: under the Linux launch
// option this program is already running when the game is created, so it would
// see the process the instant it exists and fire CreateRemoteThread into
// something still inside loader initialisation. So once the pid appears, wait
// for d3d11.dll in its module list -- which is the exact condition WrHookInit
// then waits sixty seconds for anyway, so this is not extra patience, it is the
// same patience moved somewhere it can be reported.
//
// The second gate is a grace period rather than a requirement: if the module
// list cannot be read at all, inject anyway after a settle, and say so, because
// a check that only improves the timing must never be the thing that stops the
// tool working.
static DWORD WaitForGame(int seconds, bool *blind)
{
    const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)seconds * 1000;
    ULONGLONG seenAt = 0;
    DWORD pid = 0;

    *blind = false;
    Say("[.] waiting up to %d s for momentum.exe ...\n", seconds);

    for (;;)
    {
        DWORD found = FindPid("momentum.exe");
        if (found && found != pid)
        {
            pid = found;
            seenAt = GetTickCount64();
            Say("[.] momentum.exe is up (pid %lu) -- waiting for d3d11.dll\n", pid);
        }
        else if (!found && pid)
        {
            // A launcher that re-execs itself, or a crash on startup. Either
            // way the pid we had is meaningless now.
            Say("[.] pid %lu went away -- still waiting\n", pid);
            pid = 0;
        }

        if (pid)
        {
            WrModuleState d3d = ModuleState(pid, "d3d11.dll");
            if (d3d == WR_MODULE_YES)
                return pid;
            if (d3d == WR_MODULE_UNKNOWN && GetTickCount64() - seenAt >= WR_SETTLE_MS)
            {
                *blind = true;
                return pid;
            }
        }

        if (GetTickCount64() >= deadline)
            return 0;
        Sleep(WR_POLL_MS);
    }
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    WrInjectArgs opt;
    WrParseInjectArgs(argc, argv, &opt);

    if (opt.help)
    {
        Usage();
        return 0;
    }
    if (opt.bad)
    {
        printf("[!] I do not understand this argument: %s\n\n", opt.bad);
        Usage();
        return 2;
    }

    char dllPath[MAX_PATH];

    if (opt.dll)
    {
        GetFullPathNameA(opt.dll, MAX_PATH, dllPath, NULL);
    }
    else
    {
        GetModuleFileNameA(NULL, dllPath, MAX_PATH);
        char *slash = strrchr(dllPath, '\\');
        if (slash) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - dllPath), "wrlines.dll");
    }

    // Only in --wait mode, and only after the path is known, because the log
    // goes beside the DLL.
    if (opt.wait)
        OpenLog(dllPath);

    if (GetFileAttributesA(dllPath) == INVALID_FILE_ATTRIBUTES)
    {
        Say("[!] DLL not found: %s\n", dllPath);
        Say("    Run build.bat first.\n");
        return 1;
    }

    DWORD pid;
    bool blind = false;

    if (opt.wait)
    {
        pid = WaitForGame(opt.waitSeconds, &blind);
        if (!pid)
        {
            Say("[!] momentum.exe never appeared within %d s -- giving up.\n",
                opt.waitSeconds);
            Say("    If the game did start, check that Steam is forcing a Proton\n");
            Say("    version rather than running the native Linux build.\n");
            return 1;
        }
        if (blind)
        {
            Say("[i] could not read the module list of pid %lu, so there is no way\n", pid);
            Say("    to tell whether the renderer is up. Injecting anyway after %d s.\n",
                WR_SETTLE_MS / 1000);
        }
    }
    else
    {
        pid = FindPid("momentum.exe");
        if (!pid)
        {
            Say("[!] momentum.exe is not running. Launch Momentum Mod first.\n");
            Say("    (--wait makes this sit and wait for it instead. That is what\n");
            Say("     the Linux launch option uses -- see the README.)\n");
            return 1;
        }
    }

    if (ModuleState(pid, "wrlines.dll") == WR_MODULE_YES)
    {
        // In --wait mode this is the correct end state, not a failure: the
        // launch option can fire again over a session and there is nothing
        // alarming about the DLL already being where it belongs.
        if (opt.wait)
        {
            Say("[+] wrlines.dll is already loaded in momentum.exe (pid %lu). Nothing to do.\n",
                pid);
            return 0;
        }
        Say("[!] wrlines.dll is already loaded in momentum.exe (pid %lu).\n", pid);
        Say("    It does not support unloading -- restart the game to re-inject.\n");
        return 1;
    }

    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                               PROCESS_VM_WRITE | PROCESS_VM_READ |
                               PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProc)
    {
        Say("[!] OpenProcess failed (err %lu). Try running this as Administrator.\n",
            GetLastError());
        return 1;
    }

    bool bitnessKnown = false;
    bool is64 = IsTarget64Bit(hProc, &bitnessKnown);
    if (bitnessKnown && !is64)
    {
        Say("[!] momentum.exe (pid %lu) is a 32-bit process. This injector and\n", pid);
        Say("    wrlines.dll are 64-bit. Something is very wrong -- aborting.\n");
        CloseHandle(hProc);
        return 1;
    }

    size_t len = strlen(dllPath) + 1;
    void *remote = VirtualAllocEx(hProc, NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote)
    {
        Say("[!] VirtualAllocEx failed (err %lu)\n", GetLastError());
        CloseHandle(hProc);
        return 1;
    }
    if (!WriteProcessMemory(hProc, remote, dllPath, len, NULL))
    {
        Say("[!] WriteProcessMemory failed (err %lu)\n", GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return 1;
    }

    // kernel32 is loaded at the same base in every process on a given boot, so
    // our LoadLibraryA address is valid in the target too.
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    LPTHREAD_START_ROUTINE loadlib =
        (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryA");

    HANDLE hThread = CreateRemoteThread(hProc, NULL, 0, loadlib, remote, 0, NULL);
    if (!hThread)
    {
        Say("[!] CreateRemoteThread failed (err %lu)\n", GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return 1;
    }

    // CHECK THE WAIT. On a timeout GetExitCodeThread hands back STILL_ACTIVE
    // (259), which is indistinguishable from a very small HMODULE -- this used
    // to be printed as "LoadLibraryA -> 0x00000103" and reported as a success.
    DWORD waited = WaitForSingleObject(hThread, 10000);
    if (waited != WAIT_OBJECT_0)
    {
        Say("[!] LoadLibrary in the target has not returned after 10 s.\n");
        Say("    It may still be loading -- check wrlines_data\\wrlines.log.\n");
        // Deliberately NOT freeing the remote string: the thread is still
        // running and that is the buffer it is reading its path from. A quarter
        // of a kilobyte left behind in a process that is about to be told about
        // it anyway is cheaper than a use-after-free in somebody's game.
        CloseHandle(hThread);
        CloseHandle(hProc);
        return 1;
    }

    DWORD loaded = 0;
    GetExitCodeThread(hThread, &loaded);   // low 32 bits of the HMODULE

    Say("[+] Injected wrlines.dll into momentum.exe (pid %lu). LoadLibraryA -> 0x%08lX\n",
        pid, loaded);
    if (loaded == 0)
    {
        Say("    (LoadLibrary returned NULL -- check the DLL is 64-bit and that its\n");
        Say("     dependencies resolve. Built with /MT there should be none.)\n");
    }
    else
    {
        Say("    In game: press INSERT to open the WrLines panel.\n");
        Say("    Log: <dll dir>\\wrlines_data\\wrlines.log\n");
    }

    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    CloseHandle(hThread);
    CloseHandle(hProc);
    return (loaded == 0) ? 1 : 0;
}
