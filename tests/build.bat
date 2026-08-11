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

rem Finding the toolchain is vcvars.bat's job, shared with build.bat. It used to
rem be two hardcoded BuildTools paths here, which worked on this machine and not
rem on the release runner -- that has VS 2022 Enterprise -- so the v0.7.0 tag
rem built the DLL and then failed on this line. See the essay in vcvars.bat.
call "%~dp0..\vcvars.bat"
if errorlevel 1 exit /b 1

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

rem test_quick links NOTHING, and that is the point. WrQuickDecide is a static
rem inline in wr_quick.h with the rest of this project's pure logic, so the quick
rem menu's state machine can be driven without ImGui, a job slot or a run store.
rem What it is really checking is that a run which cannot be got STOPS being
rem tried and says why -- a chain that silently retries looks exactly like one
rem that is still working, for ever.
cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% tests\test_quick.cpp ^
   /Fe:tests\test_quick.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_quick FAILED to build & exit /b 1 )

rem test_scale links nothing either -- wr_scale.h is static inline like the rest
rem of this project's pure logic. It checks that each LEG of a map gets its own
rem colour range: the old code noticed two legs on screen and pooled them, which
rem turned auto-scaling off without saying so, and pooling legs that sit four
rem thousand units apart leaves each of them a fraction of the ramp.
cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% tests\test_scale.cpp ^
   /Fe:tests\test_scale.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_scale FAILED to build & exit /b 1 )

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

rem test_peek is the one harness here that touches the disk, and it has to: the
rem thing under test is a claim about file metadata, so a stub that returned
rem metadata we invented would be a test of the stub. It writes the synthetic
rem fixture into tests\peekscratch\ and deletes it again. wr_log.cpp comes along
rem for WrDataPath and WrLogf.
cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% /Itests /Ithird_party\lzma ^
   tests\test_peek.cpp ^
   %S%\wr_peek.cpp %S%\wr_mtv.cpp %S%\wr_log.cpp tests\LzmaDec.obj ^
   /Fe:tests\test_peek.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_peek FAILED to build & exit /b 1 )

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

rem test_seam lived here. It was written at P0 and its whole subject was that
rem the typed request produced EXACTLY the command line the nine call sites used
rem to format by hand -- eleven argv strings, byte for byte. It was temporary by
rem design and its own header said so; it went with the python backend, because
rem there is no command line left for it to be about.

rem test_dp is the port's real proof. It links wr_dp.cpp and nothing else,
rem because wr_dp.cpp includes no Windows header and calls nothing -- the same
rem property that lets test_json be checked this hard. What it pins is the two
rem things a .wrpath cannot show you: the exact set of bit positions the scan
rem admits, and a compensated vector norm transcribed from CPython that has to
rem agree with it in the last place.
cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% /Itests tests\test_dp.cpp ^
   %S%\wr_dp.cpp ^
   /Fe:tests\test_dp.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_dp FAILED to build & exit /b 1 )

rem test_wrpath writes a file with the real writer and reads it back with the
rem REAL loader, which nothing did before the port -- LoadOne had no test at
rem all. That round trip is the only thing standing between the two halves of a
rem format whose second implementation has stopped shipping.
cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% /Itests tests\test_wrpath.cpp ^
   %S%\wr_path.cpp %S%\wr_profile.cpp %S%\wr_energy.cpp %S%\wr_log.cpp ^
   /Fe:tests\test_wrpath.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_wrpath FAILED to build & exit /b 1 )

rem test_jobs drives the real pool. Most of it is WrJobsWorkerCount as a table,
rem which needs no threads; the rest genuinely starts them, because "every item
rem runs exactly once" and "two jobs on different deadlines do not share one"
rem are properties of the threading and cannot be restated without it.
cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% /Itests tests\test_jobs.cpp ^
   %S%\wr_jobs.cpp %S%\wr_log.cpp ^
   /Fe:tests\test_jobs.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_jobs FAILED to build & exit /b 1 )

rem test_e2e is the only harness here that owns neither end of anything. It
rem hands WrDemoProcess a whole synthetic .mtv and compares the .wrpath that
rem comes out against the one wrpath_extract.py wrote from the same bytes,
rem committed in tests\fixture_e2e.h. Every other harness tests a layer and so
rem cannot see a seam between two of them; the parity run can, and needs a game
rem install, a demo library and a specific interpreter to do it. This is the
rem part of that comparison small enough to run on every push.
rem
rem It links the whole extraction path -- the container, the codec, the JSON
rem reader, the dynamic program, the writer AND the loader -- because that is
rem exactly the wiring it exists to check.
cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% /Itests /Ithird_party\miniz ^
   /Ithird_party\lzma tests\test_e2e.cpp ^
   %S%\wr_demo.cpp %S%\wr_dp.cpp %S%\wr_mtv.cpp %S%\wr_json.cpp ^
   %S%\wr_path.cpp %S%\wr_profile.cpp %S%\wr_energy.cpp %S%\wr_log.cpp ^
   tests\LzmaDec.obj ^
   /Fe:tests\test_e2e.exe /Fo:tests\ >nul
if errorlevel 1 ( echo [!] test_e2e FAILED to build & exit /b 1 )

rem wrextract.exe is not a harness -- it runs nothing and asserts nothing. It is
rem the console front end over the same functions the DLL calls, and it is how
rem the port's output gets diffed against wrpath_extract.py's. Built here so it
rem cannot rot; never run by this script, and never shipped.
rem
rem It links wr_extract.cpp, which is the whole point: the dispatch, the demo
rem walk, the progress lines and the failure record are the SHIPPED ones. What
rem this file adds is an argv parser and a stdout emit hook. That drags in most
rem of src\ behind it, and that is the right cost -- a front end with its own
rem copy of any of it would agree with itself and say nothing.
cl /nologo /O2 /EHsc /W3 %DEFS% /I%S% /Itests /Ithird_party\miniz ^
   /Ithird_party\lzma ^
   tests\wrextract_main.cpp tests\api_tape.cpp ^
   %S%\wr_extract.cpp %S%\wr_maps.cpp %S%\wr_msml.cpp %S%\wr_json.cpp ^
   %S%\wr_mtv.cpp %S%\wr_peek.cpp %S%\wr_api.cpp %S%\wr_http.cpp %S%\wr_board.cpp ^
   %S%\wr_fetch.cpp %S%\wr_dp.cpp %S%\wr_demo.cpp %S%\wr_jobs.cpp ^
   %S%\wr_path.cpp %S%\wr_profile.cpp %S%\wr_energy.cpp %S%\wr_log.cpp ^
   tests\miniz.obj tests\LzmaDec.obj winhttp.lib ^
   /Fe:tests\wrextract.exe /Fo:tests\ 
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
tests\test_quick.exe      || set "RC=1"
tests\test_scale.exe      || set "RC=1"
tests\test_json.exe       || set "RC=1"
tests\test_maps.exe       || set "RC=1"
tests\test_lzma.exe       || set "RC=1"
tests\test_mtv.exe        || set "RC=1"
tests\test_peek.exe       || set "RC=1"
tests\test_api.exe        || set "RC=1"
tests\test_fetch.exe      || set "RC=1"
tests\test_dp.exe         || set "RC=1"
tests\test_wrpath.exe     || set "RC=1"
tests\test_jobs.exe       || set "RC=1"
tests\test_e2e.exe        || set "RC=1"

if "%RC%"=="1" ( echo. & echo [!] SOME HARNESSES FAILED & exit /b 1 )
echo.
echo === all harnesses passed ===
endlocal
