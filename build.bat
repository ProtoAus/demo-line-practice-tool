@echo off
rem Build wrlines.dll + wrinject.exe as 64-bit using MSVC.
rem
rem Momentum Mod (Strata Source) is x64-only -- there is no 32-bit path in the
rem whole install -- so both targets must be x64.
rem
rem MSVC rather than the mingw in msys2, on purpose: we call MSVC-laid-out C++
rem virtual interfaces in the engine, and several Source methods return structs
rem by value where mingw's Itanium sret convention does not match MSVC's.
rem
rem Note we link dxguid.lib but NOT d3d11.lib or dxgi.lib. Nothing here calls an
rem exported D3D11/DXGI function: the swapchain vtable is read via
rem GetModuleHandle + GetProcAddress on whichever d3d11.dll the game already
rem loaded. That is what makes this work identically under native D3D11 and
rem under DXVK. dxguid.lib is just a static GUID blob with no DLL behind it.

setlocal enabledelayedexpansion
cd /d "%~dp0"

rem WrLines never unloads (see dllmain.cpp), so while the game is running the
rem loaded wrlines.dll cannot be overwritten. Catch that here rather than let the
rem linker report it 30 seconds later as a bare LNK1104.
tasklist /FI "IMAGENAME eq momentum.exe" 2>nul | find /I "momentum.exe" >nul
if not errorlevel 1 (
    echo [!] momentum.exe is running, and wrlines.dll is loaded inside it.
    echo     The DLL never unloads by design, so it cannot be replaced while the
    echo     game is up. Close the game, run build.bat again, then wrinject.exe.
    exit /b 1
)

rem Finding the toolchain is vcvars.bat's job, and it is a separate file because
rem tests\build.bat needs exactly the same lookup and the two copies had already
rem drifted -- see the essay at the top of it.
call "%~dp0vcvars.bat"
if errorlevel 1 exit /b 1

if not exist imgui\imgui.cpp (
    echo [!] imgui\ is missing. Fetch it with:
    echo     git clone --depth 1 --branch v1.91.9b https://github.com/ocornut/imgui imgui
    exit /b 1
)
if not exist minhook\src\hook.c (
    echo [!] minhook\ is missing. Fetch it with:
    echo     git clone --depth 1 --branch v1.3.3 https://github.com/TsudaKageyu/MinHook minhook
    exit /b 1
)

rem hde64.c, not hde32.c: the target is 64-bit.
set "MH=minhook"
set "MHSRC=%MH%\src\buffer.c %MH%\src\hook.c %MH%\src\trampoline.c %MH%\src\hde\hde64.c"

rem Committed rather than cloned, unlike imgui and minhook. third_party\
rem VERSION.txt argues that properly and records the upstream tag, the archive's
rem sha256 and what each option below switches off.
set "TP=third_party"
set "TPSRC=%TP%\miniz\miniz.c %TP%\lzma\LzmaDec.c"
rem zstd, decompression only. Ten .c files where LZMA needs one, which
rem VERSION.txt argues; the short single-file decoder in zstd's own doc\ tree
rem calls exit(1) on malformed input and would close the game.
set "ZS=%TP%\zstd"
set "TPSRC=%TPSRC% %ZS%\common\debug.c %ZS%\common\entropy_common.c"
set "TPSRC=%TPSRC% %ZS%\common\error_private.c %ZS%\common\fse_decompress.c"
set "TPSRC=%TPSRC% %ZS%\common\xxhash.c %ZS%\common\zstd_common.c"
set "TPSRC=%TPSRC% %ZS%\decompress\huf_decompress.c %ZS%\decompress\zstd_ddict.c"
set "TPSRC=%TPSRC% %ZS%\decompress\zstd_decompress.c"
set "TPSRC=%TPSRC% %ZS%\decompress\zstd_decompress_block.c"
set "TPINC=/I%TP%\miniz /I%TP%\lzma /I%ZS%"
rem NO_ARCHIVE_WRITING_APIS is not here: miniz.h defines it itself the moment
rem NO_ARCHIVE_APIS is set, and setting both is a C4005 redefinition warning.
rem LzmaDec.c wants no options at all -- VERSION.txt says why the obvious
rem _7ZIP_ST is not among them.
set "TPDEFS=/DMINIZ_NO_ARCHIVE_APIS /DMINIZ_NO_DEFLATE_APIS"
set "TPDEFS=%TPDEFS% /DMINIZ_NO_STDIO /DMINIZ_NO_TIME"
set "TPDEFS=%TPDEFS% /DMINIZ_NO_ZLIB_COMPATIBLE_NAMES"
rem zstd's options, all four switching something OFF. VERSION.txt says what each
rem one costs. ZSTD_DISABLE_ASM is the load-bearing one: without it the build
rem wants huf_decompress_amd64.S assembled, and that file is not vendored.
set "TPDEFS=%TPDEFS% /DZSTD_DISABLE_ASM=1 /DZSTD_LEGACY_SUPPORT=0"
set "TPDEFS=%TPDEFS% /DZSTD_MULTITHREAD=0 /DZSTD_NO_TRACE=1"

set "IM=imgui"
set "IMSRC=%IM%\imgui.cpp %IM%\imgui_draw.cpp %IM%\imgui_tables.cpp %IM%\imgui_widgets.cpp"
set "IMSRC=%IMSRC% %IM%\backends\imgui_impl_dx11.cpp %IM%\backends\imgui_impl_win32.cpp"

rem Our own code lives in src\. The build still runs from the repo root, so the
rem .obj files and both binaries stay where they were.
set "S=src"
set "SRC=%S%\dllmain.cpp %S%\wr_log.cpp %S%\wr_pe.cpp %S%\wr_probe.cpp"
set "SRC=%SRC% %S%\wr_engine.cpp %S%\wr_scan.cpp %S%\wr_hook.cpp %S%\wr_imgui.cpp"
set "SRC=%SRC% %S%\wr_render.cpp %S%\wr_path.cpp %S%\wr_ui.cpp %S%\wr_steam.cpp"
set "SRC=%SRC% %S%\wr_energy.cpp %S%\wr_limit.cpp %S%\wr_extract.cpp"
set "SRC=%SRC% %S%\wr_timer.cpp %S%\wr_savelocs.cpp %S%\wr_maps.cpp"
set "SRC=%SRC% %S%\wr_profile.cpp %S%\wr_board.cpp %S%\wr_intogame.cpp"
set "SRC=%SRC% %S%\wr_start.cpp %S%\wr_settings.cpp %S%\wr_quick.cpp"
set "SRC=%SRC% %S%\wr_json.cpp %S%\wr_msml.cpp %S%\wr_mtv.cpp %S%\wr_peek.cpp"
set "SRC=%SRC% %S%\wr_http.cpp %S%\wr_api.cpp %S%\wr_fetch.cpp"
set "SRC=%SRC% %S%\wr_dp.cpp %S%\wr_demo.cpp %S%\wr_jobs.cpp"
rem wr_sha256.cpp is written out rather than linked from BCrypt or ADVAPI32 on
rem purpose: either of those would add a seventh import and break the claim the
rem README makes about this list. wr_sha256.h has the argument in full.
set "SRC=%SRC% %S%\wr_sha256.cpp %S%\wr_update.cpp"

rem IMGUI_USER_CONFIG is resolved by the preprocessor relative to the file that
rem does the #include -- imgui\imgui.h -- and NOT relative to the working
rem directory or to /I. So this path is "up out of imgui\, then into src\", and
rem it has to move whenever wr_imconfig.h does.
set "DEFS=/DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS"
set "DEFS=%DEFS% /DIMGUI_USER_CONFIG=\"../src/wr_imconfig.h\""

rem /fp:precise is MSVC's default and /O2 does not change it, so this flag adds
rem nothing today. It is written down as a lock. /fp:fast would let the compiler
rem reassociate and contract floating point, which would break the exactness the
rem energy arithmetic is asserted on in wr_budget.h, and -- once the extractor
rem is C++ rather than Python -- would break bit-for-bit agreement with the
rem reference implementation, which is the only way that port can be checked.
rem Do not replace this with /fp:fast, and do not add /arch:AVX2.
set "FP=/fp:precise"

echo.
echo === wrlines.dll (x64) ===
cl /nologo /c /O2 /MT /EHsc /Zi /W3 %FP% %DEFS% /I%S% /I"%MH%\include" /I"%IM%" ^
   %TPINC% %SRC% %IMSRC%
if errorlevel 1 ( echo. & echo [!] compile FAILED & exit /b 1 )

rem The C dependencies, on their own line: they are C, they do not want /EHsc,
rem and they are not ours to warn about at our level.
cl /nologo /c /O2 /MT /W3 %FP% /I"%MH%\include" %MHSRC%
if errorlevel 1 ( echo. & echo [!] minhook compile FAILED & exit /b 1 )

cl /nologo /c /O2 /MT /W3 %FP% %TPDEFS% %TPINC% %TPSRC%
if errorlevel 1 ( echo. & echo [!] third-party compile FAILED & exit /b 1 )

rem The version block. See src\wrlines.rc for why the DLL has one and why it
rem has no icon.
rc /nologo /fo wrlines.res %S%\wrlines.rc
if errorlevel 1 ( echo. & echo [!] wrlines resource compile FAILED & exit /b 1 )

rem wrlines.res is named explicitly: the line globs *.obj and a .res is not one.
rem
rem No /guard:cf here, deliberately, and see the injector below for where it
rem does go. We call MinHook's trampolines -- g_origPresent and friends -- and
rem those live in VirtualAlloc'd PAGE_EXECUTE_READWRITE memory that nobody has
rem registered with SetProcessValidCallTargets. In a process where CFG is
rem enforced, a guarded indirect call to one is an instant __fastfail with no
rem callstack. It would not fire on most machines today, which is exactly the
rem problem: it would fire on a machine with Exploit Protection -> CFG -> on by
rem default, or the day Strata ships a CFG build of the game. Not worth it for a
rem flag no antivirus engine scores.
rem winhttp.lib is the sixth import, and it arrived at v0.6.0 with the
rem leaderboard fetcher. It is called from src\wr_http.cpp and from nowhere
rem else -- the README, HOW TO USE.txt, src\wr_board.h, docs\how-it-works.md
rem and the $expected list in .github\workflows\release.yml all name the same
rem list, and the CI assert fails the build if this line and that one disagree.
link /nologo /DLL /DEBUG /OPT:REF /OPT:ICF /PDBALTPATH:%%_PDB%% ^
     /OUT:wrlines.dll *.obj wrlines.res ^
     user32.lib gdi32.lib shell32.lib dxguid.lib winhttp.lib
if errorlevel 1 ( echo. & echo [!] link FAILED & exit /b 1 )

del wrlines.res >nul 2>&1
del *.obj >nul 2>&1

echo.
echo === wrinject.exe (x64) ===
rem The icon and version block. assets\wrlines.ico is committed, so a normal
rem build needs no Python and no ImageMagick -- assets\make_art.py only has to
rem be run when the artwork itself changes.
if exist assets\wrlines.ico (
    rc /nologo /fo wrinject.res %S%\wrinject.rc
    if errorlevel 1 ( echo. & echo [!] resource compile FAILED & exit /b 1 )
    set "RES=wrinject.res"
) else (
    echo [i] assets\wrlines.ico missing -- building without an icon.
    set "RES="
)

rem This is the file antivirus and SmartScreen actually judge -- nobody
rem double-clicks a DLL -- so it gets everything the DLL cannot have:
rem
rem   /guard:cf      on compile AND link; the link flag is what emits the load
rem                  config table. Safe here where it is not on the DLL: the
rem                  injector makes no indirect calls into memory it allocated.
rem   /Zi /DEBUG     an optimised, statically linked, symbol-free binary is
rem                  about as opaque as a sample can look. A debug directory
rem                  costs nothing and its absence is a packer tell.
rem   /OPT:REF /OPT:ICF   /DEBUG turns both OFF by default, so re-state them or
rem                  the injector quietly grows.
rem   /PDBALTPATH    stops C:\Users\<name>\... being baked into the binary.
rem   the manifest   see src\wrinject.manifest. /MANIFESTUAC:NO is required or
rem                  the linker adds a second, conflicting trustInfo block.
cl /nologo /O2 /MT /W3 /guard:cf /Zi %FP% %DEFS% ^
   %S%\injector.cpp %RES% /Fe:wrinject.exe ^
   /link /SUBSYSTEM:CONSOLE /guard:cf /DEBUG /OPT:REF /OPT:ICF ^
         /PDBALTPATH:%%_PDB%% /MANIFEST:EMBED ^
         /MANIFESTINPUT:%S%\wrinject.manifest /MANIFESTUAC:NO
if errorlevel 1 ( echo. & echo [!] injector build FAILED & exit /b 1 )
del wrinject.res >nul 2>&1

del *.obj >nul 2>&1

echo.
echo === done ===
echo   wrlines.dll   + wrlines.pdb
echo   wrinject.exe  + wrinject.pdb
echo.
echo Sanity checks:
dumpbin /nologo /headers wrlines.dll | findstr /C:"machine"
echo Exports (should be EMPTY -- that is what keeps our ImGui from ever binding
echo to the copy inside the game's devui.dll):
dumpbin /nologo /exports wrlines.dll | findstr /C:"ordinal hint"
echo Version resource (should be NON-empty -- see src\wrlines.rc):
dumpbin /nologo /headers wrlines.dll | findstr /C:"Resource Directory"
echo Imports (the README makes a claim about this list -- keep them in step):
dumpbin /nologo /dependents wrlines.dll | findstr /R /C:"\.dll$"
endlocal
