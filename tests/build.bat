@echo off
rem Build and run the harnesses.
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
rem
rem This runs from the repo root, so our sources are under src\ and the harnesses
rem under tests\. /I%S% is what lets every harness say #include "wr_foo.h" with
rem no path in it.

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
set "S=src"

cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% tests\test_matrixlife.cpp ^
   /Fe:tests\test_matrixlife.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_matrixlife FAILED to build & exit /b 1 )

cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% tests\test_pacing.cpp ^
   /Fe:tests\test_pacing.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_pacing FAILED to build & exit /b 1 )

cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% tests\test_energy.cpp ^
   %S%\wr_energy.cpp %S%\wr_log.cpp ^
   /Fe:tests\test_energy.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_energy FAILED to build & exit /b 1 )

cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% tests\test_profile.cpp ^
   %S%\wr_profile.cpp %S%\wr_energy.cpp %S%\wr_log.cpp ^
   /Fe:tests\test_profile.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_profile FAILED to build & exit /b 1 )

cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% tests\test_board.cpp ^
   %S%\wr_board.cpp %S%\wr_log.cpp ^
   /Fe:tests\test_board.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_board FAILED to build & exit /b 1 )


rem test_rank links the real run store, because ranking is a property of what is
rem loaded -- a harness that ranked its own private array would pass while the
rem shipped function looked somewhere else.
cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% tests\test_rank.cpp ^
   %S%\wr_path.cpp %S%\wr_profile.cpp %S%\wr_energy.cpp %S%\wr_log.cpp ^
   /Fe:tests\test_rank.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_rank FAILED to build & exit /b 1 )

rem test_start links the real store and the real zone fit, for the same reason:
rem the pre-roll arithmetic is the thing under test, and a harness that did its
rem own would agree with itself rather than with what ships.
cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% tests\test_start.cpp ^
   %S%\wr_path.cpp %S%\wr_start.cpp %S%\wr_profile.cpp %S%\wr_energy.cpp ^
   %S%\wr_log.cpp ^
   /Fe:tests\test_start.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_start FAILED to build & exit /b 1 )

rem test_saveloc links the real parser and the real energy sampler. The rules it
rem checks are properties of Momentum's file format and of the order of two
rem blocks in wr_energy.cpp, neither of which a harness could restate without
rem simply agreeing with itself.
cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% tests\test_saveloc.cpp ^
   %S%\wr_savelocs.cpp %S%\wr_energy.cpp %S%\wr_log.cpp ^
   /Fe:tests\test_saveloc.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_saveloc FAILED to build & exit /b 1 )

rem test_live links the real timer, the real recorder and the real save-loc
rem table, because both things it checks are interactions BETWEEN those files
rem rather than properties of any one of them: a restart holding the recorder,
rem and a save-loc load that does not move the camera far enough to be seen as a
rem teleport. Neither can be restated in a harness without restating the bug.
cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% tests\test_live.cpp ^
   %S%\wr_timer.cpp %S%\wr_savelocs.cpp %S%\wr_path.cpp %S%\wr_start.cpp ^
   %S%\wr_profile.cpp %S%\wr_energy.cpp %S%\wr_log.cpp ^
   /Fe:tests\test_live.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_live FAILED to build & exit /b 1 )

rem test_settings links the real table and the real settings structs. A harness
rem with its own private table would agree with itself and say nothing about
rem what ships -- and "one field silently stopped persisting" is precisely the
rem failure that has no other way of being noticed.
rem wr_render.cpp is deliberately NOT here: it is the draw layer and pulls in
rem ImGui, Steam and the engine. Only its DEFAULTS are stubbed -- the struct is
rem the real one, so the table is still checked against the real fields.
cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% tests\test_settings.cpp ^
   %S%\wr_settings.cpp %S%\wr_energy.cpp %S%\wr_start.cpp %S%\wr_limit.cpp ^
   %S%\wr_profile.cpp %S%\wr_path.cpp %S%\wr_log.cpp user32.lib ^
   /Fe:tests\test_settings.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_settings FAILED to build & exit /b 1 )

rem test_json needs nothing but itself: wr_json.cpp includes no Windows header
rem and calls nothing, which is the property that lets it be checked this hard.
cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% tests\test_json.cpp ^
   %S%\wr_json.cpp ^
   /Fe:tests\test_json.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_json FAILED to build & exit /b 1 )

rem test_maps links the real writer and the real reader and the real inflate,
rem so what it checks is those three agreeing with each other and with the
rem reference implementation's output -- which is the whole claim of this phase.
rem miniz comes in as C, with the same options build.bat gives it.
cl /nologo /c /O2 /W3 /DMINIZ_NO_ARCHIVE_APIS /DMINIZ_NO_DEFLATE_APIS ^
   /DMINIZ_NO_STDIO /DMINIZ_NO_TIME /DMINIZ_NO_ZLIB_COMPATIBLE_NAMES ^
   /Ithird_party\miniz third_party\miniz\miniz.c /Fo:tests\ >nul
if errorlevel 1 ( echo [!] miniz FAILED to build for the harnesses & exit /b 1 )

cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% /Itests /Ithird_party\miniz ^
   tests\test_maps.cpp ^
   %S%\wr_maps.cpp %S%\wr_msml.cpp %S%\wr_json.cpp %S%\wr_log.cpp tests\miniz.obj ^
   /Fe:tests\test_maps.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_maps FAILED to build & exit /b 1 )

rem The LZMA decoder, shared by the three harnesses below and by wrextract, the
rem same way miniz.obj is. No options: third_party\VERSION.txt says why the
rem obvious _7ZIP_ST is not one of them.
cl /nologo /c /O2 /W3 /Ithird_party\lzma third_party\lzma\LzmaDec.c ^
   /Fo:tests\ >nul
if errorlevel 1 ( echo [!] LzmaDec FAILED to build for the harnesses & exit /b 1 )

rem test_lzma checks how wr_mtv.cpp CALLS the decoder, not the decoder. See its
rem header: LZMA_FINISH_ANY, the property bytes passed straight through, and a
rem dictionary size that is not something to clamp are three decisions that
rem could each have gone the other way, and each has a section.
cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% /Itests /Ithird_party\lzma ^
   tests\test_lzma.cpp ^
   %S%\wr_mtv.cpp tests\LzmaDec.obj ^
   /Fe:tests\test_lzma.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_lzma FAILED to build & exit /b 1 )

rem test_mtv is mostly about refusal, and about the exact words used to refuse.
rem Those words end up in _failed.txt, which both implementations read back to
rem decide whether a demo is worth trying again -- so a reason string that
rem differs from the reference's is a record that reads as a different failure.
cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% /Itests /Ithird_party\lzma ^
   tests\test_mtv.cpp ^
   %S%\wr_mtv.cpp tests\LzmaDec.obj ^
   /Fe:tests\test_mtv.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_mtv FAILED to build & exit /b 1 )

rem test_api links the real API layer AND the real network client, and then
rem never reaches the network -- api_tape.cpp answers every request from a
rem checked-in recording, and a URL that is not in it is an error rather than a
rem fetch. Linking wr_http.cpp anyway is deliberate: it means a request that
rem escaped the tape would be a real request, and the harness asserts the count
rem so it could not escape unnoticed.
rem
rem The recording under tests\fixtures\api is SYNTHETIC. A real one would be a
rem hundred strangers' names and SteamID64s committed to a public repository,
rem which is the same reason wrlines_data is gitignored at any depth. Parity
rem against the live API is a local pre-release step; see tests\parity.ps1.
cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% /Itests /Ithird_party\miniz ^
   tests\test_api.cpp tests\api_tape.cpp ^
   %S%\wr_api.cpp %S%\wr_http.cpp %S%\wr_board.cpp %S%\wr_msml.cpp ^
   %S%\wr_json.cpp %S%\wr_log.cpp tests\miniz.obj winhttp.lib ^
   /Fe:tests\test_api.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_api FAILED to build & exit /b 1 )

rem test_fetch is mostly about a selection a USER TYPED and about one timestamp
rem whose failure has no symptom at all -- see the harness header. It links the
rem real wr_fetch, which drags in the API layer and therefore the network
rem client; nothing in it makes a request.
cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% /Itests /Ithird_party\miniz ^
   tests\test_fetch.cpp ^
   %S%\wr_fetch.cpp %S%\wr_api.cpp %S%\wr_http.cpp %S%\wr_board.cpp ^
   %S%\wr_msml.cpp %S%\wr_json.cpp %S%\wr_log.cpp tests\miniz.obj winhttp.lib ^
   /Fe:tests\test_fetch.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_fetch FAILED to build & exit /b 1 )

rem test_seam links the real launcher, because the whole claim it checks is that
rem the request struct turns back into the command line the call sites used to
rem build by hand. A harness with its own copy of the formatting would agree
rem with itself and say nothing about what ships.
rem
rem TEMPORARY. It goes with the python backend, once every verb is C.
rem
rem The link list grows with the port: wr_extract.cpp is the slot every verb is
rem dispatched from, so it pulls in whatever has been ported so far. That is the
rem cost of there being one slot, and it is the right cost.
cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% /Ithird_party\miniz /Ithird_party\lzma ^
   tests\test_seam.cpp ^
   %S%\wr_extract.cpp %S%\wr_maps.cpp %S%\wr_msml.cpp %S%\wr_json.cpp ^
   %S%\wr_mtv.cpp %S%\wr_api.cpp %S%\wr_http.cpp %S%\wr_board.cpp ^
   %S%\wr_fetch.cpp %S%\wr_log.cpp tests\miniz.obj tests\LzmaDec.obj ^
   winhttp.lib ^
   /Fe:tests\test_seam.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_seam FAILED to build & exit /b 1 )

rem wrextract.exe is not a harness -- it runs nothing and asserts nothing. It is
rem the console front end over the same functions the DLL calls, and it is how
rem the port's output gets diffed against wrpath_extract.py's. Built here so it
rem cannot rot; never run by this script, and never shipped.
cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% /Itests /Ithird_party\miniz ^
   /Ithird_party\lzma ^
   tests\wrextract_main.cpp tests\api_tape.cpp ^
   %S%\wr_maps.cpp %S%\wr_msml.cpp %S%\wr_json.cpp %S%\wr_mtv.cpp ^
   %S%\wr_api.cpp %S%\wr_http.cpp %S%\wr_board.cpp %S%\wr_fetch.cpp ^
   %S%\wr_log.cpp tests\miniz.obj tests\LzmaDec.obj winhttp.lib ^
   /Fe:tests\wrextract.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] wrextract FAILED to build & exit /b 1 )

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
tests\test_json.exe       || set "RC=1"
tests\test_maps.exe       || set "RC=1"
tests\test_lzma.exe       || set "RC=1"
tests\test_mtv.exe        || set "RC=1"
tests\test_api.exe        || set "RC=1"
tests\test_fetch.exe      || set "RC=1"
tests\test_seam.exe       || set "RC=1"

if "%RC%"=="1" ( echo. & echo [!] SOME HARNESSES FAILED & exit /b 1 )
echo.
echo === all harnesses passed ===
endlocal
