# How it works

The long version: what is measured, what is approximate, and the things that were tried and thrown
away. **If you just want to use the tool, [the README](../README.md) is a page long and has
everything you need.**

---

Draws the path other players' runs took as a line in the world, in the Momentum Mod map you
are currently playing. Follow the line instead of memorising a replay.

It reads the `.mtv` demos the game has **already downloaded** for you — and, if you ask it to,
fetches more from Momentum's public leaderboard — and turns them into a route you can see while
you surf, with
names and avatars on the lines, speeds at the bottom of every ramp, and a live energy readout
that tells you where you are wasting momentum.

Two halves, and **as of v0.7.0 both of them live in `wrlines.dll`**:

| | |
| --- | --- |
| offline | Reads the `.mtv` demos on disk and writes small `.wrpath` files, on a pool of background threads. Started from a button; never automatic. |
| in-game | Reads `.wrpath`, draws the lines, and gives you a panel on **INSERT**. |

That was two programs until v0.7.0: the offline half was `wrpath_extract.py`, shipped beside the
DLL and run through whatever Python you had. It went in five releases, one verb at a time — the
map index at v0.5.0, the container at v0.5.1, the leaderboard at v0.6.0, downloading at v0.6.1
and extraction at v0.7.0. The script survives in the tree under `tests/reference/` as the
oracle the port is checked against, and is not in the download.

> **Naming:** the project is *Demo Line Practice Tool*; the binary and the source files kept
> their working name, `wrlines`. Nothing depends on that — it is just what the files were
> called while it was being written.

**Requires:** Windows x64, Momentum Mod Playtest (Strata Source), and D3D11 or DXVK. Nothing
else — no interpreter, no runtime, no installer. There is no 32-bit path anywhere in the game,
so both binaries are x64.
**Linux works, through Proton** — see [Linux](#linux) below for why that is the answer rather
than a native build.

---

## Quick start

```
git clone --depth 1 --branch v1.91.9b https://github.com/ocornut/imgui imgui
git clone --depth 1 https://github.com/TsudaKageyu/MinHook minhook
build.bat

REM launch the game, load the map, then:
wrinject.exe
```

Press **INSERT** in game. Tick runs in the Runs tab. **ESC** closes the panel.

Four more keys work without opening it, all rebindable and all listed on the **About** tab as they
are currently bound:

| key | |
| --- | --- |
| `Page Down` / `Page Up` | next / previous mode on the box at your crosshair |
| `Home` | *whose line am I looking at* — off by default |
| `End` | the corner block — off by default |

They are **read, never swallowed**, so a collision means the game still acts on the key.

---

## What it does

- Finds every downloaded run for the map you are on and lists them by time.
- Draws any number of them at once, each in its own colour, so you can compare routes.
- **Name and avatar tags on the lines**, so with a dozen runs enabled you can tell whose
  route is whose. Anchored where each line crosses the fade distance, so they spread along
  different lines instead of piling up, and de-overlapped rather than drawn on top of
  each other.
- **Numbers at every turning point** — energy at the bottom of each ramp and at the top of each
  arc by default, in each run's own colour, and speed, time or your delta instead if you would
  rather. A bottom says what a line carried *through* the ramp; a top says what it bought with it,
  and the **difference between the two is what the arc cost** — which is why both ends read energy
  rather than speed. Nothing from the two seconds of walking into the start zone is labelled, so no
  line grows a number where it has not begun.
- **Colour every run by where it placed** — violet for the winner, then green through red for
  everyone behind it. Only first is held out of the ramp: a gold/silver/bronze podium was tried
  and all three medals are warm mid-brightness colours sitting inside a ramp that already runs
  green to amber to red, so second and third vanished into the field. Violet is the one hue the
  ramp never reaches. Placed **within each leg**, because a map's files hold separate runs per
  stage and per bonus and the quickest time in the folder is usually a stage, not the main track.
- **A ring on the point being compared.** Every gap on screen is your energy against theirs at one
  point of their line — the point nearest you, re-picked each frame — and it is now drawn, in that
  run's colour, so you can see which point and which line the number came from.
- **A leaderboard you can scroll and sort**, and download any part of — including the slow end,
  because the fastest runs are the hardest to follow. Two requests reach the bottom of a
  9,000-run board.
- **Energy across a whole run, plotted** — the Graphs tab draws every enabled run's energy
  against distance or time, your own line included, with a hover that reads all of them at once.
  A line that sags gently leaked everywhere; a line with one cliff in it lost the lot at one
  ramp, and only one of those is worth practising the same way.
- **Energy**, beside the crosshair where you can read it mid-ramp: how high you could still
  get if you redirected everything you have straight up, measured from the last ground you
  stood on — 0 standing still, and the map's own descent cancels out, so what is left is the
  energy you have wasted or gained. Shown as a height, as the speed that height is worth, with
  a rise/fall arrow, and as a gap against the record's energy at the same point on the route.
  A **fixed width**, so the box and the lean bar under it stay the same size whoever you are
  nearest — the compared player's name used to set the width, which meant the bar's scale
  depended on how long somebody's Steam name was. `Page Down` and `Page Up` change what it shows
  without opening the panel.
- **A live strafe gauge**, green to red, on how close you are to the most energy air strafing
  could physically add. The same metric that colours the demo lines, and deliberately slow: your
  own velocity is estimated from camera positions, and at a 0.4 s window a reading taken that way
  points the *wrong way* a quarter of the time. At 2 seconds it is 8.5%. See **A live strafe
  gauge, and why it is slow**.
- **Settings that survive a restart**, in `wrlines_data\settings.cfg` — including the panel's own
  window position. Display settings only, and nothing in it identifies anybody.
- Puts **split markers** on the line where each checkpoint was reached, labelled with
  the split time.
- **Colour along each line** by speed, by **energy**, or by strafing efficiency — one at a
  time, because a line has one colour. Energy is height plus speed squared over 2g: what the
  run actually had to spend, whether it was holding it as altitude or as pace. It is an
  absolute figure, so the same colour is the same energy on every line and two runs can be
  read against each other directly; a button fits the range to the map.
- **Aim at a line and it tells you whose it is** — the line thickens, a ring marks the point
  you are pointing at, and a plate gives the name, the track, the placing, and what that run
  was carrying right there. It uses the crosshair rather than the mouse, because there is no
  mouse while you are playing. Two honest limits, both said in the tooltip: lines draw through
  walls, so one behind the ramp you are stood on is just as pickable as one in front of it; and
  two runs that sit within a few pixels of each other for the whole visible stretch cannot be
  separated by aiming, so the plate says how many others were equally close instead of picking
  one and sounding sure. About 0.3 ms a frame with 256 lines drawn, against the 8 ms drawing
  them already costs, and Diagnostics shows the real figure rather than that estimate. The
  runner's Steam avatar sits beside their name, and the plate keeps a set distance from the point
  it is describing — measured to its **edge**, which is the fix for it having sat on top of what
  you were aiming at: the offset used to be applied to the box's centre, so the clearance you
  actually got shrank as rows were added to it.
- **Lines start where the run starts.** A demo begins recording before the run does — measured
  over 500 demo headers here, the recording runs a median of **2.06 seconds** longer than the
  run, and about three quarters of a second of that survives on the extracted line. That
  approach is what made a replay look like it began somewhere odd, and it was also the graph's
  zero: the energy origin and both axes came from a point where the run had not started. See
  **Where a run starts** below for how it is recovered and how often that works.
- **It notices when you leave the start.** Not by reading the mapper's zone — it cannot see one
  — but by fitting a start to where a few hundred loaded runs actually began, which is a better
  sample and comes with a measured spread. The circle, the way out and the trigger line are
  drawn in the world with the uncertainty band to scale, so it is visible guesswork rather than
  magic. Details under **The start zone**.
- **Colour lines by energy**, either as an absolute world height — where the same colour means the
  same energy on every line, so two runs read against each other directly — or **against each run's
  own start**, where every line reads zero where it began and the colours compare margins rather
  than energies. The second is the scale the crosshair readout uses, which is why a range like
  −100 to 500 makes sense there and would be meaningless on the first.
- **The run clock on screen**, optionally, beside the crosshair rather than only in a panel you close
  before you play — which is why loading a save-loc appeared to restore nothing. It goes green for a
  moment when a save-loc puts it back. The readout block can also be centred on the crosshair or
  pinned above or below it, so it stops creeping as rows come and go.
- **Search and filter the run list**, by player, by their current Steam name, or by track. One
  button per leg turns all of *bonus 4* on at once, and **All**/**None** work on whatever the
  filter left rather than on the whole store. The map list and the leaderboard each have a
  **This map** button that jumps to whichever map you are standing in — on the map list it
  toggles, and it reaches maps you hold nothing for, which the unfiltered list hides.
- **Send one demo to the game's replay viewer.** Momentum lists ten, and it finds them by
  scanning its own folder, so choosing which ten is the whole workaround. Removal only ever
  touches files this put there — see **Watching what you download**.
- Records **your own path** live, so you can see your line next to theirs.
- Distance culling, distance fade, screen-space decimation and per-run point budgets, so
  eight full runs on screen cost a fraction of a millisecond.
- Holds up to **1000 runs** per map and draws **256** by default. The old cap of 256 was sized
  against a demo library the game had downloaded by itself, where the busiest of 475 maps had
  221 runs; the leaderboard tab made that measurement obsolete, since you can now sort a board
  and fetch as much of it as you like. Files load a few per frame, so a big map fills in behind
  you rather than stalling.
- The draw cap is **not** a free knob, and the point budget does not bound it — that budget is
  per run, so the work is the budget times the number of lines, and distance culling is the
  only thing that caps the total. On a compact stage where culling rejects nothing: 8 lines
  cost 0.24 ms a frame, 256 cost about 8 ms, 1000 cost 32 ms. It only bites once that many runs
  are actually *enabled*, which takes a deliberate **All** — out of the box a map draws one
  line. The slider says all of this where you can read it.
- Stops drawing when you leave a map. Disconnecting does not clear the camera matrix or stop
  it looking valid — it just stops being written — so without this the whole route stayed
  drawn over the main menu, frozen in place. That rule is suspended while the panel is open,
  since using the panel means standing still and the lines you are ticking on and off are the
  entire point; the suspension is only taken if the matrix was live shortly before you opened
  it, so opening the panel at the main menu still draws nothing.

## What it deliberately doesn't do

- **It does not use the engine's debug overlay.** `IVDebugOverlay::AddLineOverlay` would
  give depth-correct lines for almost no code, but it is gated behind
  `enable_debug_overlays`, which is `FCVAR_CHEAT`. In a speedrunning game anything that
  drags `sv_cheats` into the picture taints your runs. This projects and draws the lines
  itself instead.
- **Lines have no depth test.** They draw through walls. That is the price of the point
  above, and for a route tool it is arguably what you want anyway.
- **It never unloads.** No unhooking, no `FreeLibrary`. Tearing down a `Present` hook
  while another thread is inside the trampoline is the classic injected-DLL
  crash-on-exit. Restart the game to load a rebuilt DLL; the injector refuses to inject
  twice for the same reason.
- **It writes nothing into the game install unless you turn one thing on**, sets no cvars,
  **runs no console commands**, and never touches `sv_cheats`. Everything it writes lives in
  `wrlines_data\` next to the DLL — with a single exception you have to press for: *send*,
  *local* and *watch* each copy one demo into the game's own replay folder so the game can play
  it. Every copy is recorded in `wrlines_data\into_game.txt` first and nothing outside that list
  can be deleted from the panel. Described under **Watching what you download** below. The
  **watch** button puts a console command on your clipboard for you to paste; WrLines does not
  execute it.
- **In its default configuration it makes zero calls into the game.** It hooks `Present` to
  draw, reads memory read-only, and reads two files.
- **It reaches outside your machine in exactly two places, both behind a checkbox, both off
  unless you say otherwise.** Name/avatar tags ask the Steam client to look up each runner's
  persona and picture — the same request a scoreboard makes. And the Maps tab can download
  demos you do not have from Momentum's public leaderboard. With both off, nothing leaves the
  process; tags fall back to the name already stored in the `.wrpath` and a coloured dot.
  See **Downloading demos** below for what the second one does and does not do.
- **It does not decode the demo netstream properly.** See below — the extractor is a
  pattern-matcher, and it tells you when it is unsure rather than guessing quietly.

## Frame cap

There is a frame limiter in the **Frame cap** tab, off by default. It exists so you do not
have to run a second overlay purely for a cap — which turned out to cost more than the cap was
worth, because two tools drawing into the same swapchain is a fight neither wins.

It reads the refresh rate of whichever monitor the game window is on and defaults to 3 Hz below
it, the usual variable-refresh recommendation. The wait is a high-resolution waitable timer for
the bulk of the frame and a short busy-wait for the tail.

Measured on a harness that drives the real limiter with simulated frame work, 600 frames per
row:

| | target | mean | σ | worst deviation | spin |
| --- | --- | --- | --- | --- | --- |
| 160 fps, 0.35 ms spin (default) | 6.250 ms | 6.250 | 0.085 | **0.216 ms** | 1.3 % |
| 160 fps, timer only, no spin | 6.250 ms | 6.250 | 0.247 | 0.453 ms | 0 % |
| 160 fps, 1.0 ms spin | 6.250 ms | 6.250 | 0.003 | 0.060 ms | 9.4 % |
| 240 fps, 0.35 ms spin | 4.167 ms | 4.166 | 0.083 | 0.241 ms | 1.8 % |

The spin window earns its keep — without it the worst deviation doubles — and widening it past
the default buys accuracy nobody can see for seven times the CPU.

Two behaviours worth knowing about:

- **The schedule is absolute, and it never catches up.** Targets advance from the previous
  target, so ordinary jitter cancels instead of accumulating and the long-run cadence is exact
  to a microsecond. But a frame that overruns leaves the schedule in debt, and repaying that
  debt means releasing the next frame *early* — which on a variable-refresh display is not a
  correction, it is a second artefact, because the panel refreshes when the frame arrives. So
  a late frame is absorbed and the schedule resynchronises from where it landed.
- **It cannot invent performance.** A cap above what the machine sustains does nothing; the tab
  detects that and says so rather than showing a target it is not meeting.

### What the catch-up rule cost

The first version repaid the debt at up to a quarter of a frame each, which sounded modest and
was not. Driving the schedule with a wobbling workload and a 6 ms spike every 37th frame,
against a 240 fps target:

| after a spike | repay a quarter-period | no catch-up |
| --- | --- | --- |
| shortest interval | 3.125 ms — **released early** | 4.167 ms |
| worst step between consecutive frames | 1.92× | **1.44×** |
| intervals more than 10 % off target | 113 / 1999 | **54 / 1999** |

One overrunning frame was being turned into *two* wrong intervals instead of one. The schedule
arithmetic is `wr_pacing.h`, kept free of Windows and D3D so it can be driven by a script of
frame timings — `tests/test_pacing.cpp`, which also runs the old policy alongside the new one,
because "this is better" should be a number.

That harness immediately found a second bug it was not looking for: the first frame of every
session set the phase a period ahead *and* then advanced it again, so the second frame of every
session waited two full periods — an 8.3 ms interval against a 4.2 ms target.

### The cap has to suit the panel

The tab now shows refresh ÷ cap, because a cap that is not a whole fraction of the refresh rate
cannot be displayed evenly unless variable refresh is genuinely engaged. **240 fps on a 360 Hz
panel is 1.5 refreshes per frame** — the panel has to alternate between holding a frame for one
refresh and for two, so frames land 2.8 ms, 5.6 ms, 2.8 ms, 5.6 ms. The average is exactly 240
and it looks nothing like 240. When the division is not whole, the tab says so and offers the
ones that are.

### Where the wait happens

At the **top** of the Present hook, before the camera matrix is read and before anything is
drawn. It used to be at the bottom: read the matrix, draw the lines, hold the frame for up to a
millisecond, then present — which left the lines a whole wait older than the frame they landed
on, and only when the cap was on. That is a one-line ordering change and it removes the entire
wait from the matrix-to-present latency.

None of it is derived from any other limiter's source. Frame pacing is standard technique, and
reproducing a GPL-licensed implementation would have relicensed this MIT project by accident.

### Living with other overlays

SpecialK, RTSS and the Steam overlay all hook `Present` too, and a second hook in that chain
is a real thing to be careful with — a frame limiter that paces itself inside its own Present
hook will oscillate if the work below it varies frame to frame. Three deliberate choices:

- **Nothing to draw means nothing happens.** When no runs are ticked, the panel is shut and
  the energy readout is off — and always at menus and loading screens — `Present` binds no
  render target, builds no ImGui frame and touches no device state. Diagnostics counts the
  frames it skipped, so you can confirm it.
- **The render target is put back.** What was bound before we draw is saved and restored.
  ImGui's own state backup happens *after* we bind ours, so without this it faithfully
  restores the wrong thing and whatever draws next in the same frame inherits our target.
- **The window procedure is only replaced while the panel is open**, and restored on close —
  and if something else subclassed on top of us in the meantime, we leave the chain alone
  rather than cut it out, and say so in the log.

The vtable read at startup uses a WARP (software) device rather than a hardware one. All it
needs is the layout, which belongs to the loaded `d3d11.dll`, and it avoids showing other
overlays a phantom swapchain to reason about.

With the energy readout on — which is the default — a small, *constant* amount of work happens
every frame. Turning it off in the Energy tab makes `Present` a complete passthrough.

### Fair play

This is a client-side visualisation tool. It shows you a route that is already public in the
demos the game handed you, it does not modify the game, and it gives you no capability the
game does not — no aim assistance, no timer interaction, no movement input, no memory writes
of any kind.

That is still a different question from whether Momentum's own rules consider an **injected
DLL** acceptable alongside submitted runs. Check that before using it on anything you intend
to submit. Treat it as a practice tool.

---

## Settings, and where they live

Until 0.4.4 nothing was persisted at all: four `*Defaults()` calls ran once at startup and that was
the whole story, so every restart of the game was a fresh install. Settings now live in
`wrlines_data\settings.cfg`, written a couple of seconds after you stop changing something — so a
slider being dragged writes once when you let go rather than on every frame of the drag.

**One table, populated by the file that owns each variable.** Every persisted field is registered
once, next to its declaration, and the writer and the reader both walk that list. A serialiser
written out by hand over a hundred-odd fields goes out of step with the struct the first time
somebody adds a field and edits only one of the two halves; there is only one half here, and adding
a setting is one line. `tests\test_settings.cpp` round-trips every registered field against the real
structs, because *one setting silently stopped persisting* is a failure with no other way of being
noticed.

- **An unknown key is ignored and a missing key keeps its default**, so an old file loads into a new
  build and a new file into an old one. There is no version number to get wrong.
- **Every value is clamped to the range its own slider has**, on read. It is a text file people will
  edit, so it is an untrusted input — including `NaN`, which fails every comparison and would
  otherwise propagate into the geometry and draw nothing at all, silently.
- **Reset everything** restores a snapshot taken at registration, so it reaches every field and not
  only the ones that happen to live in a struct with a `Defaults()` function.
- **Display settings only.** No names, no SteamID64s, no map or run data, no paths, no record of
  what you watched. It is safe to paste into a bug report — unlike the rest of `wrlines_data\`,
  which is gitignored precisely because it is not.

The panel's own position and size are saved beside it, in `wrlines_data\imgui.ini`. ImGui's default
is a bare `imgui.ini` relative to the working directory, which for an injected DLL is the game's and
belongs to the game's own devui — so that was set to `NULL`, and the cost was the panel forgetting
where it was every session. An absolute path under our folder keeps the promise and fixes the
forgetting.

## Build

Needs MSVC Build Tools (2022 or 2026) and the Windows SDK. `build.bat` finds `vcvarsall.bat`
on its own; if yours lives somewhere unusual, edit `VCVARS` at the top of the script.

```
build.bat
```

Produces `wrlines.dll`, `wrlines.pdb` and `wrinject.exe`, all x64.

**Close the game before rebuilding.** The DLL never unloads, so while Momentum is
running the loaded copy cannot be overwritten. `build.bat` checks for this and says so
rather than letting the linker fail with a bare `LNK1104`.

MSVC rather than mingw on purpose: this calls MSVC-laid-out C++ virtual interfaces in the
engine, and several Source methods return structs by value where mingw's Itanium sret
convention does not match MSVC's.

The build links `dxguid.lib` but **not** `d3d11.lib` or `dxgi.lib`. Nothing calls an
exported D3D11/DXGI function — the swapchain vtable is read via `GetModuleHandle` on
whichever `d3d11.dll` the game already loaded. That is what makes this work identically
under native D3D11 and under DXVK. `dumpbin /dependents wrlines.dll` should show no
graphics DLLs, and `dumpbin /exports` should show none at all (that is what stops the
bundled ImGui ever binding to the copy inside the game's `devui.dll`).

### Third-party dependencies

Four, split into two groups by a rule worth stating: **anything on the path that decides what a
run path contains is committed; anything that draws a panel is cloned.** A decompressor that is
subtly the wrong version does not fail loudly — it produces a different byte somewhere in a run
and the line in the world bends. Git is the pin for those.

Cloned into the source folder before building, and pinned by tag because the release workflow
records the commit:

```
git clone --depth 1 --branch v1.91.9b https://github.com/ocornut/imgui imgui
git clone --depth 1 --branch v1.3.3   https://github.com/TsudaKageyu/MinHook minhook
```

- **Dear ImGui** (MIT) — left exactly as cloned; only `imgui{,_draw,_tables,_widgets}.cpp`
  and the dx11 + win32 backends are compiled. Build settings live in `wr_imconfig.h`, pulled
  in with `/DIMGUI_USER_CONFIG`, so the vendored tree is never edited.
- **MinHook** (BSD-2-Clause) — built with `hde64.c` rather than `hde32.c`, since the target
  is 64-bit.

Committed under [third_party/](../third_party/), unmodified, with
[VERSION.txt](../third_party/VERSION.txt) recording the upstream tag, the release archive's
SHA-256, the SHA-256 of every file taken, and what each build option switches off:

- **miniz** 3.1.2 (MIT) — inflate only, for the map catalogue. The `.zip` reader is compiled
  out with `MINIZ_NO_ARCHIVE_APIS`; it is the part of miniz with a CVE history and nothing
  here opens an archive.
- **LZMA SDK** 23.01 (public domain) — five files of the ~400 in the archive, which is the
  exact set `DOC/lzma.txt` names as an ANSI-C decoder. Decoder only: this build cannot
  compress anything, in either format, which is why the test fixtures are committed as
  pre-compressed bytes.

Neither adds an import — both are static C — so `dumpbin /dependents wrlines.dll` was unchanged
by their arrival. The thing that did change it was `winhttp.lib` at v0.6.0, which is the OS and
not a dependency to vendor; see *Your data* in the README and
[src/wr_http.h](../src/wr_http.h). The release workflow asserts the exact list either way.

### zstd

About 3.5% of demos — the very largest — have a zstd body rather than an LZMA one, and reading
them needs a second decompressor for a format no current demo uses. They are reported as
**skipped**, never as failed, and that distinction is load-bearing: a skip is not written to the
failure record, so recording them as failures would put a permanent entry in every user's
`_failed.txt` that *Retry the failures* would re-fail for ever.

---

## Use it

### Extracting from inside the game

The Runs tab counts the demos you have downloaded for the map you are standing in, how many
are already extracted, and how many are new — then offers a button to run the extractor
without leaving the game. Output streams into the panel and the lines appear when it finishes.

The count is exact rather than a guess, and cheap: everything a demo says about itself lives in
its first 512 bytes, and the extractor names its output after the source file's basename, so
"is this demo for this map, and has it been done" is one 512-byte read plus one file-exists
check. Measured on a 4095-demo library across 475 maps, that reads every file correctly.

It used to read one field — the map name at offset `0x10` — because reading the rest meant
having a parser, and the parser was in Python. Now that [src/wr_mtv.cpp](../src/wr_mtv.cpp)
exists it reads the whole fixed header and applies the same three sanity gates the reference
does: a non-empty map name, a tick interval in `[0.001, 0.1]`, and a SteamID64 whose high word
is `0x01100001`. A demo that fails one of those has had its layout moved under it and can never
be extracted, so it is now counted as *already known bad* rather than as work waiting to be
done. Nothing in a 6,249-demo library fails any of them, which is the point: the counter should
be telling the truth on the day one does.

It runs **only when you press the button** — starting work off a map change would compete with
the game for CPU while you play, behind your back. The workers run at below-normal priority and
are additionally marked as background work through `SetThreadInformation`, which on a hybrid CPU
asks the scheduler to park them on the efficiency cores and leave the performance cores for the
game. Skipping what is already done means pressing it a second time costs seconds rather than
minutes.

It used to launch `python.exe` with a pipe and read its output. From v0.7.0 it is a worker pool
inside the game's own process, which cost three things that were free before: a worker that
faults takes the game with it (hence the one `__try` in the project, in
[src/wr_jobs.cpp](../src/wr_jobs.cpp)); a demo's working set now lives in the game's heap, times
N (hence the admission budget); and **Stop is no longer instant**, because `TerminateThread` on a
thread of somebody else's process leaks whatever locks it held — the CRT heap lock among them —
and the game would deadlock on its next allocation. Cancellation is cooperative, the panel says
so rather than claiming a clean stop, and nothing is lost either way: every file is written to a
temp name and renamed.

The panel is there whether or not the map has any paths yet. A map with nothing extracted is
exactly when you need the button most.

### The extractor was not hung, it was mute

Reported as "the timeout needs to be quicker". The timeout was not the problem.

`_run_all` used `ProcessPoolExecutor.map`, which yields in **submission** order — while its own
docstring claimed completion order. One slow demo held back the `[n/total]` line of every finished
demo behind it, so the panel went silent for as long as that demo took, however much work was
actually completing. It is `submit` + `as_completed` now.

The timeout was also narrower than it looked: `check_deadline` was called from two places, both
inside the chain-search DP. Decompression and the **candidate scan** — 32 passes over the whole
decompressed body, which on the 47 MB demo in this library is the phase that runs long — were
outside it entirely, so a demo could sail past `--timeout` by any margin it liked. The scan checks
it now.

The default drops **180 s → 30 s**, and it is a slider. Measured across 4,388 demos here: median
58 KB and about a second, slowest normal one 7 s, but the 99th percentile is 5.8 MB and 6.5% are
over 700 KB — which is the size that actually hit the old limit, twice, at three minutes each.

And there is a **Stop** button, which needed three things to be true first:

- It terminates a **job object**, not the process. The extractor runs one worker per core bar two,
  and those are grandchildren we hold no handles for; killing the parent alone leaves them each
  burning a core. If the job cannot be created the fallback says so rather than reporting a clean
  stop.
- `g_proc` is under the lock. The UI thread and the reader thread both touch it, and unsynchronised
  the worst case is `TerminateProcess` on a **reused** handle — someone else's process.
- The failure record is flushed **as failures happen**. It used to be written only in the epilogue,
  so stopping at demo 40 of 66 would have thrown away every expensive timeout learned so far and
  you would pay them all again. Fixing that had to come before the button existed.

Completed `.wrpath` files survive any kill — every write is a temp file plus an atomic replace. The
comment claiming otherwise, which was the only cancel-adjacent reasoning in the codebase, was stale
and has been corrected.

### It was mute again, one layer earlier

Reported after v0.7.0 as "it freezes for 20–30 seconds the first time I hit Extract, then all the
text appears at once, and every time after that is fast". The same shape as the bug above, in a
different place, and the "every time after that is fast" is the clue that says which place.

Nothing was slow. Before the extractor can do anything it has to work out *which* of your demos are
for the map you are on, and a demo's map name is inside the file: the filename is a replay hash,
and the folder says nothing — `momtv\online\` is keyed by map ID, `momtv\local\` is whatever the
game called your own recordings, and `wrlines_data\demos\` is ours. So the answer was always found
the only way it can be found. Open all of them. **6,416 files here**, and not one line printed for
the whole of it.

Warm, that costs about a quarter of a second. Cold — the first press after the machine booted, or
after the game had pushed everything else out of the file cache — it is thousands of individual
seeks, which is exactly the 20 to 30 seconds reported. The second press was fast because Windows
still had every one of those headers in memory. Two separate walks did it (the demo counter on
every map change, the extractor on every run), and a finished job started the counter again, so
pressing Extract shortly after loading a map ran two of them against one cold disk.

Two changes, and they are different kinds of fix:

- **It says so now.** One line before the walk and a count every second during it. That is the
  honest half: even at its fastest this is work, and a button that does not acknowledge a press is
  indistinguishable from one that is broken. It is also the half that had to be done carefully —
  `tests\parity.ps1` compares stdout **character for character** against the frozen Python oracle,
  which prints nothing here and cannot be changed, so the progress lines are behind a flag that only
  the in-game path sets. The console front end the oracle is compared against stays silent.
- **It stops asking.** `wrlines_data\demoindex.txt` remembers which map each demo is for, keyed on
  the file's path, size and last-write time — all three of which come out of the directory listing
  already being read, at no extra cost. A row is believed only while all three still match, so a
  re-downloaded demo is re-read rather than trusted; size catches a truncated download, and the
  write time catches a replacement that happened to be the same length. Measured over the game tree
  with the file cache already warm, the walk went from 0.33 s to 0.09 s — and warm is the case where
  there was least to win. Cold, it replaces several thousand scattered reads with one sequential
  800 KB one.

It is a **cache**, not a record. A miss is not an error, it is a file being opened the old way;
a corrupt row is dropped and re-read; deleting the file costs one slow run and nothing else. A
refusal is remembered too — a file that is not a demo at all is not reopened on every walk to be
told the same thing.

Like everything else under `wrlines_data\`, it is per-machine and not for sharing: it holds paths to
files on your disk and the map names of other players' runs, which is why it is not in
`settings.cfg` — that file promises it contains display settings and nothing else, and it keeps
that promise.

### Demos that cannot be extracted

Some demos cannot be read at all — see [Known limits](#known-limits). Failing to read one is
not cheap: on `surf_colin_blaster_69000` the 66 unreadable demos take **220 seconds** to fail,
every time, and until now every press of Extract paid that again and reported the same 66 as
"new".

So failures are recorded, in `wrlines_data\paths\<map>\_failed.txt`, with the reason and the
demo's size:

```
# extractor-revision <TAB> bytes <TAB> demo <TAB> why
1  19307  00d30670475ca1b1b6769664992ec66a3c86db7e  origin stream identified but only 11% of ticks could be recovered
```

`--skip-existing` then skips them, and the Runs tab counts them separately — "75 extracted,
0 new, 66 could not be extracted and are being skipped" rather than a permanent, misleading
66 new. Measured: 220.5 s → **0.6 s**.

Three things keep the record from going stale on its own:

- The size is checked as well as the name, so a re-downloaded demo gets another go.
- The record carries `EXTRACTOR_REVISION`, which is also stamped into every `.wrpath` at
  offset `0xFC`. Bumping it when the extraction logic changes retries every recorded failure
  **and** marks everything already written as out of date — so a fix that changes what gets
  extracted actually reaches the files that were extracted wrongly, rather than skipping them
  forever as "already done".
- Any demo that later succeeds is dropped from the record.

`--retry-failed`, or the button beside Extract, tries them anyway.

### Speed

Each demo is completely independent — one file in, one file out — so extraction runs across
threads. The worker count defaults to all cores but two, and the two are not arbitrary: one of
the cores we would otherwise take is the game's render thread and the other is its main thread,
and the whole point of the panel is that you can keep playing while this runs. It is capped at
sixteen whatever the machine has, because the work is memory-bound long before it is core-bound
— each worker holds a decompressed body and a candidate arena.

There is also a **timeout**, 30 s per demo by default, on the slider beside the button. The
dynamic program is quadratic in the worst case and some demos genuinely take a minute; that
reads as "it did four quickly and then stopped". A demo that hits the limit is recorded as an
ordinary failure, so it is not paid for twice.

Measured against the Python it replaces, on the same demos and the same machine: about **fifty
times faster**. Most of that is not the language. It is that the reference reads a float at an
arbitrary bit position by shifting the *entire* decompressed body by each of eight bit phases and
keeping the copies — roughly ten times the body in allocations, per demo — where the port does
one unaligned 8-byte load and a shift. See [src/wr_dp.h](../src/wr_dp.h).

### The two lines of Python that are not what they look like

The port has to agree with the reference **to the last bit**, and not out of neatness. Both
numbers below are compared against a threshold, and a chain that misses a threshold is not
slightly different — it is *banned*, and the search goes off and finds a different path through
the demo. So a last-place disagreement does not move a coordinate; it selects another route.

Two ordinary-looking expressions do arithmetic nobody would write out:

- **`math.dist` and `math.hypot` are not `sqrt(x² + y² + z²)`.** Since 3.8 CPython computes a
  vector norm with lossless power-of-two scaling, exact squaring via `fma`, Neumaier compensated
  summation and a differential correction of the square root. On a table of 407 real coordinate
  pairs, **153 of them disagree with the naive form** — and those are ordinary surf-map
  distances, not exotica.
- **`sum()` over floats is Neumaier-compensated too, and only since 3.12.** Not `math.fsum`,
  which is exactly rounded, but not a running total either. This one was found by the parity
  run rather than by reading: a plain `+=` over nine thousand step lengths landed 3.6 × 10⁻¹¹
  away from the reference. That is harmless where it was noticed — a printed diagnostic — and
  is not harmless in the other place the reference uses it, where the same function averages a
  segment's coordinates and compares the result against a radius to decide whether that
  segment is part of the route at all.

  Worth stating plainly: **the port was correct against Python 3.11 and wrong against 3.12.**
  The version in `wr_dp.h` is not decoration.

Where the reference is *naive*, so is the port. The dynamic program's own step lengths are a
plain `sqrt` on both sides; "fixing" those would change which edges the DP accepts. `build.bat`
passes `/fp:precise` for the same reason — reassociating any of this is a correctness change.

**Launch the game**, then:

```
wrinject.exe
```

**3. Press INSERT.** Runs tab, tick the ones you want. **ESC** closes the panel.

### Staged maps

Momentum records a **separate demo per stage and per bonus**, not one per map. On a
staged map that is most of what you have downloaded:

| | full-map | stage | bonus |
| --- | --- | --- | --- |
| `surf_demise` | 51 | 0 | 0 |
| `surf_tensor2` | 2 | 27 | 3 |

Those 27 stage runs start in ten different places tens of thousands of units apart, so
"the three fastest runs" are usually three *different stages*, none of them where you are
standing — which looks exactly like the lines being broken or missing after a teleporter.

So the Runs tab shows a **Track** column (`main` / `stage 3` / `bonus 1`) and a **Near**
column giving each run's distance from where you actually are, and the default button is
**Best 3 near me** rather than fastest-overall. Deltas compare within a track, since a
stage-3 time against a full-map time means nothing.

Lines appear once the memory scan has located the world→screen matrix, which needs you to
be **in a map** — there is no camera at the main menu for the oracle to confirm against.
It takes a second or two. Diagnostics shows exactly where it got to.

If a map has no cached paths yet, the Runs tab shows you the exact command to run.
**Pick map...** overrides auto-detection, which is useful if you want to look at a route
for a map you are not currently standing in.

### The mouse

The panel adapts to what the game is doing with the cursor:

- **Game menu open** (cursor visible) — it follows the real cursor exactly, and does not
  draw a second one.
- **Normal gameplay** (cursor hidden and captured) — the OS cursor is pinned to the
  screen centre by Source's mouselook and is useless, so it integrates raw-input
  deltas into its own pointer and draws that. Raw input is consumed while the panel is
  open, so the camera does not turn under you.

---

## How the extractor works

A `.mtv` is Momentum's own container (`MMTV`): a packed binary header, a JSON run-stats
blob, then the run body compressed with Valve-LZMA (or zstd on the very largest files).
The body is a Source entity-delta netstream — there is no public spec, and the send-table
metadata needed to decode it properly lives in the closed-source game DLL.

Two details worth knowing about that container, because both cost a day. The JSON blob is
**not** at a constant offset — `0xC6` in one container version and `0xC7` in the next, with
nothing saying which — so it is found by scanning for a `{` whose preceding `u32` is a
plausible length and whose byte at that length is a codec magic. And that scan has to try
*every* `{` in the window rather than the first: the padding around the player name is
arbitrary bytes, and taking the first on faith is what produced `implausible JSON length
1076353433` on two demos. Valve-LZMA is its own thing too — seventeen bytes of header, then a
raw LZMA1 stream with no end marker, so the decoder has to be told how much to produce.

All of that exists twice: in the frozen `tests/reference/wrpath_extract.py`, and in
[src/wr_mtv.cpp](../src/wr_mtv.cpp) over a committed copy of the LZMA SDK's decoder. It is
allowed to exist twice because the two are checked against each other:
[tests/parity.ps1](../tests/parity.ps1) runs both over every demo on this machine and compares
the decompressed bodies byte for byte. That comparison is a *perfect* oracle — no floats are
involved yet, so a difference is a difference rather than something to argue about — which is
why it is the checkpoint the rest of the port is built on top of.

We don't decode it properly. `m_vecOrigin` is networked as three raw IEEE-754 float32, so
the extractor scans the decompressed body for every bit position holding a plausible
coordinate triple, then finds the longest physically-smooth chain through them with a
dynamic program.

A DP rather than a greedy walk, for three reasons: several other fields (velocity,
`wishVel`) also form long smooth chains; the stride between player frames is variable —
216 and 400 bits are both common, because Source only sends props that changed; and a
greedy walk that takes one wrong turn loses the whole rest of the run. The DP keeps the
best predecessor for every candidate, so a bad edge costs only that edge.

Picking the right chain is exact and self-validating: the JSON header records the run's
own `maxHorizontalSpeed`, and the real path is the chain whose step lengths reproduce it.
It matches to about 0.01 u/s in practice. A chain that cannot be confirmed is still
written, but flagged, and the Runs tab shows a `!` next to it.

**Measured on 52 `surf_demise` demos:** 51 extracted, median coverage 96.8 %
(min 95.4 %, max 100 %). Across a mixed sweep of other maps, median 97.3 %.

Coverage is never 100 % on most runs and is not meant to be — Source delta-compresses, so
a tick where the player did not move re-sends no origin, and those ticks contribute no
path.

Sanity check that the output is real, not noise: across 51 independent runs by different
players, every extracted path starts within **247 units** of the same point (the map
spawn), and the total path lengths agree to within about 1 % (134 244 – 135 416 units).
The record and the second-place run stay a median of 121 units apart — they are surfing the
same ramps.

Runs that teleport (stage transitions) are recovered by re-running the DP over the bit
ranges no accepted chain covers, then stitching the legs with the joins recorded as explicit
breaks so the renderer never draws a line across them. That took one map from 8.8 % of demos
extracted to 90.3 %, and the stitched result was validated independently: voxelising 30
known-good single-stage demos and testing the stitched points against that geometry puts
98.4 % of them on the real route.

### Known limits

- **zstd bodies are skipped.** ~310 of the 6,250 demos on this test install, all of them the
  very largest files. Reported as *skipped*, never as failures — a skip is not written to the
  failure record, and getting that backwards would poison every user's record with hundreds of
  permanent entries that `--retry-failed` would re-fail forever.
- **An install path with non-ASCII characters in it will not work, and v0.7.0 made this
  worse.** Every path here is `char*` and every file call is the `-A` Windows form, so a byte
  ≥ 0x80 anywhere in the path — `D:\Игры\`, an accented user name — cannot be named. That was
  always true of the demo counter. What changed is that extraction used to be a Python script,
  and Python opens files with *wide* paths, so it read such an install perfectly and the DLL
  drew the results. Moving extraction in-process took that away.

  It is the **one respect in which the port is a step backwards**, so it is detected and
  said out loud rather than discovered: the log names both paths at startup, and pressing
  Extract refuses with the reason instead of reporting a clean run over nothing.
  `GetShortPathNameA` is not the workaround it appears to be — 8.3 name generation is off by
  default on modern volumes, so it returns the long path unchanged on exactly the machines
  that would need it. The real fix is a `-W` conversion of the project, which is a change of
  a different size and is the obvious follow-up. Bounded, at least: demo file names are hex
  and map names are ASCII by the game's own rules, so only the install path is exposed.
- **A junction or symlink inside a demo folder is not followed.** If you keep demos on a second
  drive and link them into `momtv` or into `wrlines_data\demos`, the linked directory is listed
  and not descended into, so nothing inside it is found. This matches the reference exactly —
  `os.walk` defaults to `followlinks=False`, and CPython has reported junctions as links since
  3.8 — and it is also what stops a link pointing back at one of its own parents from recursing
  until the stack is gone. The link you hand the extractor as a *root* is fine; it is only links
  found *during* the walk that are skipped.
- A small number of runs fail identification outright (2 of 30 in the mixed sweep). They
  are reported as `FAIL`, never silently written.
- Short stage runs that start near the world origin can be indistinguishable from the
  velocity stream by magnitude alone. These are reported as failures with the reason given.
- ~~**Some maps are simply much worse.**~~ **Fixed, and it was our bug.** `surf_colin_blaster_69000`
  was the worst measured by far: 66 of 141 demos failed, and of the 75 written only 17 passed
  the max-speed check, at a median coverage of 30.1 % against 96.8 % on `surf_demise`. The
  cause was `WORLD_LIMIT`, the filter that decides whether three floats could be a coordinate.
  It was ±16384, the figure every Source reference quotes — and that map runs out to **−31295**.
  Most of the origin stream was being discarded before the search ever saw it. At ±65536:

  | | before | after |
  | --- | --- | --- |
  | extracted | 75 of 141 | 130 of 141 |
  | median coverage | 30.1 % | 83.9 % |

  Re-running ten `surf_demise` demos at the wider limit gives byte-identical results, so this
  only ever threw away real data. The same constant in the DLL meant that map had **no lines
  and no energy readout at all** whenever the player was past 16384 — the oracle rejected the
  real camera matrix every frame.
- Split markers are only drawn when the anchoring passed its confidence check. Wrong
  markers are worse than no markers.


### The energy readout was re-basing itself mid-air

Reported as "it flickers at ramp ends, and dropping from a platform makes the number jump". It
was not noise. The height everything was measured from was re-armed every frame a ground test
passed, and that test — vertical speed under 30 for three frames inside a six-unit band — is
satisfied **at the apex of every arc**. At g=800 the vertical speed passes through zero slowly
enough to hold the window for about a quarter of a second, so the zero point silently re-based
itself at the top of every jump and stepped again by the full height difference on landing.

The fix is an **anchor** that is set once and never re-armed by anything the player does. In
free flight energy is conserved, so against a fixed anchor a whole jump must read as a flat
line — which is exactly what `tests/test_energy.cpp` now asserts, driving the real estimator
chain with a ballistic arc plus view bob:

```
relative energy over the whole arc: 549.6 .. 556.5 (spread 7.0)
```

against a theoretical 551 for that launch. The first version of that test read a spread of 88,
and chasing it down found a second bug worth more than the first: the velocity was measured
over a window but paired with the position at the window's *end*, so the two terms of
`E = z + v²/2g` referred to instants 20 ms apart. Under gravity that is a systematic error, not
noise — a ballistic arc appeared to lose 46 units of energy while falling. Pairing both at the
window's midpoint makes E exact under constant acceleration.

Everything is filtered in **seconds** now rather than in frames. Every filter used to be a
frame count, so the readout behaved like a different instrument at 60 fps than at 300 — and the
frame rate moves with how many lines are drawn, so turning lines on changed how the number
moved. Measured at 60, 200 and 500 fps the chain now agrees to within 0.1 units.

### A fail trigger used to latch the readout off permanently

Reported next: "every time you hit a fail trigger it saves your value and stops updating, and
hitting the restart key doesn't reset it". That one was not in the filters at all — it was the
**order of two statements**.

The live velocity is differenced from camera positions, so a teleport has to be detected and the
window dropped, or a 5000-unit jump in one frame reads as a million units per second. Detecting
it was fine. Recovering from it was not:

1. the teleport is detected, and the velocity window is emptied
2. the window now holds one sample, so the estimator has nothing to divide by and returns false
3. the sampler returns early — **before** recording where the player now is
4. the next frame therefore compares against the *pre-teleport* position, sees a jump over 400
   units again, and empties the window again

Every frame, for the rest of the map. The displayed figure stayed at whatever it read the
instant you failed, and since you fail at the bottom of a map while the anchor is the start pad,
that stuck value was a large negative one — the reported "it drops to −6000". Only the Reset
button cleared it, and only because Reset clears the flag the teleport test was gated on. The
same latch also froze the velocity vector.

The last position is now recorded before every early return, and a teleport is treated as a
discontinuity: the filters are *dropped* rather than run across it, since a 0.3 s average that
spans a teleport is a number that was never true at either end. A teleport that lands back at
the anchor is taken as a **restart** — a fail trigger, or the restart key — and zeroes the run
clock and the peak. That is the only restart signal the tool has; it reads the camera and
nothing else, so it cannot see a trigger fire or a key press.

`tests/test_energy.cpp` links the real `wr_energy.cpp` for this, not a copy of the chain. Put
the old statement order back and it fails in exactly the reported way:

```
a fail trigger does not freeze the readout
     -800 at the moment of the fail, -800 two seconds after the respawn
```

### Falling does not add energy, and that is the point

Also reported: "I thought falling from the start zone would add a bunch of energy but it
doesn't — it only changes when my horizontal speed changes." Correct, by construction. Falling
converts height into speed at exactly the rate `E = z + v²/2g` is defined to hold constant, so a
clean drop reads flat. Negative is not height dropped; it is energy that ended up in neither
height nor speed — bad ramp entries, wall clips, friction.

Which means `E_rel` itself cannot be made to "slowly rise" while you play well: on a descending
surf map it falls for everyone.

### The budget: the same information with the big numbers going up

The follow-up ask was "a mode that counts all units from the start zone — counting all the gain,
including the fall". That works, and it needs no new measurement, because it is an identity.
With `K = |v|²/2g` and `H = z_anchor − z`:

```
E_rel = (z − z_anchor) + (K − K_anchor)     ⇒     K − K_anchor = H + E_rel
```

What you are carrying is exactly what you spent plus what you netted. So:

```
spent   H = z_anchor − z      height cashed in       rises as you descend
banked  K = |v|²/2g           still in your speed    rises as you speed up
wasted  H − K                 the difference         = −E_rel, to the last digit
carried K/H                   the headline, a %
```

Implementation is one extra EMA on the window-midpoint *height* beside the one on energy. Since
an EMA is linear, subtracting it gives exactly the filtered kinetic term, so `spent − banked`
equals the negated headline figure rather than nearly equalling it — the harness asserts a worst
disagreement of **0.0000 units** over a scripted drop, and reads `carried` at exactly **100 %**
through a clean fall from rest.

`carried` works *mid-run*, not only at the finish. On `surf_demise`:

| run | 5 % | 25 % | 50 % | 75 % | finish |
| --- | --- | --- | --- | --- | --- |
| WR 37.17 s | 120 % | 104 % | 101 % | 95 % | **88 %** |
| 57.84 s | 97 % | 79 % | 50 % | 26 % | 34 % |

Over 100 % is not a bug — it means air strafing put in more than the map gave you, and the
fastest `surf_utopia` run finishes at **293 %**. `H` is deliberately not clamped monotone:
measured median backtrack from its running maximum is 1,465 units on `surf_demise` and **31,160
on `surf_vacant`**, and clamping would break the identity to hide that.

### Counting only the gains, without counting the noise

The gross figure — total energy your strafing added, ignoring the losses — is the one that only
ever rises. The obvious way to compute it is a trap:

```
gained += max(0, E_now − E_prev)
```

Rectifying a noisy derivative accumulates the noise. On a trajectory where energy is *exactly*
constant, that sum reads **6,600–12,800 units over a minute** against a real world record's
~3,700. And the natural test passes anyway: a rectified EMA's noise floor is `T·σ/(τ√2π)`, which
contains no `dt` at all, so 60 fps and 500 fps agree about a number that is entirely noise.

So legs are banked only once the signal reverses by `h` — an excursion smaller than `h`
contributes *exactly* zero, and the leg count is bounded by the signal rather than by the sample
count. `h` was swept rather than picked. Worst case over six speed/frame-rate combinations on
the null trajectory, against the cost to a real world record:

| `h` | null trajectory (true 0) | surf_demise WR |
| --- | --- | --- |
| 25 | 4253 | 4365 |
| 50 | 4.4 | 4189 |
| **150** | **0.0** | **3720** |
| rectifier | 12801 | — |

`h = 50` looks fine and is not: rebuild the binary with unrelated code changed and it moves
between 0 and 203, because the excursions sit right on the threshold and which ones confirm
turns on the last bits of a float. `h = 150` costs 15 % of a real signal and reads exactly zero.

It also *discriminates better*, because what the larger threshold rejects in a slow run is mostly
noise-scale wobble. Median `gained` across 50 clean `surf_demise` runs, in quartiles by time:

| | Q1 37.2–38.0 s | Q2 38.1–39.0 s | Q3 39.0–40.9 s | Q4 41.0–57.8 s |
| --- | --- | --- | --- | --- |
| `h = 50` | 2596 | 979 | 316 | 197 |
| `h = 150` | **2338** | **793** | **72** | **14** |

A factor of 167 end to end against 13. The plain correlation with run time is only −0.325, and
that is not a contradiction — the relationship collapses over the first two seconds and is flat
after, so a linear coefficient understates it badly. Read it as a threshold: 30 of the 50 runs
gain under 500 in total, and every one of those is slower than 37.8 s.

### Strafing efficiency, and the metric that had to be thrown away

The obvious way to show "you turned too fast" is to colour the line by how fast the velocity is
turning. **The data says that fires on perfect play.** Air acceleration can turn a velocity by
at most `wishspeed/|v|` radians per tick — 57°/s at 2000 u/s — but a surf ramp turns it too,
through the surface normal, and far faster. Fraction of samples exceeding what air accel alone
could manage:

| run | |
| --- | --- |
| `surf_demise` world record | 10 % |
| `surf_vacant` world record | ~20 % |
| `surf_666` (43.575 s) | 24 % |

So the line is coloured by something measurable instead. Per tick, air strafing adds at most
`ws² − c²` of energy, maximised at `c = 0`, giving a hard ceiling of

```
P_max = ws² / (2·g·tick) = 900 / (2·800·0.015) = 37.5 energy units per second
```

independent of speed. Checked against twelve record-class runs on three maps, the 99th
percentile of measured `dE/dt` lands between **+38.18 and +38.88** against that theory of 37.50.
Efficiency is `(dE/dt) / P_max`, and it discriminates enormously: median on the `surf_demise`
world record is **+0.531**, and on the two slowest runs in the same set **+0.010** and **+0.003**.

Boosters add energy for free — real runs contain `dE/dt` spikes of +726, +3116, +9039 and
+13142 units/s, up to 350× the ceiling — so anything past 3× on the **gain** side is drawn as a
gap rather than as perfect play.

### The rejection was symmetric, and that hid almost everything worth seeing

Reported next: *"can you describe why it goes red or green like I'm 12? the colours don't make
any sense."* They didn't, and it was not only the explanation.

The `3×` rejection above was applied to *both* signs, returning exactly `0.0` — the same value as
free flight. But the ceiling is on **gain** only. Nothing bounds how fast energy can be taken
away, because a ramp collision clips the velocity into the surface plane and can remove as much
as the geometry likes. Measured over 865,026 samples from 342 stored runs:

| | samples | share |
| --- | --- | --- |
| `eta > +3` — a booster, as documented | 9,675 | 1.12 % |
| `eta < −3` — undocumented, drawn identically | 162,601 | **18.80 %** |

**94.4 % of the rejections were negative, and those samples carry 94.6 % of all energy lost in the
whole library.** Every hard ramp entry — the most informative thing on a surf line, and the reason
the mode exists — was being painted as "nothing is happening".

Three smaller faults compounded it. The bucket count was **even** over a symmetric range, so
there was no bucket at zero and free flight drew as a faintly green grey while the velocity
vector drew the same value as true neutral. Saturation was asymmetric (red at −0.5, green at
+0.6). And the per-point array was `calloc`d, so the ends of a run and every teleport-spanning
window read "neutral" when the truth was "no reading".

With the rejection one-sided and a real neutral band, on `surf_demise`:

| run | gaining | losing | nothing happening | no reading |
| --- | --- | --- | --- | --- |
| WR 37.2 s | 50.9 % | 41.3 % | **6.0 %** | 1.8 % |
| median 39.0 s | 56.7 % | 35.6 % | 7.2 % | 0.5 % |
| slowest 57.8 s | 30.6 % | 35.1 % | **34.1 %** | 0.2 % |

The discriminator is the neutral share, not the red one: **a world record is dim for 6 % of its
length and the slowest run for 34 %.** A line that is mostly dim means the player is barely
strafing. A world record is 41 % red, because every ramp entry costs something.

Neutral now keeps the run's own colour, dimmed, instead of going grey — colouring by efficiency
used to replace the line colour outright over its whole length, so turning the mode on destroyed
every cue about whose line was whose. There is also an on-screen key, which no colour scheme in
this tool had before.

### The velocity vector was reading noise

Its colour came from live efficiency: a 0.30 s difference of a 0.30 s average of a
camera-differenced velocity. On a trajectory where energy is exactly constant, with the position
noise implied by the module's own stated "few percent" velocity error:

| speed | p05..p95 of `dE/dt` | as eta | fraction saturating the ramp |
| --- | --- | --- | --- |
| 2000 u/s | ±25.5 | ±0.68 | **14 %** |
| 3200 u/s | ±40 | ±1.07 | **36 %** |

Full green needed 6.75 energy units over the window — smaller than the 12-unit dead band the same
module refuses to call "rising", and larger than the 5-unit step it rounds the number to for
display. It is coloured by the 0.75 s trend now, the same signal as the arrow beside the
crosshair, so the two can no longer disagree.

**It shares a colour ramp with the line efficiency and does not mean the same thing**, which is
worth stating because it reads as a contradiction. The lines are **efficiency** — how much of what
air strafing *could* add was actually added. The arrow is a **trend** — which way your total is
going. On a ramp you can be strafing beautifully, green on a demo line, while your own energy
falls because the ramp is taking more than you can put in. The on-screen key says so now.

### Why efficiency colours demo lines and not your own

Asked directly, and measured before answering. A stored run differences a velocity the demo
recorded; your own line has to difference one estimated from where the camera was. Simulated
against twelve real `surf_demise` runs — resample the position at 200 Hz, add view bob, push it
through the real estimator, record points the way `WrLiveRecord` does, then compare against the
truth **over the same window** so only the estimate is judged:

| window | colours it right | points the **wrong way** |
| --- | --- | --- |
| 0.25 s | 45.2 % | 26.0 % |
| 0.40 s | 58.2 % | 24.1 % |
| 0.60 s | 63.5 % | 21.8 % |
| 2.00 s | 81.5 % | 8.5 % |

A quarter of the line drawn backwards is not a metric, and a 2-second window smears across a whole
ramp. Two results make this conclusive rather than a tuning problem.

**View bob does not matter.** At bob = 0 — a perfectly steady camera — the figures are the same to
a point or two. This is not jitter a filter could remove.

**It is worse exactly where it matters.** Restricted to airborne samples, where eta really is air
strafing rather than a ramp collision, it agrees **45.5 %** at 0.40 s and points the wrong way
**32.1 %** — and barely improves with a longer window. Surface contact is easy to sign because the
losses are enormous; the small numbers are the whole point of the metric, and they are the ones a
camera-differenced velocity cannot resolve against a 37 units/s ceiling.

So it is not shipped for the live line, and the on-screen key says so rather than leaving you to
wonder why your line is plain.

### The live line was pairing a position with a velocity from a different moment

Found by that measurement: with a *perfectly noiseless* camera the error was still larger than the
signal, which is not what noise looks like.

`WrEnergySample` is careful about this — it pairs the **raw** window velocity with the position at
the window's **midpoint**, because those refer to the same instant, and a comment records that
getting it 20 ms wrong made a ballistic arc appear to lose 46 units over 1.6 s of free fall. But
`WrLiveRecord` was handed *this frame's* feet and the *smoothed* velocity readout: two moments
about **80 ms** apart. Energy is quadratic in speed, so on a ramp that is worth hundreds of units,
and everything computed from a live point inherited it — the Graphs tab's live curve included.

`WrEnergySampleAt` now hands out the sampler's own pair. Measured in `tests\test_energy.exe` on a
ballistic arc, where energy is conserved by definition: mean energy drift across the arc
**0.8 units, against 78.7 before**.

### The deadstrafe period, and why it does not apply here

Source's `CategorizePosition` quarters `m_surfaceFriction` while a player is airborne rising
slower than +140 u/s over a surface too steep to stand on, and `AirAccelerate` multiplies its
acceleration by that friction. The KZ community calls the result a deadstrafe period. It is real.

It only bites if it drops the acceleration below the 30 u/s wishspeed cap, and the acceleration
is computed from the **uncapped** wishspeed:

```
accelspeed = sv_airaccelerate × maxspeed × tick × surfaceFriction
```

| config | normal | quartered | ceiling | bites? |
| --- | --- | --- | --- | --- |
| Momentum surf, airaccel 150 | 562.5 | **140.6** | 37.50 → 37.50 | **no** |
| CS:GO KZ, airaccel 12 | 46.9 | 11.7 | 36.00 → 22.63 | yes |
| CS:S bhop, airaccel 10 | 37.5 | 9.4 | 37.50 → 19.78 | yes |

The crossover is around `sv_airaccelerate 32` at 250 maxspeed on a 66.7 tick. Confirmed against
the demos — p95 of `dE/dt` split on vertical speed:

```
rising 0 < vz ≤ 140 :  69,916 samples    p95  38.07   (±4 window: 37.30)
everything else     : 795,096 samples    p95  37.99   (±4 window: 37.12)
```

No depression inside the window, and both land on the theoretical 37.50. Honest limit: this shows
the achievable *gain* is not reduced. It cannot show whether the friction was set at all, because
at these settings both branches predict the same ceiling — but the achievable gain is the only
thing the tool uses. The ceiling takes `sv_airaccelerate` and `sv_maxspeed` as settings now, and
the Energy tab says whether the quarter would bite at whatever they are set to.

### A live strafe gauge, and why it is slow

Asked for as *"green to red based on whether it's the optimal strafe"* — green when you are turning
ideally on a ramp and gaining maximum energy, redder as the curvature drifts off. Two things the
question assumed, both already answered above:

**The ramp's angle needs no compensating.** Turn rate was the first attempt at this and it fires on
a tenth to a quarter of the samples of *record-class* runs, for the reason in the section above: a
ramp turns your velocity through its surface normal far faster than air acceleration can. What is
measured instead is the consequence — `dE/dt` against a ceiling of `ws²/(2·g·tick)` = 37.5 energy
units a second — and that bound is the same at 500 u/s as at 3500 and the same on every angle of
ramp. There is no geometry left in it to correct for.

**The quarter needs none either**, at these settings, and it is `0 < vz ≤ 140` — rising *slower*
than 140, not above 140 units of height. See the table above: at `sv_airaccelerate 150` the
quartered acceleration is still 141, four and a half times the wishspeed cap, so the ceiling does
not move. `WrAirPowerCeilingEx(..., 0.25f)` exists for the settings where it *would*, and the Energy
tab warns when they are.

So the metric was already there; what was missing was a live reading of it. That is a **fifth mode
on the centre box**, reached with Page Up / Page Down, showing one number and colouring it off the
same red-green ramp the demo lines use, so the two can never disagree.

It is deliberately **slow**, and the window is the entire design. Your own velocity is estimated by
differencing camera positions, and against a 37-unit ceiling that estimate is coarse — simulated
against twelve real runs, resampled and pushed through the actual estimator:

| window | agrees with the truth | points the wrong way |
| --- | --- | --- |
| 0.25 s | 45.2% | 26.0% |
| 0.40 s | 58.2% | 24.1% |
| 0.60 s | 63.5% | 21.8% |
| 2.00 s | 81.5% | 8.5% |

So the default window is **2 seconds** and your own drawn line stays uncoloured. Restricted to
airborne samples — where the number means strafing rather than a ramp collision — 0.40 s agrees
45.5% and points the wrong way 32.1%, and barely improves with a longer window. A quarter of a line
drawn backwards is not a metric. Demo lines do not have the problem: their velocity is what Momentum
recorded, and it is exact.

The gauge needs its own history ring for the same reason `WrEnergyPower` divides by the span the ring
*could* give rather than the one asked for. The trend ring is 256 samples of every frame — 0.85 s at
300 fps — so asking it for two seconds returns the change over 1.3 and reads the rate low by the
ratio it was short by, silently, and only at high frame rates. The gauge samples at 20 Hz instead, so
the same 256 slots hold 12.8 seconds and the window always fits.

A rate too large to have come from a player is a **booster**, and it is drawn as *no reading* rather
than as zero — zero also means free flight, and drawing those two the same is what made the first
version of the line colours unreadable.

### Five presses across two tabs

The panel was built to be complete, and it is: nine tabs, every setting anybody could want. What
completeness cost only showed up in somebody else's hands. The path from *a run on the leaderboard*
to *a line on the screen* was five presses across two tabs, and the two tabs did not mention each
other — a fetch finished, the store reloaded, and the demos it had just downloaded were still not
extracted, because downloading and extracting were different buttons on different pages and nothing
said so.

So there is a second panel on `DELETE`, and everything it does the other one could already do. What
it removes is the deciding. One page: the legs of the map you are standing in, the fastest twenty
runs of whichever leg you are looking at, and a tick box per run that means *put that line on the
screen*.

Three things made it harder than it looks, and each is a bug that would have been invisible.

**There is one job slot.** Fetching, extracting and board-reading share a single latch, and it is
also the latch behind every button in the full panel — so a tick cannot call three functions. It
becomes a want, and a step function advances the chain when the slot is free. `WrExtractSubmit`
used to return `void` and no-op silently when busy, which is fine for a button that has just
disabled itself and useless for a state machine; it returns a `bool` now, from the latch itself,
because testing `WrExtractRunning()` first cannot answer the question — the slot can go between the
test and the call.

**A store reload turns every line off.** `FinishLoad` disables everything and the auto-enable then
picks exactly one run. Since every extraction ends in a reload, "tick means set `enabled`" would
lose every previous tick each time the next one finished — tick five runs and watch four vanish,
one at a time. So the tick list is the truth and the store is made to agree with it after each
reload. That is also why `WrPathCancelAutoEnable` exists: the auto-enable runs *later in the frame*,
from inside the renderer, and would otherwise win. Guessing which run you want is right until
somebody has said.

**The chain has to stop.** A run that cannot be got has to stop being tried and say why, because a
row that quietly retries looks exactly like one that is still working — for ever. One fetch attempt
and one extract attempt per run, and then one of two reasons: *the download did not arrive*, or
*that demo could not be read*. Those are different failures and the row says which.

The decision itself is a `static inline` in `wr_quick.h` with the rest of the project's pure logic,
so `tests\test_quick.exe` links **nothing** — no ImGui, no job slot, no run store — and drives the
whole state machine, including a sweep over all 64 states of a pick asserting that a settled row
never starts working again.

Two smaller things fell out of it. The legs of a map had no cheap source: the run store knows the
legs you have extracted, the board cache the legs you have fetched, and the leaderboard API answers
per leg rather than listing them — so a map you have never touched offered one chip on a map with
nine stages. The game's own catalogue has the exact answer for all two thousand maps, offline, and
was simply not being read out; it goes to `wrlines_data\tracks.txt` rather than into `maps.txt`,
which is byte-compared against the frozen reference and cannot gain a column. And the tick list
itself cannot live in `settings.cfg`, which promises in writing to hold no names, no run data and
no record of what was watched — a list of replay hashes you chose to watch is exactly what that
promise excludes, so it sits under `wrlines_data` with everything else that names other players.

### The slow end, and a table that can be sorted

Two things the page could not do, and both are about the twenty rows in front of you rather than
about the chain behind them.

**It could only show the top.** The fastest runs are the hardest to follow — a 37-second
`surf_demise` world record is not a line anybody can trace, and the 79-second run at rank 9,108 is —
and `wr_board.h` has said exactly that since the Board tab was written. `WR_BOARD_SLOWEST` was
already in the job dispatch and already wired to a button in the full panel, so the fetch was never
the problem. The *display* was: the cache accumulates into one rank-sorted file, so after asking for
both ends it holds ranks 1–20 and 9,089–9,108, and a reader that takes the first twenty lines will
never reach the second group no matter how many times it is fetched.

Hence `WrBoardParseTail`, a second reader rather than a flag on the first — that signature is what
the Board tab and `test_board` already call, and changing it would touch every caller to say "no,
still the top". It is dearer, and the comment says so: the forward reader stops at `maxRows`, so the
top twenty of a rank-sorted file really is a twenty-line read, while nothing in the format says
where the end is until you reach it. It reads the cache whole. That is bounded by the windows you
have asked for rather than by the board — the largest one on this machine is 39 KB for a board of
nine thousand runs — and it belongs on a leg change, not on a draw path.

The one thing it must not do is measure the window against the raw row count. The display rule drops
a row with no rank or no hash, so counting rows the file holds rather than rows a table would *show*
would return two fewer than asked for on a cache with two junk rows at the front — the last places
on the board, quietly missing. There is a check for that.

**And it could not be sorted.** The Board tab has had a sortable table since it was written, so this
is the same order array, the same file-static specs pointer for the duration of the `qsort`, and the
same rule that rank breaks every tie so the order cannot wobble between frames. All five columns,
including the two with no heading — those two are the reason it is worth doing at all: the tick
column sorts what you have asked for to the top, and the status column sorts by how far along the
chain each row is. Both get a heading of one space rather than an empty string, because an empty
header has no width to click.

Two details that would each have been a bug. The runner column sorts on the name the table actually
**draws** — the live Steam persona when we have one, the cached alias otherwise — because sorting
the alias while showing the persona looks exactly like a sort that does not work. And the order is a
separate array rather than a resorted `g_rows`: the ticks are keyed by hash and the status cells by
row index, so a sort applied to the rows but not to one of those would put *gave up* on somebody
else's run.

The button underneath ticks the first five **as displayed**, and is named for that. It was "Top 5"
over an unsorted rank list where the two meant the same thing; with a sort and a slow end they do
not, and a button that quietly ignored both would be the one control on the page that did not act on
the table in front of it. At the slow end, sorted by rank, "tick first 5" is the five slowest runs
on the board — which is the point of having gone there.

### One byte, and two features that could not work

The quick page shipped saying **"that demo could not be read"** about five runs whose `.wrpath`
files were sitting on disk. The extraction had worked perfectly. The matching had not.

`WrRun::srcSha1` is the source demo's basename, written into a forty-**byte** field at 0x9C by
`WrPathFixedField` — which always keeps one byte for a terminator. A forty-**character** replay
hash therefore comes back thirty-nine characters long, in every file ever written, by the
reference implementation too. It cannot be widened: `player[]` begins at 0xC4, immediately after,
and the format is frozen and byte-compared. So comparing that field to a leaderboard row's hash
with `strcmp` is not merely fragile, it is **never true** for a downloaded run, and every
successful extraction the chain performed was declared a failure.

Verified on the reported map rather than argued: of the top twenty rows of `surf_helloworld`,
five had a `.wrpath` on disk, the old comparison matched **zero** of them, and the prefix match
matches all five.

The same fault was older and quieter one file over. The Runs tab builds `"<srcSha1>.mtv"` as a
filename for its **send**, **local**, **watch** and **take out** buttons — and a name one
character short is simply not a file. Every one of those had been failing to find its demo for
every downloaded run, reporting "no demo on disk" about a file right there. The evidence is in
the manifest: every entry in `into_game.txt` is forty characters, because every entry came from
the Board tab, which carries the full hash from the leaderboard and hits the exact name first
try. Nothing the Runs tab sent had ever reached it.

So the reading allows for it, in one place each: `WrRunIsFrom` for comparing a stored stem to a
known hash, and a one-character wildcard fallback for turning a stored stem back into a filename.
Thirty-nine hex characters is 156 bits of a SHA-1; the interesting part of the guard is not the
collision but the **floor**, because a run recorded by the game itself has a stem like
`104455274-surf_fiellu-1781797367-main-nrm-60.990`, which truncates in the middle — and a prefix
match with no floor would cheerfully call two of those the same run.

It is pinned in `test_wrpath`, at the end of the round trip through the real writer and the real
loader, as a fact about the format rather than a bug awaiting a fix: the stem is forty, the field
is thirty-nine, `strcmp` is false, `WrRunIsFrom` is true.

### Every map on the machine was asked about in surf

The quick page on `DELETE` reads the leaderboard by itself on a map change. On `bhop_hades` it did
that, correctly, within half a second — and then drew a button offering to do it again.

The log is unambiguous about the sequence. A job was submitted at t=515.438 and finished at
t=515.969 with exit code 0, and it cannot have been the demo count, because that runs on its own
latch and never reaches `EndRun`. So the read happened, succeeded, and produced no cache file. That
is not a contradiction: it is precisely what a successful request for an **empty board** looks like.

`g_quick.gamemode` was initialised to 1 and there was no control anywhere on the page to change it.
Gamemode 1 is **surf**. The page had asked the surf leaderboard of a bhop map, been told the truth,
and had no way to say so — an empty result and a wrong question produce the same blank table. On
every map that is not surf, the front door was useless while looking, from the inside, like
everything working.

The map index cannot answer this and it is worth saying why, because it is the first place anybody
would look. `maps.txt` carries a `modes` column and for `bhop_hades` it reads
`1,2,3,5,6,7,8,9,10,11,12,13`. Momentum gives nearly every map a leaderboard in nearly every mode
and almost all of them are empty; a list of boards that *might* exist cannot pick the one that
does.

What can answer it, in order of how much each step actually knows:

1. **A choice made by hand for this map**, remembered in `quickpicks.txt`. First, and it has to be
   first — most maps worth looking at already have a board cached, so a cache that outranked the
   picker would be a picker that did nothing on exactly the maps where it mattered.
2. **A board already cached for this map.** Not a guess at all: the filename is
   `boards\<map>_g<mode>_t<type><num>.tsv`, so one directory listing with no file opened says which
   mode somebody already fetched this map in. It is what makes `bhop_telehop_theory` resolve
   without anything having to know that `bhop_` means anything.
3. **The map's name.** Momentum prefixes by discipline and the convention is near-universal:
   `surf_`, `bhop_`, `rj_`, `sj_`, `ahop_`, `conc_`, `defrag_`/`df_`. The climb family is
   deliberately absent — it has three modes (Momentum, KZT, 16-unit) and a `kz_` prefix cannot tell
   you which, so it returns nothing rather than being confidently wrong two times in three.
4. The setting, which is now a fallback rather than the answer.

And the message changed, because "nothing came back for this leg — it may simply have no runs" was
true of every empty stage board and wrong about the one case that actually happened. It names the
mode it asked, and when the map's name disagrees it names that too: *nothing on the surf board for
bhop_hades — it reads like a bhop map, so try bhop above.* Two indistinguishable outcomes, and the
line now carries the one fact that separates them.

### Two hundred and fifty to three thousand five hundred

The speed ramp ran 250 to 3500 u/s, and the energy ramp 0 to 4000, because those numbers had to be
*something*. They are about right for the top of a board. On the rest of it they are mostly wasted:
a run that lives between 400 and 1200 occupies a fifth of the ramp and comes out one colour, so the
mode that was supposed to show where speed was lost shows nothing at all.

Scaling to the runs actually on screen is therefore less a feature than a connection. `WrRun` has
carried `speedMin` and `speedMax` since the loader was written, with the comment *for
colour-by-speed* beside them, and nothing had ever read them. Energy cannot be precomputed the same
way — it is `z + |v|²/2g` and gravity is a live setting — so that one costs a pass over points, and
only for the mode that is on.

Two decisions worth naming. The sliders are **not** written back over: a slider that moves on its
own is a setting you can no longer hold, so the fitted numbers go to a separate `use*` set and
turning auto-scaling off restores the sliders by doing nothing at all. And rank scales too, into
`shownRank` rather than over `rank` — with four lines on screen, colour-by-rank should spend its
whole ramp on those four rather than on the nine thousand runs they were picked from, but the Runs
tab reports `rank` as a fact about the leaderboard and a number that changed when you unticked
something else would be a lie.

It is behind a dirty stamp, not recomputed per frame, for the reason written beside `rank` in
`wr_path.h`: the renderer asks for a run's colour once for the line, again for its name tag, its
ramp numbers, its checkpoints and its comparison ring.

### One ramp per leg, because a stage and the main track are not comparable

The version above had a hole in it, and the hole was the second thing you would try. Turn on
auto-scaling, enable a stage as well as the main track, and it silently stopped working: the code
noticed more than one leg was on screen and gave up, pooling every enabled run into one range.

Pooling is not a neutral fallback. Momentum cuts a map into legs and they do not live in the same
place — a stage of surf_utopia sits thousands of units above the one before it, a bonus is thirty
seconds where the main track is ninety. The union of two legs' energy bands is mostly the empty gap
between them, so each leg gets a fraction of the ramp and every line on it comes out one flat
colour. Which is precisely the failure auto-scaling exists to fix, reappearing one checkbox later,
with nothing on screen to say it had.

So there is a range per leg now — speed, energy and energy-relative, in three small fixed tables
keyed by `(trackType, trackNum)`. The renderer resolves one pair per run and puts it on
`WrPathDraw`, which already carried a per-run `startEnergy` for the same kind of reason; the ramp
lookup downstream is parameterised on a normalised `t`, so varying which `lo` and `hi` produce that
`t` is the whole of it. Two stages on screen are two ramps.

Rank went the same way, and there it is not a new idea at all — the on-screen key has read *placed
within each leg — a bonus cannot out-place a main run* since rank colouring shipped, and
`test_rank.cpp` exists because a 34-second bonus taking first place from a 52-second main track is
entirely plausible right up until you notice it is on the wrong line. The placing pass now groups by
leg in the same table as everything else.

**The cost is a promise, and it had to be given up out loud.** `WR_LINE_ENERGY`'s key says
*absolute, so the same colour means the same energy on every line* — that is the whole reason the
absolute mode exists next to the relative one, and it is **false** once two legs are scaled apart.
So the key stops saying it: with more than one leg scaled, the numeric labels become *lowest on its
own leg / the middle of that leg / highest on its own leg* and a footer says each leg has its own
ramp. A legend that keeps a promise the picture has stopped keeping is worse than no legend, because
it is the one place somebody would go to check.

Past sixteen legs at once the table is full and everything falls back to the pooled range — the old
behaviour, which is coarse and visible rather than wrong and silent. Your own live line is on no leg
and keeps the pooled range too; picking a stage for it would be picking one arbitrarily.

The pure part is `src/wr_scale.h`, `static inline` with the rest of this project's testable logic,
so `tests\test_scale.exe` links nothing at all.

One thing fell out of writing the key, and it had been wrong for as long as the key existed.
`SpeedColour` took a **value** and normalised it by the speed range — every caller, in every mode,
including the key's own swatches. So on an energy range of 0–4000 the middle swatch asked where
2,000 sits between 250 and 3,500 and drew that colour, which is not the middle of anything; the two
end swatches survived only because they clamp. The ramp takes a **position** now — one quantity per
mode, three modes, one gradient, and where a point falls in its own range is a number between 0 and
1. The lines are unchanged (their callers were already round-tripping a fraction out through the
speed range and back); the key's middle swatch is now the colour it claims to be.

### Downloading demos

There was no good way to see what existed. `surf_demise` has **9,104 runs on its main track**;
this machine had 52 of them, and nothing told you about the other nine thousand.

**Browsing costs nothing.** The game already keeps the whole catalogue on disk, in
`momentum\_cache`: an `MSML` header, two `u32`s giving the decompressed size and the map count,
then a raw zlib stream from offset 12 decompressing to JSON with every map's id, name and
leaderboard tiers — **2,135 maps** at the time of writing. The Maps tab lists all of it with what
you hold for each, and asks nothing of anybody's server to do so.

Reading it used to be Python's job, because it needs inflate and a JSON parser and the DLL had
neither. It is [src/wr_msml.cpp](../src/wr_msml.cpp) now: `tinfl_decompress_mem_to_heap` from a
committed copy of miniz, and a three-hundred-line cursor-style JSON reader in
[src/wr_json.cpp](../src/wr_json.cpp) that walks the twelve megabytes once and allocates nothing.
Neither adds an import — they are static C — so the Maps tab works with no interpreter installed
and the DLL's dependency list is unchanged.

**Fetching is opt-in.** Momentum's backend is open source and `GET /maps/:id/leaderboard` carries
`@BypassJwtAuth`, so it needs no account and no token. Each entry hands back an absolute
`downloadURL` and a `replayHash` — and that hash **is** the `.mtv` filename the game itself
stores, so working out what is missing is exact and free. Asking for the top fifty of a map you
have forty-nine of downloads one file.

The rules are self-imposed. Momentum's terms say nothing about automated access in either
direction, which makes this manners rather than permission:

- **Never automatic.** A button, every time — same policy as extraction. Nothing happens on a
  map change or in the background.
- **Off by default**, behind a checkbox next to the Steam one.
- **One request at a time**, with a pause between them and a cap per press. No parallel fetching.
- **Dedupe before fetching**, so a repeat costs one request.
- **A User-Agent naming the tool and this repo**, so operators can see who we are.
- **Written to `wrlines_data\demos\`, never into the game install** — the promise above still
  holds. The extractor gained a second search root rather than the downloader gaining write
  access to `momtv\`.

**Both halves live in the DLL now.** v0.6.0 moved reading a leaderboard into
[src/wr_api.cpp](../src/wr_api.cpp); v0.6.1 moved downloading into
[src/wr_fetch.cpp](../src/wr_fetch.cpp), which is the last thing in this project that reached the
network from anywhere else. A demo body now travels the same one file a leaderboard page does,
to an address the leaderboard reply itself supplied.

The cost of the first half is the one claim this project has had to give up. Until v0.5.1 the DLL
linked no HTTP client at all and `dumpbin /dependents` proved it in one line — five system DLLs,
none of them a network stack, "it reads memory and two files" verifiable rather than asserted.
`WINHTTP.dll` is the sixth name now. There is no version of this port that keeps that sentence,
and the release workflow's import assert changed in the same commit as this paragraph, the README
and `HOW TO USE.txt`, so none of them can quietly go stale.

What replaced it is narrower but still checkable in a couple of minutes, and
[src/wr_http.h](../src/wr_http.h) is written to be the first file a suspicious reader opens:

- `WinHttp` — the prefix every function in that API carries — appears in [src/](../src/) in
  exactly two files, `wr_http.cpp` and its header. 120 lines, one function, GET and nothing else.
  No POST, no cookies, no credentials, no scheme it did not start with.
- Every URL is built in one place from one constant host, plus the absolute `downloadURL` the
  server's own reply contained.
- It sends no identifier of any kind. The User-Agent names the tool and this repo and that is
  all. The one request that carries SteamID64s carries your *friends'*, because you pressed a
  button that says so.
- Nothing runs on a timer, at startup, or on a map change.

One consequence worth stating: downloaded demos carry other players' names and SteamID64s, the
same as the ones the game downloads. That is why `wrlines_data\` is gitignored.

**The two counts in the Maps tab are both yours.** `demos` is `.mtv` files on your disk and
`lines` is `.wrpath` files extracted from them; neither is what the leaderboard holds, and a map
you have nothing for shows a 0 rather than a blank. The server's own total costs a request per
map per track, so it lives on the **browse** button, which fetches one page, prints the
leaderboard with everything you already hold marked, and downloads nothing.

### Browsing a leaderboard, and reaching the slow end of it

**The fastest runs are the hardest to follow.** A 37-second `surf_demise` record is not a line you
can trace; the 79-second run at rank 9,108 is. Being able to ask only for "the top N" made the one
thing a learner wants unreachable.

The Board tab is as much of a map's leaderboard as you have asked for — scrollable, sortable on
every column, filterable by player, with a tick box per row and one download button.

**How a board is paged.** `take` is capped at 100 — `take=200` is an HTTP 400 — so any window
wider than that is fetched a page at a time. "Top 200" is two requests. `skip` works all the way
to the end: verified on `surf_demise`, `skip=9106` returns rank 9,107 of 9,108. So **the slowest
runs cost two requests**, because `totalCount` comes back with the first page — one request gets
the size and the next lands on the tail.

**The cache is a window, and it accumulates.** The whole board would be 92 requests for
`surf_demise` and **170 for `surf_boreas`** (16,993 runs) — a minute or more of sustained requests
per map, per refresh, against infrastructure somebody else pays for. So you fetch the top hundred,
then the slowest hundred, then ranks 4000–4020, and the table shows all three with the gaps
between them visible. You browse as much of the board as you actually looked at.

Rows are deduped on the **replay hash, not the rank**. Ranks move as runs land, so the same run
cached a week apart would otherwise sit in the file twice under two different numbers.

**Since v0.6.0 the DLL fetches this itself** — [src/wr_api.cpp](../src/wr_api.cpp) over
[src/wr_http.cpp](../src/wr_http.cpp) — so **Fastest 50** works with no Python installed. Two
things in that port are worth knowing because neither is obvious and both are one line wide:

- **The rank sort has to be stable over insertion order.** The reference writes
  `sorted(rows.values(), key=rank)` on a dict keyed by replay hash; Python dicts iterate in
  insertion order and `sorted` is stable, so two rows at the same rank come out in the order they
  were first seen — file order, then API order — and a re-fetched row keeps its original position
  while taking its new value. `qsort` is not stable. The port keeps an insertion-ordered array and
  breaks a rank tie by index, which is a one-line difference in a two-hundred-row file and an
  afternoon to find without a test for it.
- **A leaderboard is not a fixture**, so it cannot be an oracle either: fetch it twice an hour
  apart and you get two different files that are both correct. `--api-record` saves a real
  conversation once and `--api-replay` answers both implementations from it, which is what makes
  "did the port write the same bytes" a question with an answer. `tests\parity.ps1 -Verb board`
  runs a five-step accumulating sequence three ways — the reference live, the reference off the
  recording, then the port — and the middle pass exists to prove the recording itself is faithful
  before anything is concluded from it.

**Spread** samples N places evenly across the whole board, one request each — the cheap way to see
the shape of a 17,000-run leaderboard. Twenty requests gives you a fast one, a mid one and a slow
one to lay over each other, where caching that board in full costs a hundred and seventy.

**Downloading from the board costs no leaderboard requests at all.** The cache stores the
`downloadURL` the server itself handed back, so ticking rows and pressing download fetches only
the demo bodies. Capped at 64 per press, and the cap is stated rather than silently truncating.

Porting the download half at v0.6.1 turned up three things that are worth writing down, because
each of them fails without saying anything:

- **"Do I already have this run" is a set, and the names in it are not all hashes.** The reference
  builds a Python `set` of every `.mtv` basename across the game's tree and ours. The port's first
  version used a sorted array of 48-byte keys, which is roomy for a 40-character replay hash and
  not for the names the game gives *your own* recordings under `momtv\local\` — those run to 64
  characters here. Two of them alike for their first 47 collapse into one, and the visible effect
  of a false "we already have that" is a demo that never downloads and no message. It also has to
  be a set rather than a list: with `--into-game` the same run is in both trees, so a list counts
  it twice and prints that count to the user.
- **The copy into the game's folder has to carry the source's write time.** `shutil.copy2` does;
  `CopyFileA` does not, and the port does it by hand with `GetFileTime`/`SetFileTime`. That
  timestamp is how **take out** tells a demo we placed from one the game downloaded itself — see
  ADOPTION in [src/wr_intogame.h](../src/wr_intogame.h) — and when it breaks the download still
  works, the demo still plays, the lines still appear, and only the removal button quietly stops
  recognising anything fetched after that day. `tests\parity.ps1 -Verb fetch` asserts it to the
  tick on both sides, and `tests\test_fetch.exe` backdates a source by a year, which is the only
  way to tell a carried stamp from a fresh one that lands in the same second.
- **A recorded conversation with nothing in it is a valid recording.** The `--ranks` path's whole
  claim is that it makes *zero* leaderboard requests, so its tape is an index file with no
  entries — and the port refused to open one, which meant the single step most worth comparing
  was the one step that could not run. The reference tolerates it and only complains at the first
  unmatched request, so the port does too.

**Gamemode has to be picked, and the map cannot tell you.** Momentum gives nearly every map a
leaderboard in nearly every mode — all 546 surf maps in the local catalogue list twelve of them,
and most of those boards are empty. (`surf_demise` really does have 3 bhop runs.) The names come
from Momentum's own `Gamemode` enum rather than a guess: 1 surf, 2 bhop, 3 bhop HL1, 4 climb Mom,
5 climb KZT, 6 climb 16, 7 RJ, 8 SJ, 9 ahop, 10 conc, 11–13 defrag CPM/VQ3/VTG.

### Your friends' runs, which the site itself will not give you

Momentum's leaderboard **does** have a friends filter — and it answers **401 Unauthorized** without
an account, so it is not something the API hands out. That is exactly why it isn't an option.

Asking for **specific SteamID64s is not gated at all**, and that turns out to be strictly better.
Verified live:

```
&steamIDs=76561198273400282,76561197994615740,76561198027112507,...
  -> each player's run, at its TRUE global rank: 1, 1302, 2603, 3904, 9109
```

- Comma-separated. The repeated-parameter form is a 500.
- **200 ids in a 3,704-character URL works**, so 100 per request has room to spare.
- Ids with no run come back absent rather than as an error, so a friends list full of people who
  have never played the map costs one request.

The client half was already built: `wr_steam.cpp` holds a live `ISteamFriends` and calls it through
the flat C API by name, so enumerating your friends is two more `GetProcAddress` lines and no new
mechanism. It has to happen in the DLL because that is the half that is *inside the game* and has a
Steam connection; the fetch script does not and cannot. So the DLL writes `wrlines_data\friends.txt`
and the script reads it.

Found a friend at **rank 14,491 of 17,001** on `surf_boreas` in one request, without caching the
14,490 runs above them. There is also a **friends only** filter on the cached board, which is purely
local — the rows already carry each runner's SteamID64.

`friends.txt` holds your friends' SteamID64s, which is one more reason `wrlines_data\` is
gitignored.

### Watching what you download

Downloads normally land in `wrlines_data\demos\`, which the game knows nothing about — so you could
draw them as lines but not watch them. **Also put them where the game can play them** copies each
one into `momentum\momtv\online\<mapID>\` as well.

That is the game's own replay folder, under the game's own filename — the replay hash, which is
already what our copies are named. A **copy, not a move**: your own tree keeps its copy, so a game
cache clear cannot take your lines with it. Written as a temp file and moved into place, so the game
can never see a half-written replay.

**Off by default**, because it is the one thing here that writes into the game install, and the
promise at the top of this file is qualified rather than quietly dropped.

There is also a **send** button on each row of the Runs and Board tabs, which copies that one demo
in, and a count of how many of ours are currently sitting in that folder.

Removal is where the care goes. That directory is the game's, not ours — it holds replays the
game downloaded by itself, 4,268 of them on this machine — so a *clear* that removed `*.mtv`
would take all of it. Every file sent is written into `wrlines_data\into_game.txt` **before**
the copy, and **removal only ever touches paths in that file**. A demo the game downloaded is
not in the list and cannot be reached from the panel. The list is re-checked against disk each
time it is shown, so a game cache clear that took our copies with it shows up as a smaller
count rather than as a stale claim.

### Two replay folders, and which one the game reads

Sending demos looked broken, and every message the panel printed was true. Both tabs shared one
status string, so an answer about one run stayed on screen while you looked at another, and two
correct per-run answers read as the tool contradicting itself. Measured here:

| | |
|---|---|
| entries in `into_game.txt` | **0**, while 7 of the 16 files in `momtv\online\104` had been put there by us |
| `.wrpath` files with no `.mtv` anywhere | **35 of 1,749** — surf_me 12, surf_ispy 10, surf_fiellu 2 |
| `.wrpath` whose name is not a replay hash | **202 of 1,749** — your own local recordings |

The fetcher's `--into-game` writes the identical destination as the send button and never told the
manifest, so the panel said "none of ours", the **Remove ours** button never appeared at all, and
pressing send correctly answered *"the game already has that one"* while doing nothing. Those copies
are now **adopted** — but only when `wrlines_data\demos\<map>\<hash>.mtv` exists too, because that is
the proof they came from us. A demo the game downloaded has no counterpart in our tree, is never
adopted, and stays exactly as untouchable as before.

The other two states are now shown rather than discovered by pressing a button that cannot work. A
run whose `.mtv` was deleted in game leaves its `.wrpath` behind and lists for ever with nothing to
send: those rows say **no demo**. A run extracted from one of your own recordings says **yours** —
the game can already see it. That last case used to report *"no numeric map id — refresh the map
list"*, which was the wrong advice for 202 files, and the map id was never the problem: the name was.
`srcSha1` is the source `.mtv`'s filename **stem**, and yours look like
`104455274-surf_fiellu-1781797367-main-nrm-60.990`.

**What is still not known.** The game keeps replays in two trees:

```
momentum\momtv\online\<numeric map id>\<40-hex replay hash>.mtv      1,675 files, 290 dirs
momentum\momtv\local\<map name>\<user id>-<map>-<unix>-<track>-<style>-<time>.mtv
                                                                     2,591 files, 447 dirs
```

and its leaderboard panel has a tab for each — `Leaderboards_Error_NoLocalReplays` and
`Leaderboards_Error_NoDownloadedReplays` in `momentum_english.txt`. `engine.dll` carries the literals
`momtv/local`, `momtv/online` and a `momtv/local/*%s` glob.

So each row also has a **local** button, which puts that one demo in the local tree instead. It is
one press per demo rather than a default, because those are other people's runs going in among 2,591
of your own recordings, and it is recorded and removable like everything else.

### The Downloaded tab is not a listing of the folder

That was the open question above, and the game's own files answer it. Momentum ships its UI as
Panorama source, and `panorama/scripts/common/leaderboard.ts` declares:

```ts
export enum LeaderboardEntryType { INVALID = -1, LOCAL = 0, ONLINE = 1, ONLINE_CACHED = 2 }
export enum LeaderboardType { LOCAL = 0, LOCAL_DOWNLOADED = 1, TOP10 = 2, ... }
```

**`ONLINE_CACHED`** — an online leaderboard row whose replay is cached locally. So the Downloaded tab
enumerates *leaderboard rows that have a file*, not *files that exist*. A demo copied into
`momtv\online\<map id>\` lights up only a run the game had already listed, which is exactly why *"the
game already has that one"* and *"it is not in my list"* were both true at the same time. Copying
harder was never going to fix it.

**The way in is the one the game itself uses.** `panorama/scripts/pages/end-of-run/end-of-run.ts`
runs, on its own Watch button:

```ts
GameInterfaceAPI.ConsoleCommand(`mom_tv_replay_watch ${this.baseRun.filePath}`);
```

It takes a **path**. No leaderboard row, no list, nothing to agree with. `client.dll` carries both
`mom_tv_replay_watch "%s"` and `Invalid run metadata for replay file %s` — so the engine reads a
replay's metadata out of the file and the filename is not parsed for anything.

So every row now has a **watch** button. It copies

```
mom_tv_replay_watch "momtv/local/<map>/<hash>.mtv"
```

to the clipboard; paste it in the console and the demo plays, whatever the two tabs show. The path is
taken from where the file actually *is* rather than rebuilt from the hash, which matters for the 202
runs extracted from your own recordings: those are already in the local tree under a name the game
chose, and a hash-shaped path would have missed every one of them. If the demo is not yet anywhere
the game's filesystem can name, one copy goes into the local tree first — recorded and removable,
and only because the button was pressed.

WrLines still runs nothing. The command goes on your clipboard for you to paste; no console command
is executed, no cvar is set, and copying a file remains the most it ever does to the game.

**Names are not ASCII.** Printing them was: Python takes stdout's encoding from the locale, and
under the DLL that locale is cp1252, so the first alias outside it raised `UnicodeEncodeError`
and killed the download mid-run — after some demos had already been written. `surf_demise`'s
top 25 alone has Cyrillic, Hangul and a name built out of dingbats. Both streams are now UTF-8
with replacement, so printing a name cannot fail. The panel's font only has Latin glyphs, so an
unfamiliar alias shows as boxes there; the demo still lands under its hash, which is what it is
keyed on anyway.

### Save-loc times

`momentum\savedlocs.txt` is plain KeyValues with a `time` field per save-loc. It is `"-1"` in
**all 3239 entries across 261 maps** — the field exists and is never populated. So WrLines keeps
its own note in `wrlines_data\savelocs\`, keyed on position rather than index because indices
renumber when a save-loc is deleted. Load a save-loc and the clock returns to what it said when
you made it. The game's file is opened read-only and shared; nothing is ever written into the
game install.

A time is recorded when a save-loc is **created**, and only then. An earlier version stamped
whichever untimed save-loc you were standing near, which meant *walking past one timed it* — that
produced 111 stamps here and left surf_hades2 with twenty entries at the spawn, one per lap. That
rule cannot be loosened, and it leaves every save-loc made before WrLines existed permanently
untimed: **141 of 3239 have a time, across 11 of 261 maps.** A time that was never recorded cannot
be recovered and will not be guessed at, so those can be **typed in** instead, and are marked `*`
so a stated time is never mistaken for a measured one.

### The load that nobody noticed

The clock kept counting through a save-loc load, and the reason had nothing to do with the times.
The restore lived entirely inside `if (teleported)`, and `teleported` is raised in exactly one
place: when the camera moves more than **400 units between two consecutive frames**. Momentum
restores the *exact* stored origin — so loading a save-loc you are standing beside moves you a few
units, often none, no teleport fires, the match is never even attempted, and control falls straight
through to `g_elapsed += dt`. Practising a section by loading the same loc over and over is precisely
the case that could never work.

The threshold is not the thing to change: 400 is what keeps ordinary movement from resetting the
energy filters, and having exactly one teleport detector is a rule this file has broken before and
paid for. So there is a second, much narrower trigger beside it. After a load the camera's **x and
y** equal the save-loc's to a fraction of a unit, and that is a far stronger statement than any
distance test — at surf speed the camera crosses fifty units in a frame, so a one-unit circle is not
somewhere you arrive by moving.

It is **horizontal only**, deliberately. The obvious sharper test is `cam.z == pos.z + eyeHeight`,
and it is wrong twice: `eyeHeight` is a setting with a slider, fixed at 64 because that is the
standing view offset, and it is simply not 64 when the loc was saved or loaded ducked. Two
coordinates at sub-unit precision say everything a third would, and ducking cannot defeat them.

And it fires on the **rising edge**, because holding `+mom_savestate_load` parks you on the spot for
as long as the key is down and a level trigger would re-set the clock every frame — it would restore
correctly and then never advance.

Three things about *recording* a time were wrong in the same direction:

- `firstForMap` was inferred as "we hold nothing, so this is the first look". On a map with no
  save-locs yet that stays true after the first read and for ever, so the first save-loc you ever
  make on a map — the one you make while finding out whether the feature works — was never stamped.
- The file's timestamp was committed as seen *before* the "a read is already running" guard, so a
  change that arrived during a read was recorded as handled and then dropped. The file will not
  change again by itself, so it was never re-read. Two save-locs in quick succession is all it takes.
- `WrSavelocMatch` never filled `fromCps` or `ordinal`, and its caller declares the hit
  uninitialised. Latent rather than live, until the new matcher wanted `fromCps`.

A load on a save-loc with **no** time now says so, rather than doing nothing. That silence was half
the problem: it is indistinguishable from not having noticed the load, and 3,098 of 3,239 save-locs
here have no time, so the common case was a correct answer that looked like a broken one.

Two things it will not do, said outright. **Making** a save-loc where you stand looks identical to
arriving on one — same camera, new entry — so the table carries a version and a change to it on a
frame you have not moved is read as a creation, not a load. And **loading the same save-loc twice
without leaving its one-unit circle** is genuinely invisible: nothing observable changes, so the
clock is not put back the second time. Step off it and back and it works; that is what practising
actually looks like, but it is a limit rather than an oversight.

### A fail is not a load, and it was being read as one

The fix above shipped and the next report was *"a timer shows up for about a second when I fail, then
disappears"* — plus the graph still clearing. Those are one bug, and the log proves it without
needing to reproduce anything:

```
[237.985] energy: teleported back to the anchor, treating it as a restart
[238.110] timer: set to 5.47s (loaded a save-loc, without moving)
[244.281] energy: teleported back to the anchor, treating it as a restart
[244.281] timer: set to 5.47s (loaded a save-loc)
```

Two fails, and each restored 5.47 s from a save-loc kept **on the start pad** — which is an entirely
ordinary thing to keep there. The restore called `WrSavelocNoteRestore`, the HUD borrows the clock row
for two seconds when that note is fresh, and with the clock row off by default that borrowed row is
the only clock you ever see: a number appears when you fail and vanishes. And restoring also cleared
the `restart` flag, on the reasoning that *a save-loc is a more specific answer than "you are near the
start"*. It is not — `restart` is raised **only** when the landing is within 384 units of the anchor,
which is the start pad, so the specific answer was being read off the least specific event there is.
Clearing the flag skipped the block that holds the recording, so the graph was wiped exactly as
before. **A restart now outranks a save-loc**, which is what the velocity seed beside it had always
done.

The 125 ms between those two log lines is the other half. A fail does not put you *on* the pad, it
puts you above it and lets you fall — and the exact matcher is horizontal only over a 96-unit band, so
it fires on whichever frame the fall brings `z` into range, by which time the teleport flag has long
been consumed and "was this a teleport" answers no. So a restart now starts a short **quarantine**:
save-loc clock restores are refused until you have moved 96 units from where you landed, or three
seconds have passed, whichever comes first. Distance is the real test; the timeout only exists so
that standing on the pad and then deliberately loading a loc you keep right there still works.

### The stopwatch that threw the held line away

The hold from the previous release was correct and then undid its own work. Its last-resort release
was *"held, and the clock has passed three seconds"* — and the clock starts as soon as you are 32
units from the anchor, while a fail drops you **hundreds** of units from it, because the game
respawns you at the start trigger and the anchor is where the chased run's recording begins. So the
clock started on essentially the next frame and the buffer was thrown away three seconds after every
fail: before you have finished falling, let alone opened the panel.

The release is now positional, which is what was asked for in the first place. When the hold is
taken, the start zone you were put back in is latched — centre and radius — and the hold is released
when you leave that circle. The crossing edge stays the primary release, because it fires at the
right moment, when you leave the plane at speed; the circle is only for legs where the start machine
never arms. The latch is taken **first** and the hold only if it succeeded, so "held" implies "has
somewhere to leave" and a stranded recorder is not reachable.

`tests\test_live.cpp` drives all three of these against the real timer, recorder, zone fit and
save-loc table. Each was checked by putting the bug back: reverting the precedence, dropping the
quarantine, and restoring the stopwatch each fail exactly one assertion, and two of those checks
were rewritten when the first version of them survived the mutation — the landing geometry was wrong,
so the code under test was never reached.

### Your own line, after you fail

*Record my path* was being wiped by the fail trigger, not by the graph. The recorder clears its
buffer on a camera jump — right for a teleport — and failing a run drops you back on the pad, which
is a very long way. So the attempt was erased on that frame, and by the time you had opened the panel
to see what went wrong there were one or two points left and the graph drew nothing. It reads exactly
like the graph clearing itself every time you look at it. Nothing in the Graphs tab ever touched the
buffer.

A restart now **holds** the buffer instead: recording stops, what is there stays, and leaving the
start zone clears it and begins the next one. A hold rather than "keep appending", and that is not a
preference — a live point's `t` is the run clock, which is zeroed on a restart and again at the start
line, so appending across one would send the graph's time axis backwards, and `wr_profile` binary-
searches that axis.

The hold is only ever taken when a start zone is actually known, because leaving one is what releases
it — see the two sections above for how that release was got wrong first. While in there, the
recorder's own teleport threshold moved from 512 to **400**,
to agree with the one every other part of the tool uses — a 450-unit jump used to be a teleport
everywhere except here, which drew the straight bar across the map that the recorder exists to
prevent.

### The velocity was in the file all along

Loading a save-loc used to leave the energy readout showing **the energy you had when you failed**,
until a new velocity could be measured by differencing the camera — and held the banked gain/lost
figures for a further 0.9 s. WrLines has no entity access, so re-measuring looked like the only
option.

It was not. Momentum records the velocity it is about to restore, in the same file, and the parser
was reading only `pos` and discarding the rest. Measured across the 3239 save-locs here: `vel` is
present and finite in **100% of them**, `predictedVel` disagrees materially in **2**, and **62%
were saved above 250 u/s** — mid-surf, which is exactly where re-deriving it costs most. Unlike the
time beside it, this needs nothing WrLines wrote, so it works on save-locs made years before it
existed.

So the readout is **seeded** from the file and starts at the right number. But a value read from a
file is a claim about a moment nothing on this side witnessed, and a fast readout that is quietly
wrong would be worse than the slow one it replaced. Every seed is therefore checked against the
first velocity actually measured after it — **about 35 ms later, near enough the same instant to be
a fair comparison**, where half a second later would be comparing it against half a second of you
playing. A seed that disagrees is thrown out and the filters fall back to measuring, so a wrong
seed costs the 35 ms it was always going to cost and nothing more. Diagnostics counts the seeds and
the rejects; if that ratio is ever bad, the file is not saying what the game does and the feature
should be switched off.

**Held down, it holds.** Momentum's load key freezes you at the loaded position until you let go —
so the first velocity measurable after landing is *zero*, and the guard-rail above would compare the
file's answer against that zero and throw out a good seed, on every save-loc made at speed, which is
62% of them. While a seed is unjudged and the camera has not left where it landed, the loaded values
stay on screen and nothing is measured. The moment you move, the velocity window is **reset** before
measuring resumes: without that, the first measurement averages the frozen stretch together with the
new motion, reads far too low, and rejects a seed that was right all along.

Two things it deliberately refuses. A **restart** is never seeded — a fail trigger drops you at the
start, and keeping a save-loc on the start pad is ordinary, so the landing matches one and the
claimed speed would be applied to a player the game just stopped dead. And a `startmarks` entry is
never treated as a save-loc: that is a second list of positions in the same file, carrying a `pos`
and no `vel`, **16 of them across 12 maps here**, which had been silently read in as save-locs and
were eligible to be stamped with your clock.

### The spikes in the graph, and why they are not ducking

Runs show two-tick jumps in energy that come straight back. The obvious suspect is ducking, which
moves the camera height — and it is the wrong one. Measured across the fourteen surf_fiellu bonus-4
runs here: of 208 single-tick jumps over 150 units, **206 are in the speed term and 2 in height**,
and the component that moves is **vertical velocity** (median |Δvz| 283 u/s against |Δv| horizontal
of 31). **88%** of the excursions are back where they started within two ticks, and they **cluster by
map position** — eight of the fourteen runs spike within 300 units of the same spot.

They are not the player, and the proof is that they happen in **free fall, where energy is
conserved**. One run sits at 1476 units for twenty ticks, reads 2054 for exactly two, and returns to
1470, with its height falling smoothly at 21 units a tick throughout. Nothing gains 577 units and
gives them back inside 30 ms under gravity alone.

Library-wide that is 0.13% of 7.4 million ticks — but **76% of runs carry at least one**, and one is
enough to set the graph's vertical scale.

So the plotted curve is filtered with a **median of five**, which is the one filter that removes a
two-tick impulse *exactly* and passes everything else through untouched. An average would do the
opposite of what is wanted: smear the spike across 75 ms and round off the genuine ramp exits, which
are the steepest real features on the curve and the entire reason to look at it. Measured over the
whole library:

| | |
|---|---|
| two-tick impulses removed | **99.7%** (9,987 → 26) |
| samples changed by more than 25 units | **0.6%** — so 99.4% of the curve is passed through |
| a real step's height, median error | **0.0 units**; p90 15 |

It filters **the picture and never the stored run**, the toggle is in the Graphs tab, and the panel
says how many samples it moved — median 7 per run, 48 at the ninetieth percentile. It is not applied
to the coloured lines, where the same impulses are two points in several thousand: a colour blip
rather than a scale problem.

### Energy across a whole run

The crosshair readout answers "what is my energy now", which cannot distinguish a run that bled
its energy away evenly across a stage from one that threw the lot away at a single ramp. Those
finish identically and want completely different practice. The Graphs tab plots the whole curve.

Three decisions in it are worth stating, because each has a wrong answer that looks fine:

**Every curve starts at zero, at its own start.** Not a display preference — a stored run's
points are the player's **feet** and your live line is your **camera**, 64 units apart forever.
Subtracting each series' own first point cancels that exactly, and it is the more useful question
anyway: how much did this run lose from where it began, whatever height that was.

**Buckets keep a minimum and a maximum, not one sample.** A 38,751-point run has to become a few
hundred pixel columns somehow, and taking every Nth point is the obvious way. Measured against
the full-resolution curve across all **396 runs** on this machine, one sample per bucket hides a
**median of 1324 units** of excursion and tens of thousands on the worst runs — whole ramp exits
fall between samples, and the plot then says a surfer was smooth exactly where they were not. The
faint band around each curve is that lost detail; it is not shading.

**Distance never crosses a teleport.** A save-loc load moves the player across the map in one
sample, and adding that chord would put a kilometre of "path" on the axis where nobody travelled.
Stored runs use the break list found at load; your own line has none, so it gets the same
`WR_TELEPORT_UNITS` test directly. `tests\test_profile.exe` asserts both.

Time is offered as an axis but only for runs whose recovered clock passed its trust test — point
times are derived from the sample index, and on the worst map measured that ran from 0.36× to
10.32×. Runs that fail it are **left out of a time plot and counted on screen**, rather than
drawn wrong.

---

## Where a run starts

A `.mtv` starts recording before the run does. Measured across 500 demo headers on this
machine, `ticks × tick_interval` exceeds `run_time` by a **median of 2.06 seconds** — min 1.02,
max 4.11 — which is the player walking into the start zone while the recorder is already
running. The extractor keeps whatever of that it recovers and writes `t = index × dt` from
index 0, so **point 0 is somewhere in the approach**, and about three quarters of a second of
it survives on the finished line.

Everything downstream inherited that. The energy profile took its zero and both its axes from
point 0; the live energy readout anchored there; the clock started 32 units from there. One
root cause, four symptoms, and the visible one was "the graph looks wrong for a run that is
fine".

**How it is recovered.** Three routes, in decreasing order of authority:

| | |
| --- | --- |
| the extractor's stored index | matched against the demo JSON's `effectiveStartVelocity` — three full-precision floats, the same kind of fingerprint the split markers are anchored on. Does not care whether the point stream is complete. |
| the split markers | extrapolated back from the first split, whose `timeReached` the **game** measured. A measurement, not an inference. |
| the back-solve | `(pointCount-1) − runTime/tick`. Valid because the extracted stream ends *at* the finish: implied post-roll is a median 0.00 s over every file here that can be checked. |

Agreement between the last two is what earns *trusted*; neither available leaves the run at
index 0, which is exactly what it did before any of this existed.

**How often it works.** Over the 1,735 `.wrpath` files on this machine:

```
marker-derived pre-roll        median 0.72 s, p90 1.11, max 1.74, never negative
first-to-last marker drift     median 0.00 s; only 2.4% exceed 0.10 s
back-solve vs marker truth     99.4% within 0.05 s, 100% within 0.15 s
recoverable without re-extract 1,059 of 1,735  (61%)
```

The other 39% are files whose extracted point stream came out **incomplete**, and there the
back-solve does not go slightly wrong — it goes to −1089 seconds. That is why the plausibility
range is really a completeness test, and why those files are left alone rather than handed a
confident wrong number.

Re-extracting fixes them: `find_start` matches a velocity rather than counting ticks, so a
fragmented stream does not defeat it. Tested on 100 of the 676 files the DLL alone cannot
place, it **rescued 98**, at pre-rolls of 0.39–1.39 s. The extractor revision is bumped, so
the next *Extract new demos* re-does the library by itself.

**On disk.** The index goes in bytes `0xE8`–`0xEF` of the `.wrpath` header, which were
`minSampleDist` and `minSampleAngleDeg`: written as `0.0` since the format existed and never
read by anything. Verified zero across all 1,735 files here. So this needs **no format version
bump and no branch in the reader** — an older file reads 0, and 0 already means "unknown",
which is what an older file genuinely is.

---

## The start zone

Momentum starts your clock when you leave a mapper-tagged trigger brush having been on the
ground inside it. **This tool cannot see that brush.** It has no entity list, no netvars, no
sight of the game's timer — it knows a world-to-screen matrix found by scanning memory, a
camera solved out of it, and some files on disk. That is not going to change; the history of
the one time this project called into the engine is in `wr_engine.cpp`.

What it has instead is better than it sounds. Once each run knows where its *run* began, a map
with two hundred loaded runs carries **two hundred independent observations of where the start
is**, recorded by two hundred different players — and, unlike a trigger read, it comes with a
measured spread.

Two details decide the design:

- **The medoid, not the mean.** One run whose start was recovered wrongly lands in the middle
  of the map and would drag a mean thousands of units toward it. A medoid needs no scratch
  array, ignores that, and — the part that matters — is always a *real recorded player origin*,
  so anchoring to it is directly comparable with a run's own t = 0.
- **The trigger is a plane, not the edge of the circle.** `points[startIndex]` is where the
  timer started, which is on the way *out* of the real zone rather than in the middle of it.
  Fitting a circle to those points centres it on the exit. Firing when you leave that circle
  would start the clock late by radius ÷ speed — half a second at 256 units and 500 u/s, which
  is enormous here. So the circle only decides when you count as *standing in* the start; the
  moment that fires is crossing the plane through those points, outward.

Arming needs you inside, on the ground and slow, which is what stops a route that loops back
over the start line at speed from re-firing it. `WrEnergyOnGround` also fires at the apex of
every arc — its own header says so — and it is safe here precisely because it is only an arming
condition: an apex inside a 512-unit cylinder at under 200 u/s is a hop on the start pad, so
the wrong answer and the right answer are the same answer.

### Most starts are flat, and the runs can be asked

Two things were wrong with that, and both come from the same place: the fitted circle is centred
on the **exit** of the real trigger, so part of the actual pad is reliably outside it. Stand
there and nothing arms. And "slow" is not the only way to be at a start — strafing about at
400 u/s working up to the line is not standing still and is not a run in progress either, and
until v0.8.1 it armed nothing at all, so the anchor was never taken and the clock never zeroed
unless you first came to a stop.

The fix for both is one observation: **most starts are a flat pad, and that is a thing the runs
can be asked rather than assumed.** Every recovered start on a leg sits within a few units of one
horizontal plane when there is a pad, and does not when the starts are spread down a ramp. So
`WrStartZone` gains a fitted `planeZ` — the *median* member height, for the same reason the
centre is a medoid — and a `flat` flag, true when the p90 deviation from it is under 48 units,
about a step and a half in Source.

Three things read it. The arming circle is allowed to grow on a flat leg, because the floor band
is then a strong enough statement of where you are to carry a bigger circle. The energy anchor is
taken at `planeZ` rather than at `centre.z`, which is one member's height chosen for being
horizontally central and says nothing about the floor. And the "inside" test leads with the floor
band — it always did, but now it matters why: the band is the half fitted from where two hundred
clocks actually started, and the circle is the half known to be in the wrong place.

The second change turned out to need **less** code than planned. "Moving without changing height"
does not want a vertical-speed test of its own, because `WrEnergyOnGround` already *is* that
measurement and a better one: settled within six units of one height, at under 30 u/s vertical,
for a twentieth of a second. Adding a second threshold would have restated another module's
constant and gone quietly wrong the day that constant moved. What was left to bound was the
*horizontal* speed — on a map whose route comes back over its own start pad, everything on the
ground inside the floor band would otherwise arm, and crossing the plane outward would zero the
clock in the middle of a run.

`wr_energy.cpp` carries two ordering defects in its history and both had the same symptom: not a
noisy number, but a number whose **origin moved**. So anything that widens when the anchor may be
taken is pinned in both directions. `tests\test_start.exe` has a must-fire case — flat, grounded,
400 u/s, inside a fitted zone, arms — and two must-not-fire cases: the identical motion in a flat
corridor nowhere near a start, and the identical motion *inside* the start at 1600 u/s. Each
asserts `WrStartZoneHere()` explicitly, so a case cannot pass by being somewhere else than it
claims; and each burns the one-second post-reset settle first, so it cannot pass by being too
early. Removing the speed bound makes the third one fail, which is the only evidence that it is a
test rather than a decoration.

The circle, the way out and the trigger line are drawn in the world, with a band either side of
the line that is the **measured p90 spread** of where those clocks started. Anything that
infers a place from data ought to show its error bars.

With no runs loaded there are no zones, nothing fires, and everything falls back to the manual
anchor and the move-32-units clock that existed before. There is deliberately **no matching
finish detector**: a finish is a line crossed once at speed rather than a place you wait in, so
the same machinery would be far less reliable while looking equally confident.

---

## Linux

**It works, through Proton, and that is the answer rather than a shortcoming.**

There is no native Linux build to make. The Momentum install ships `bin\win64` and **zero
`.so` files** — checked on this machine — so there is no native Linux game for a native Linux
tool to attach to. Proton runs the Windows game, which is why the Windows DLL loaded into it
and worked. A `.so` would have nothing to hook.

What that means in practice:

- Inject **inside the same Wine prefix** as the game. `wrinject.exe` uses Toolhelp32,
  `VirtualAllocEx` and `CreateRemoteThread`, all of which Wine implements; run it through the
  game's own prefix (`protontricks`, or `WINEPREFIX=... proton run wrinject.exe`).
- The panel's **Diagnostics** tab reports `platform  Wine/Proton` when it detects one, because
  that changes what several other lines on that tab mean — which `d3d11.dll` is loaded, and
  whether a path the game printed is a Windows path or a `Z:` view of a Linux one.
- **Nothing has to be installed inside the prefix**, and that is new at v0.7.0. Until then the
  extractor was a Python script launched as a child process from inside the game, so it needed
  an interpreter on the *prefix's* `PATH` — and a Python installed on the Linux side is not on
  it. That was the single most confusing thing about running this under Proton, and it is gone:
  extraction happens inside the DLL, which is already inside the prefix by definition.
- Everything else is unchanged. The lines, the panel and the leaderboard have no platform
  surface at all; the whole D3D11 dependency lives in two files.

---

## Energy, and what is exact

Internally: `E = z + |v|² / (2g)`. Both terms are heights, so `E` is a height — how high you
would get if you converted everything you have into altitude. It is **absolute**, which is what
makes the comparison free: two players at the same spot with the same `E` have the same total
mechanical energy, so reading the record's energy at your position needs no alignment, just a
lookup.

Absolute `E` is the wrong thing to put on screen, though. Standing still on a pad 1888 units up
it reads 1888, which is true and useless. So what you see is measured from **the anchor** — the
start of the run you are chasing, or a point you set by hand:

```
E_rel = (your height − the anchor) + |v|² / 2g     0 when you are standing still on it
v_eq  = sqrt(2g · E_rel)                           the same figure written as a speed
```

**The map's shape cancels out of `E_rel` exactly.** Drop 18 000 units down a surf map and convert
every unit of it into speed and `E_rel` stays at 0; what it actually reads is the energy you
*wasted* getting there. On a descending map everybody reads negative — that is not you doing
badly, it is friction and imperfect ramp entries, and what matters is whether you are less
negative than the run you are chasing.

That is not a claim, it is measured. Across 51 `surf_demise` records, run time against `E_rel`
at the finish correlates at **−0.976** — near enough a straight line. The fastest run is the one
that wasted the least energy:

| run time | `E_rel` at the finish |
| --- | --- |
| 37.170 s | −2275 |
| 37.275 s | −2562 |
| 37.710 s | −3027 |
| 37.890 s | −3240 |

The reference is an **anchor**: set once from the start of the run you are chasing, or by hand,
and never re-armed by anything you do. It used to arm itself from a ground test, which is the
defect described above. A separate "since jump" figure in the Energy tab still uses ground
detection, and that is all ground detection is used for now.

| | |
| --- | --- |
| Loaded runs | **good to about 0.1 %.** `.wrpath` stores a velocity per point, but it is a central finite difference of positions rather than the engine's own vector — the netstream carries velocity floats and not at an offset that is reliable across runs. Checked against the exact velocities the demo records at its own checkpoints: 4.2–4.8 u/s out at ~3900 u/s |
| You, live | **approximate.** It only knows where the camera is, so your velocity is differenced from camera motion over a fixed 40 ms window and smoothed |

Single-frame differencing at 200 fps turns a two-unit view bob into a 400 u/s spike, hence the
baseline and the smoothing; expect a few percent of error and slight lag on sharp changes.

The camera is the eye, ~64 units above your feet. That offset **cancels out of `E_rel`**, because
the reference height is a camera height too — it only survives in the comparison against a run,
whose points are the player origin, and there is a slider for it. Gravity is a setting
(`sv_gravity`, default 800) because nothing here reads cvars.

The comparison also refuses to answer unless an enabled run is within 384 units of you. Momentum
records a separate demo per stage, so without that test it will happily compare you against a run
several thousand units away on a different stage and report a confident, meaningless number.

---

## How the DLL finds the camera

Strata Source is closed-source. The interface versions the shipped `engine.dll`
advertises — `VEngineClient015`, `VDebugOverlay004` — are ahead of every public SDK, and
the shipped `IVEngineClient` has **200 methods** where the public SDK has under 40, so no
vtable index can be taken on faith.

The original approach was to probe for `IVEngineClient::WorldToScreenMatrix`, with every
guard I could think of: code-pointer validation, a shadow `this`, distinct scratch
arguments, SEH, one call per frame, and a crash-resume blacklist. **It doesn't work, and
it can't be made to.** It located `GetScreenSize` correctly (index 38, against an SDK
index of 5) — and then killed the game, repeatedly, *without leaving a breadcrumb*. The
probe call returned normally and the process died about a second later somewhere
unrelated.

That is the one failure mode none of those guards can catch. There is nothing to
blacklist, because by the time it crashes the guilty call is long gone. A narrower probe
window doesn't help; a better-predicted one doesn't help.

**So it reads the matrix instead of calling for it.** The world→clip matrix is 16
floats sitting in the game's own writable memory, rewritten every frame. `wr_scan.cpp`
finds them:

1. Walk the writable committed regions of `engine.dll`, `client.dll`, `materialsystem.dll`
   and friends, in overlapping 256 KB chunks, at 4-byte stride.
2. At each offset, a cheap structural filter: in a view-projection matrix the w row is the
   unit view-forward vector, so its squared length is ~1. Almost no random float32 triple
   lands in `[0.09, 9.0]`, so the expensive test runs on a tiny fraction of offsets.
3. Survivors go through the full oracle, both row-major and transposed.
4. If module data comes up empty, sweep the rest of the process's writable memory (capped
   at 768 MB).

The oracle is closed-loop and needs no external data:

> 16 finite floats → w-row length in `[0.3, 3.0]` → rows 0, 1 and 3 orthonormal after
> normalising → `|row1| / |row0|` equal to the backbuffer aspect ratio → solve for the camera
> origin (the unique point projecting to `x = y = w = 0`, a 3×3 solve) → require it inside
> world bounds and not at the origin → **reproject a point 512 units down the view axis and
> require it to land within 1 % of screen centre** → and then require it to *move*: at least
> 96 units of travel across frames, with fewer than 5 % teleport-sized jumps.

Nothing that isn't a live view-projection matrix satisfies all of that. The last clause matters
as much as the rest: a constant projection matrix in `.data`, and about twenty identical
fixed-viewpoint matrices for skybox and cubemap passes, both pass every static test and produce
a line that snaps around the screen.

### When more than one candidate is right

The oracle identifies *a* world→clip matrix for this viewport. It cannot identify *the* one
the world you are looking at was drawn with, because a Source frame contains several — any
pass rendered from the player's own eye is orthonormal, has the right aspect ratio, solves to
a camera inside the world, reprojects to screen centre, changes every frame and follows the
player. The only thing that separates them is the field of view.

Picking the wrong one is subtle rather than obvious: the lines still track the world, they are
still correct where you are aiming, and they drift further off towards the edges of the screen.
It is easy to read as the tool being broken.

Two things follow from that, and both are deliberate:

- **A map change does not rescan** — it keeps the address, on probation. Rescanning re-entered
  the lottery every time, so the tool could come back from a map change using a different
  matrix than it went in with. (The old rescan also blocked the render thread for up to three
  seconds, from inside `Present`, during a level load. Restarts are asynchronous now.)
- **Diagnostics lists every candidate** with its address, field of view, aspect ratio and how
  far its camera has followed you, and lets you switch with one click. **Remember this one**
  writes it to `wrlines_data\wrlines_matrix.ini` as *module + offset* — never a bare address,
  since ASLR moves the module every launch — and the next launch adopts it in the first second
  instead of re-deriving it. It is still validated every frame, so a game update that moves it
  falls back to scanning rather than pointing the renderer at nothing.

Where a rule can be justified it is applied automatically: among otherwise equal candidates the
one whose aspect ratio matches the backbuffer most exactly wins, then the fewest
discontinuities, then the most ground covered. Two candidates with the *same* aspect and
different fields of view are both genuine, and nothing measurable from outside the engine says
which is which — hence the list.

Nothing is picked until a candidate has followed the camera for **512 units**. It used to be
96, and the log read `96 units travelled` — exactly the threshold, meaning the winner was
whichever candidate crossed the line first on a frame where they all crossed together, decided
by array order. There is nothing to rank on until they have had time to differ.

### Knowing when the address has died

Probation exists because an address can die in two ways, and one of them is invisible.

**It stops passing the oracle.** Easy to see, hard to judge: a level load produces exactly the
same long run of failures and that is not the address's fault. The grace period used to be 600
frames, described in the code as "a long grace period" for menus and loading screens. It is
not — a loading screen still presents at several hundred frames per second, so 600 frames was
1.5 seconds. In one logged session a good address was declared stale 1.39 s into a level load,
replaced from the leftover candidate list, declared stale again, and again: three swaps in a
second and a half, ending on a matrix that only worked on that one map. It is **15 seconds**
now, and seconds are the point.

**It stops being written.** Memory keeps its last contents, so a dead slot goes on passing the
oracle forever — valid, motionless, and completely dead. Nothing separates that from a player
standing still *except the company it keeps*: if other candidates are being rewritten, a world
is being drawn and this one is not part of it. Four seconds of that, and only after a map
change, and the address is given up.

That second rule is why a slow level load is safe: during a load nothing changes anywhere, so
no evidence accumulates and the clock does not run.

When an address is given up, every candidate's hit count, update count and travel is **reset**.
Those were accumulated on the previous level; leaving them in place is what let three duds be
chosen in a second and a half, each one instantly "eligible" on evidence that no longer
applied.

This logic is `wr_matrixlife.h` — deliberately free of Windows, D3D and engine types so it can
be run against scripted frames rather than only against a running game:

```
> tests\test_matrixlife.exe
the address survives the load          an 8 s load at 400 fps does not kill it        ok
the address does not survive the load  frozen while the world moves is caught         ok
how long that takes                    caught after 4.00 s (budget 4.0 s)             ok
standing still is not death            ten minutes motionless is never called death   ok
the menu                               ten minutes at the menu triggers no re-pick    ok
the frame rate does not change it      60 fps 4.017 s / 300 fps 4.003 s / 1000 4.001  ok
```

That last line is the regression the frame-counted version would have failed by 16×.

This is strictly safer than probing, not just differently risky: **scanning never writes
and never transfers control**, so it cannot corrupt engine state. Every read goes through
`ReadProcessMemory` on our own process, which returns `false` on an unmapped page instead
of raising — a region being freed mid-scan is a non-event. The worst case is finding
nothing.

Probing is still in the build, **off by default**, behind a checkbox in Diagnostics, with
a manual single-index box (which calls exactly one method rather than dozens). It is there
for the case where scanning genuinely comes up empty.

Screen size comes from the DXGI swapchain description, not from the engine.

**The current map is not obtained from the engine either.** The engine rewrites
`momentum\demoheader.tmp` on every map load; it is a `CSVCMsg_ServerInfo` protobuf whose
field 16 is the map name. It is read from there and validated against an installed `.bsp`
before being believed. That replaced an earlier attempt to probe `GetLevelName`, which had
turned out to be `IsInGame` at the index the SDK suggested.

All of this is visible live in the **Diagnostics** tab: the address the matrix was found
at, what the oracle saw, how many candidates are still alive, how much memory was scanned,
and the log tail.

---

## If it breaks

Everything goes to `wrlines_data\wrlines.log`, flushed on every line.

- **Panel never appears** — check the log for `Present @ ...`. If the first bytes there
  are a jump, something else (Steam overlay, RTSS) hooked DXGI first.
- **No lines, and Diagnostics says the scan found nothing** — load into a map first; the
  oracle needs a real view to confirm against, and there is no world→screen matrix at the
  main menu. Then **Re-scan**. If it still finds nothing, the fallback is Diagnostics →
  *Enable vtable probing* → type an index → *Try this index*. Best estimate is **69**
  (observed `GetScreenSize` 38, minus SDK 5, plus SDK 36).
- **Lines lag the camera by a frame** — the scan picked a stale copy. Hit **Re-scan**;
  ranking prefers candidates that change every frame, and a re-scan re-rolls the choice.
- **A game update moved everything** — nothing is hardcoded to an address. The scan runs
  fresh every launch, so an update that moves the matrix costs a second of scanning and
  nothing else.

---

## Layout

```
build.bat           vcvars + cl.exe
injector.cpp        -> wrinject.exe
dllmain.cpp         entry point, per-frame ordering, the hotkey thread
wr_common.h         Vec3 / VMatrix / paths
wr_log              log file + ring buffer for Diagnostics
wr_pe               PE section walk -> "is this pointer code in that module"
wr_probe            the safe-call layer (shadow this, scratch args, SEH, blacklist)
                    -- fallback only, off by default
wr_scan             read-only memory search for the world->screen matrix
wr_engine           the oracles, camera solve, map name from demoheader.tmp
wr_steam            steam_api64 by name -> persona names and avatar textures
wr_energy           E = z + v^2/2g, live sampling, ground/jump detection
wr_hook             swapchain vtable, Present/ResizeBuffers, window proc
wr_imgui            our own ImGui context, separate from the game's
wr_render           projection, near-plane clip, LOD, polylines, markers, tags, HUD
wr_path             .wrpath loading, run store, live recording
wr_profile          energy against distance/time per run, for the Graphs tab
wr_maps             the map catalogue and what is on disk for each
wr_board            a map's leaderboard, as much of it as you asked for
wr_savelocs         our own times for the game's save-locs
wr_timer            the run clock
wr_limit            the frame cap
wr_extract          counting unextracted demos, and cmd_extract: the demo
                    walk, the work list, the progress lines, the failure record
wr_peek             which map each demo is for, remembered on (path, size,
                    mtime) so the work list is not six thousand file opens
wr_mtv              the .mtv container and Valve-LZMA -- bytes, no floats
wr_dp               the candidate scan, the dynamic program and the scoring.
                    NO WINDOWS HEADERS: the one place where "does this produce
                    the same numbers as the reference" is the only question
wr_demo             one .mtv -> one .wrpath: the run's own JSON, the split
                    markers, where the RUN starts as opposed to the recording
wr_jobs             the worker pool, the memory budget, cooperative cancel
wr_json / wr_msml   a hand-written JSON reader, and the game's cache container
wr_http / wr_api    the only file that reaches the network, and what it asks
wr_fetch            downloading demos, and the copy that keeps its timestamp
wr_intogame         copies into the game's replay folder, and the manifest that
                    makes them the only thing removable from the panel
wr_settings         one registration table, walked by both the reader and the
                    writer -- see "Settings, and where they live"
wr_quick            the one-page panel on DELETE: legs, the top runs of one,
                    and the tick -> fetch -> extract -> draw chain
wr_matrixlife.h     when a chosen matrix has died -- pure logic, tested
wr_pacing.h         when the next frame may be presented -- pure logic, tested
wr_budget.h         gross gain/loss without counting noise -- pure logic, tested
wr_stress.h         the air-strafing ceiling and efficiency -- pure logic, tested
wr_quick.h          the quick page's chain, as a decision, and which leaderboard
                    a map's name implies -- pure logic, tested
wr_scale.h          one colour range per leg of a map -- pure logic, tested
wr_ui               the full panel on INSERT
tests\              standalone harnesses -- tests\build.bat builds and runs all
                    of them. Most link the real .cpp files, because the
                    defects they cover were in those files rather than in the
                    headers. test_live links the timer, the recorder, the zone
                    fit and the save-loc table together, because everything it
                    checks is an interaction BETWEEN them: a fail that must hold
                    the recording rather than wipe it, a fail that must not be
                    read as a save-loc load, and a save-loc load that does not
                    move the camera far enough to look like a teleport. Its
                    three newest checks were each verified by putting the bug
                    back -- two of them survived that and had to be rewritten,
                    because the landing geometry never reached the code.
```

Everything the tool writes lives under `wrlines_data\`, next to the DLL:

```
paths\<map>\*.wrpath      extracted run paths
paths\<map>\_failed.txt   demos that could not be extracted, and why
wrlines.log               everything, flushed per line
wrlines_offsets.ini       remembered vtable indices (probing only, off by default)
wrlines_matrix.ini        remembered world->screen matrix, as module + offset
into_game.txt             demos copied into the game's replay folder -- the only
                          thing the removal button is allowed to delete
settings.cfg              the panel's settings; display only, safe to share
imgui.ini                 the panel's own window position and size
savelocs\<map>.txt        our own time at each of the game's save-locs
demos\<map>\*.mtv         demos fetched from the public leaderboard
boards\                   cached leaderboard pages
friends.txt               your friends' SteamID64s, for the friends filter
maps.txt                  the map catalogue
```

Everything in there apart from `settings.cfg` and the two `.ini` files carries other people's
names, SteamID64s or run data, which is why the whole directory is gitignored.

---

## Licence

MIT — see [LICENSE](../LICENSE).

Dear ImGui is MIT (Omar Cornut and contributors). MinHook is BSD-2-Clause (Tsuda Kageyu).
Neither is redistributed here; both are cloned at build time.

Not affiliated with or endorsed by Momentum Mod or Strata Source.
