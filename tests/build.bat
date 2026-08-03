@echo off
rem Build and run the three harnesses.
rem
rem These exist because the regressions this project has actually shipped were
rem all in code with no way to run it outside the game: a matrix that went stale
rem on a map change, a frame cap that double-advanced its phase, an energy
rem reference that re-armed at the apex of a jump, and a teleport that latched
rem the sampler off. Every one of them is a few lines of arithmetic over a
rem trajectory, which is exactly the kind of thing a test can drive.
rem
rem test_energy links the real wr_energy.cpp -- the last defect was the ORDER of
rem two statements in it, which no amount of testing the pure headers would have
rem caught. wr_log.cpp comes along for WrLength/WrDist/WrSaneVec and WrLogf.

setlocal
cd /d "%~dp0\.."

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" (
    echo [!] Could not find vcvarsall.bat. Edit VCVARS at the top of this script.
    exit /b 1
)
call "%VCVARS%" x64 >nul
if errorlevel 1 ( echo [!] vcvarsall x64 failed & exit /b 1 )

set "DEFS=/DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS"

cl /nologo /O2 /EHsc /W3 %DEFS% /I. tests\test_matrixlife.cpp ^
   /Fe:tests\test_matrixlife.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_matrixlife FAILED to build & exit /b 1 )

cl /nologo /O2 /EHsc /W3 %DEFS% /I. tests\test_pacing.cpp ^
   /Fe:tests\test_pacing.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_pacing FAILED to build & exit /b 1 )

cl /nologo /O2 /EHsc /W3 %DEFS% /I. tests\test_energy.cpp wr_energy.cpp wr_log.cpp ^
   /Fe:tests\test_energy.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_energy FAILED to build & exit /b 1 )

del tests\*.obj >nul 2>&1

set "RC=0"
tests\test_matrixlife.exe || set "RC=1"
tests\test_pacing.exe     || set "RC=1"
tests\test_energy.exe     || set "RC=1"

if "%RC%"=="1" ( echo. & echo [!] SOME HARNESSES FAILED & exit /b 1 )
echo.
echo === all harnesses passed ===
endlocal
