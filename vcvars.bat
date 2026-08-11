@echo off
rem vcvars.bat  --  find MSVC and put x64 on PATH. One place, called by both.
rem
rem This exists because the same drift that wr_version.h was written to stop
rem happened again, in the build. build.bat learned to find the toolchain with
rem vswhere at v0.4.6; tests\build.bat kept the two hardcoded BuildTools paths
rem that used to be all there was. On this machine both work, because BuildTools
rem is installed. On the GitHub Actions runner only one of them does -- the
rem runner has VS 2022 ENTERPRISE -- so the release workflow built the DLL and
rem then failed on the very next step, at the v0.7.0 tag, for a reason that had
rem nothing to do with anything in that release. Two copies of a lookup, one of
rem them updated: it is worth a third file to make that unrepresentable.
rem
rem Order: an explicit override first, then vswhere, then the two hardcoded
rem paths. vswhere is installed by every VS 2017+ setup and lives at a fixed
rem location, so it finds Community, Professional and Enterprise as well as
rem BuildTools. The hardcoded paths stay as a fallback so a machine that built
rem yesterday still builds today.
rem
rem NO setlocal, on purpose: the whole point is to leave the compiler on the
rem CALLER'S PATH, and setlocal would throw that away at the return. And no
rem delayed expansion either -- tests\build.bat prints 26 messages that start
rem with [!], and enabling it there would eat every one of those exclamation
rem marks. So VSWHERE is set on its own line at the top level and read with
rem ordinary %% expansion rather than being read inside the block that sets it.
rem
rem Call it, then check: it exits 1 having said why.
rem
rem     call "%~dp0vcvars.bat"
rem     if errorlevel 1 exit /b 1

set "VCVARS="
if defined WRLINES_VCVARS set "VCVARS=%WRLINES_VCVARS%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined VCVARS if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (
    `"%VSWHERE%" -latest -products * ^
       -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
       -property installationPath`
) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvarsall.bat"

if not defined VCVARS set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" (
    echo [!] Could not find vcvarsall.bat. Set WRLINES_VCVARS to its full path.
    exit /b 1
)

call "%VCVARS%" x64 >nul
if errorlevel 1 ( echo [!] vcvarsall x64 failed & exit /b 1 )
exit /b 0
