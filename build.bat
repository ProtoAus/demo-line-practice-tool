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

setlocal
cd /d "%~dp0"

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" (
    echo [!] Could not find vcvarsall.bat. Edit VCVARS at the top of this script.
    exit /b 1
)

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

call "%VCVARS%" x64 >nul
if errorlevel 1 ( echo [!] vcvarsall x64 failed & exit /b 1 )

if not exist imgui\imgui.cpp (
    echo [!] imgui\ is missing. Fetch it with:
    echo     git clone --depth 1 --branch v1.91.9b https://github.com/ocornut/imgui imgui
    exit /b 1
)
if not exist minhook\src\hook.c (
    echo [!] minhook\ is missing. Fetch it with:
    echo     git clone --depth 1 https://github.com/TsudaKageyu/MinHook minhook
    exit /b 1
)

rem hde64.c, not hde32.c: the target is 64-bit.
set "MH=minhook"
set "MHSRC=%MH%\src\buffer.c %MH%\src\hook.c %MH%\src\trampoline.c %MH%\src\hde\hde64.c"

set "IM=imgui"
set "IMSRC=%IM%\imgui.cpp %IM%\imgui_draw.cpp %IM%\imgui_tables.cpp %IM%\imgui_widgets.cpp"
set "IMSRC=%IMSRC% %IM%\backends\imgui_impl_dx11.cpp %IM%\backends\imgui_impl_win32.cpp"

set "SRC=dllmain.cpp wr_log.cpp wr_pe.cpp wr_probe.cpp wr_engine.cpp wr_scan.cpp"
set "SRC=%SRC% wr_hook.cpp wr_imgui.cpp wr_render.cpp wr_path.cpp wr_ui.cpp"
set "SRC=%SRC% wr_steam.cpp wr_energy.cpp wr_limit.cpp wr_extract.cpp"

set "DEFS=/DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS"
set "DEFS=%DEFS% /DIMGUI_USER_CONFIG=\"../wr_imconfig.h\""

echo.
echo === wrlines.dll (x64) ===
cl /nologo /c /O2 /MT /EHsc /Zi /W3 %DEFS% /I. /I"%MH%\include" /I"%IM%" ^
   %SRC% %IMSRC%
if errorlevel 1 ( echo. & echo [!] compile FAILED & exit /b 1 )

cl /nologo /c /O2 /MT /W3 /I"%MH%\include" %MHSRC%
if errorlevel 1 ( echo. & echo [!] minhook compile FAILED & exit /b 1 )

link /nologo /DLL /DEBUG /OPT:REF /OPT:ICF /OUT:wrlines.dll *.obj ^
     user32.lib gdi32.lib shell32.lib dxguid.lib
if errorlevel 1 ( echo. & echo [!] link FAILED & exit /b 1 )

del *.obj >nul 2>&1

echo.
echo === wrinject.exe (x64) ===
cl /nologo /O2 /MT /W3 %DEFS% injector.cpp /Fe:wrinject.exe /link /SUBSYSTEM:CONSOLE
if errorlevel 1 ( echo. & echo [!] injector build FAILED & exit /b 1 )

del *.obj >nul 2>&1

echo.
echo === done ===
echo   wrlines.dll   + wrlines.pdb
echo   wrinject.exe
echo.
echo Sanity checks:
dumpbin /nologo /headers wrlines.dll | findstr /C:"machine"
echo Exports (should be EMPTY -- that is what keeps our ImGui from ever binding
echo to the copy inside the game's devui.dll):
dumpbin /nologo /exports wrlines.dll | findstr /C:"ordinal hint"
endlocal
