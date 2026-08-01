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
// Usage:  wrinject.exe [path\to\wrlines.dll]
//         (defaults to wrlines.dll sitting next to this exe)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

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

static bool AlreadyInjected(DWORD pid, const char *dllName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE)
        return false;

    bool found = false;
    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    if (Module32First(snap, &me))
    {
        do {
            if (_stricmp(me.szModule, dllName) == 0) { found = true; break; }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

int main(int argc, char **argv)
{
    char dllPath[MAX_PATH];

    if (argc > 1)
    {
        GetFullPathNameA(argv[1], MAX_PATH, dllPath, NULL);
    }
    else
    {
        GetModuleFileNameA(NULL, dllPath, MAX_PATH);
        char *slash = strrchr(dllPath, '\\');
        if (slash) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - dllPath), "wrlines.dll");
    }

    if (GetFileAttributesA(dllPath) == INVALID_FILE_ATTRIBUTES)
    {
        printf("[!] DLL not found: %s\n", dllPath);
        printf("    Run build.bat first.\n");
        return 1;
    }

    DWORD pid = FindPid("momentum.exe");
    if (!pid)
    {
        printf("[!] momentum.exe is not running. Launch Momentum Mod first.\n");
        return 1;
    }

    if (AlreadyInjected(pid, "wrlines.dll"))
    {
        printf("[!] wrlines.dll is already loaded in momentum.exe (pid %lu).\n", pid);
        printf("    It does not support unloading -- restart the game to re-inject.\n");
        return 1;
    }

    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                               PROCESS_VM_WRITE | PROCESS_VM_READ |
                               PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProc)
    {
        printf("[!] OpenProcess failed (err %lu). Try running this as Administrator.\n",
               GetLastError());
        return 1;
    }

    bool bitnessKnown = false;
    bool is64 = IsTarget64Bit(hProc, &bitnessKnown);
    if (bitnessKnown && !is64)
    {
        printf("[!] momentum.exe (pid %lu) is a 32-bit process. This injector and\n", pid);
        printf("    wrlines.dll are 64-bit. Something is very wrong -- aborting.\n");
        CloseHandle(hProc);
        return 1;
    }

    size_t len = strlen(dllPath) + 1;
    void *remote = VirtualAllocEx(hProc, NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote)
    {
        printf("[!] VirtualAllocEx failed (err %lu)\n", GetLastError());
        CloseHandle(hProc);
        return 1;
    }
    if (!WriteProcessMemory(hProc, remote, dllPath, len, NULL))
    {
        printf("[!] WriteProcessMemory failed (err %lu)\n", GetLastError());
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
        printf("[!] CreateRemoteThread failed (err %lu)\n", GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return 1;
    }

    WaitForSingleObject(hThread, 10000);
    DWORD loaded = 0;
    GetExitCodeThread(hThread, &loaded);   // low 32 bits of the HMODULE

    printf("[+] Injected wrlines.dll into momentum.exe (pid %lu). LoadLibraryA -> 0x%08lX\n",
           pid, loaded);
    if (loaded == 0)
    {
        printf("    (LoadLibrary returned NULL -- check the DLL is 64-bit and that its\n");
        printf("     dependencies resolve. Built with /MT there should be none.)\n");
    }
    else
    {
        printf("    In game: press INSERT to open the WrLines panel.\n");
        printf("    Log: <dll dir>\\wrlines_data\\wrlines.log\n");
    }

    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    CloseHandle(hThread);
    CloseHandle(hProc);
    return (loaded == 0) ? 1 : 0;
}
