# Demo Line Practice Tool

Draws the path other players' runs took as a line in the world, in the Momentum Mod map you
are currently playing. Follow the line instead of memorising a replay.

It reads the `.mtv` demos the game has **already downloaded** for you — and, if you ask it to,
fetches more from Momentum's public leaderboard — and turns them into a route you can see while
you surf, with
names and avatars on the lines, speeds at the bottom of every ramp, and a live energy readout
that tells you where you are wasting momentum.

Two halves:

| | |
| --- | --- |
| `wrpath_extract.py` | offline. Reads the `.mtv` demos the game already downloaded and writes small `.wrpath` files. |
| `wrlines.dll` | in-game. Reads `.wrpath`, draws the lines, and gives you a panel on **INSERT**. |

The DLL never parses `.mtv`. It only ever reads a simple, versioned, CRC-checked array of
points.

> **Naming:** the project is *Demo Line Practice Tool*; the binary and the source files kept
> their working name, `wrlines`. Nothing depends on that — it is just what the files were
> called while it was being written.

**Requires:** Windows x64, Momentum Mod Playtest (Strata Source), D3D11 or DXVK, Python 3.8+
for the extractor. There is no 32-bit path anywhere in the game, so both binaries are x64.

---

## Quick start

```
git clone --depth 1 --branch v1.91.9b https://github.com/ocornut/imgui imgui
git clone --depth 1 https://github.com/TsudaKageyu/MinHook minhook
build.bat

python wrpath_extract.py --list                 # what demos you have
python wrpath_extract.py --map surf_demise      # extract them

REM launch the game, load the map, then:
wrinject.exe
```

Press **INSERT** in game. Tick runs in the Runs tab. **ESC** closes the panel.

---

## What it does

- Finds every downloaded run for the map you are on and lists them by time.
- Draws any number of them at once, each in its own colour, so you can compare routes.
- **Name and avatar tags on the lines**, so with a dozen runs enabled you can tell whose
  route is whose. Anchored where each line crosses the fade distance, so they spread along
  different lines instead of piling up, and de-overlapped rather than drawn on top of
  each other.
- **Numbers at every turning point** — speed, energy, time or your delta, at the bottom of each
  ramp and at the top of each arc, in each run's own colour. A bottom says what a line carried
  *through* the ramp; a top says what it bought with it, and reading only one of the two tells
  you a surfer lost speed without telling you whether they got height for it.
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
- Puts **split markers** on the line where each checkpoint was reached, labelled with
  the split time.
- Optional **colour-by-speed** — blue slow through to red fast, which for surf and bhop
  is the mode you actually want.
- Records **your own path** live, so you can see your line next to theirs.
- Distance culling, distance fade, screen-space decimation and per-run point budgets, so
  eight full runs on screen cost a fraction of a millisecond.
- Holds up to **256 runs** per map. Measured across a 4095-demo library covering 475 maps,
  the busiest has 221 — so that is the whole of the largest map at once, and the files are
  loaded a few per frame so a big map does not stall on load.
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
- **It writes nothing into the game install**, sets no cvars, and never touches
  `sv_cheats`. Everything it writes lives in `wrlines_data\` next to the DLL.
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

Neither is committed to this repository. Clone them into the source folder before building:

```
git clone --depth 1 --branch v1.91.9b https://github.com/ocornut/imgui imgui
git clone --depth 1 https://github.com/TsudaKageyu/MinHook minhook
```

- **Dear ImGui** (MIT) — left exactly as cloned; only `imgui{,_draw,_tables,_widgets}.cpp`
  and the dx11 + win32 backends are compiled. Build settings live in `wr_imconfig.h`, pulled
  in with `/DIMGUI_USER_CONFIG`, so the vendored tree is never edited.
- **MinHook** (BSD-2-Clause) — built with `hde64.c` rather than `hde32.c`, since the target
  is 64-bit.

### Python

The extractor needs no packages for most demos. The very largest ones use zstd bodies:

```
pip install zstandard        # optional; without it those demos are skipped, not failed
```

---

## Use it

### Extracting from inside the game

The Runs tab counts the demos you have downloaded for the map you are standing in, how many
are already extracted, and how many are new — then offers a button to run the extractor
without leaving the game. Output streams into the panel and the lines appear when it finishes.

The count is exact rather than a guess, and cheap: a demo's map name lives at offset `0x10` of
the `MMTV` header, and the extractor names its output after the source file's basename, so
"is this demo for this map, and has it been done" is one 80-byte read plus one file-exists
check. Measured on a 4095-demo library across 475 maps, that reads every file correctly.

It runs **only when you press the button** — starting a python process off a map change would
launch a program behind your back and then compete with the game for CPU while you play. The
process runs at below-normal priority, and `--skip-existing` means pressing it a second time
costs seconds rather than minutes.

Needs `py.exe` or `python.exe` on `PATH`; the panel says which it found, or that it found
neither.

The panel is there whether or not the map has any paths yet. A map with nothing extracted is
exactly when you need the button most.

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
processes. `--jobs` defaults to all cores but two: the in-game button starts the script at
below-normal priority so it loses any fight with the game, but priority does not help if every
core is busy. `--jobs 1` forces serial.

There is also a `--timeout`, 180 s per demo by default. The dynamic program is quadratic in the
worst case and some demos genuinely take a minute; serially that reads as "it did four quickly
and then stopped". A demo that hits the limit is recorded as an ordinary failure, so it is not
paid for twice.

**1. Generate paths for a map**, if you would rather use a terminal. From this folder:

```
python wrpath_extract.py --list                        # what demos you have
python wrpath_extract.py --map surf_demise             # extract them
python wrpath_extract.py --map surf_demise --verify    # extract but write nothing
```

Roughly 0.7 s per demo. Files land in `wrlines_data\paths\<map>\`.

If your game is not at the default `C:\Program Files (x86)\Steam\steamapps\common\Momentum
Mod Playtest`, pass `--game <path>`.

**2. Launch the game**, then:

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

- **zstd bodies are skipped** unless you `pip install zstandard`. That is ~142 of the
  4035 demos on one test install, all of them the very largest files.
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

### Downloading demos

There was no good way to see what existed. `surf_demise` has **9,104 runs on its main track**;
this machine had 52 of them, and nothing told you about the other nine thousand.

**Browsing costs nothing.** The game already keeps the whole catalogue on disk, in
`momentum\_cache`: an `MSML` header, then raw zlib from offset 12, decompressing to JSON with
every map's id, name and leaderboard tiers — **2049 maps**. The Maps tab lists all of it with
what you hold for each, and asks nothing of anybody's server to do so.

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

None of it lives in the DLL. It links no HTTP client and no zlib, and `dumpbin /dependents` is
checked on every build precisely so "it reads memory and two files" is verifiable rather than
asserted — the import list is still five system DLLs. The work is flags on `wrpath_extract.py`,
which the DLL already knew how to launch and stream.

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

**Spread** samples N places evenly across the whole board, one request each — the cheap way to see
the shape of a 17,000-run leaderboard. Twenty requests gives you a fast one, a mid one and a slow
one to lay over each other, where caching that board in full costs a hundred and seventy.

**Downloading from the board costs no leaderboard requests at all.** The cache stores the
`downloadURL` the server itself handed back, so ticking rows and pressing download fetches only
the demo bodies. Capped at 64 per press, and the cap is stated rather than silently truncating.

**Gamemode has to be picked, and the map cannot tell you.** Momentum gives nearly every map a
leaderboard in nearly every mode — all 546 surf maps in the local catalogue list twelve of them,
and most of those boards are empty. (`surf_demise` really does have 3 bhop runs.) The names come
from Momentum's own `Gamemode` enum rather than a guess: 1 surf, 2 bhop, 3 bhop HL1, 4 climb Mom,
5 climb KZT, 6 climb 16, 7 RJ, 8 SJ, 9 ahop, 10 conc, 11–13 defrag CPM/VQ3/VTG.

**Names are not ASCII.** Printing them was: Python takes stdout's encoding from the locale, and
under the DLL that locale is cp1252, so the first alias outside it raised `UnicodeEncodeError`
and killed the download mid-run — after some demos had already been written. `surf_demise`'s
top 25 alone has Cyrillic, Hangul and a name built out of dingbats. Both streams are now UTF-8
with replacement, so printing a name cannot fail. The panel's font only has Latin glyphs, so an
unfamiliar alias shows as boxes there; the demo still lands under its hash, which is what it is
keyed on anyway.

### Save-loc times

`momentum\savedlocs.txt` is plain KeyValues with a `time` field per save-loc. It is `"-1"` in
**all 3213 entries across 260 maps** — the field exists and is never populated. So WrLines keeps
its own note in `wrlines_data\savelocs\`, keyed on position rather than index because indices
renumber when a save-loc is deleted. Load a save-loc and the clock returns to what it said when
you made it. The game's file is opened read-only and shared; nothing is ever written into the
game install.

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
wrpath_extract.py   .mtv -> .wrpath, offline
build.bat           vcvars + cl.exe
injector.cpp        -> wrinject.exe
dllmain.cpp         entry point, per-frame ordering, INSERT hotkey
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
wr_extract          counting unextracted demos, running the extractor
wr_matrixlife.h     when a chosen matrix has died -- pure logic, tested
wr_pacing.h         when the next frame may be presented -- pure logic, tested
wr_budget.h         gross gain/loss without counting noise -- pure logic, tested
wr_stress.h         the air-strafing ceiling and efficiency -- pure logic, tested
wr_ui               the panel
tests\              standalone harnesses -- tests\build.bat builds and runs all
                    five. test_energy, test_profile and test_board link the
                    real .cpp files, because the defects they cover were in
                    those files rather than in the headers.
```

Everything the tool writes lives under `wrlines_data\`, next to the DLL:

```
paths\<map>\*.wrpath      extracted run paths
paths\<map>\_failed.txt   demos that could not be extracted, and why
wrlines.log               everything, flushed per line
wrlines_offsets.ini       remembered vtable indices (probing only, off by default)
wrlines_matrix.ini        remembered world->screen matrix, as module + offset
```

---

## Licence

MIT — see [LICENSE](LICENSE).

Dear ImGui is MIT (Omar Cornut and contributors). MinHook is BSD-2-Clause (Tsuda Kageyu).
Neither is redistributed here; both are cloned at build time.

Not affiliated with or endorsed by Momentum Mod or Strata Source.
