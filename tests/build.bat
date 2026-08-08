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

cl /nologo /O2 /EHsc /W3 %DEFS% /I. tests\test_profile.cpp wr_profile.cpp ^
   wr_energy.cpp wr_log.cpp /Fe:tests\test_profile.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_profile FAILED to build & exit /b 1 )

cl /nologo /O2 /EHsc /W3 %DEFS% /I. tests\test_board.cpp wr_board.cpp ^
   wr_log.cpp /Fe:tests\test_board.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_board FAILED to build & exit /b 1 )

rem test_rank links the real run store, because ranking is a property of what is
rem loaded -- a harness that ranked its own private array would pass while the
rem shipped function looked somewhere else.
cl /nologo /O2 /EHsc /W3 %DEFS% /I. tests\test_rank.cpp wr_path.cpp ^
   wr_profile.cpp wr_energy.cpp wr_log.cpp ^
   /Fe:tests\test_rank.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_rank FAILED to build & exit /b 1 )

rem test_start links the real store and the real zone fit, for the same reason:
rem the pre-roll arithmetic is the thing under test, and a harness that did its
rem own would agree with itself rather than with what ships.
cl /nologo /O2 /EHsc /W3 %DEFS% /I. tests\test_start.cpp wr_path.cpp ^
   wr_start.cpp wr_profile.cpp wr_energy.cpp wr_log.cpp ^
   /Fe:tests\test_start.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_start FAILED to build & exit /b 1 )

rem test_saveloc links the real parser and the real energy sampler. The rules it
rem checks are properties of Momentum's file format and of the order of two
rem blocks in wr_energy.cpp, neither of which a harness could restate without
rem simply agreeing with itself.
cl /nologo /O2 /EHsc /W3 %DEFS% /I. tests\test_saveloc.cpp wr_savelocs.cpp ^
   wr_energy.cpp wr_log.cpp /Fe:tests\test_saveloc.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_saveloc FAILED to build & exit /b 1 )

rem test_live links the real timer, the real recorder and the real save-loc
rem table, because both things it checks are interactions BETWEEN those files
rem rather than properties of any one of them: a restart holding the recorder,
rem and a save-loc load that does not move the camera far enough to be seen as a
rem teleport. Neither can be restated in a harness without restating the bug.
cl /nologo /O2 /EHsc /W3 %DEFS% /I. tests\test_live.cpp wr_timer.cpp ^
   wr_savelocs.cpp wr_path.cpp wr_start.cpp wr_profile.cpp wr_energy.cpp ^
   wr_log.cpp /Fe:tests\test_live.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_live FAILED to build & exit /b 1 )

rem test_settings links the real table and the real settings structs. A harness
rem with its own private table would agree with itself and say nothing about
rem what ships -- and "one field silently stopped persisting" is precisely the
rem failure that has no other way of being noticed.
rem wr_render.cpp is deliberately NOT here: it is the draw layer and pulls in
rem ImGui, Steam and the engine. Only its DEFAULTS are stubbed -- the struct is
rem the real one, so the table is still checked against the real fields.
cl /nologo /O2 /EHsc /W3 %DEFS% /I. tests\test_settings.cpp wr_settings.cpp ^
   wr_energy.cpp wr_start.cpp wr_limit.cpp wr_profile.cpp ^
   wr_path.cpp wr_log.cpp user32.lib /Fe:tests\test_settings.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_settings FAILED to build & exit /b 1 )

del tests\*.obj >nul 2>&1

set "RC=0"
tests\test_matrixlife.exe || set "RC=1"
tests\test_pacing.exe     || set "RC=1"
tests\test_energy.exe     || set "RC=1"
tests\test_profile.exe    || set "RC=1"
tests\test_board.exe      || set "RC=1"
tests\test_rank.exe       || set "RC=1"
tests\test_start.exe      || set "RC=1"
tests\test_saveloc.exe    || set "RC=1"
tests\test_live.exe       || set "RC=1"
tests\test_settings.exe   || set "RC=1"

if "%RC%"=="1" ( echo. & echo [!] SOME HARNESSES FAILED & exit /b 1 )
echo.
echo === all harnesses passed ===
endlocal
