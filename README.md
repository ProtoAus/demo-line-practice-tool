# Demo Line Practice Tool

Draws the path other players' runs took as a line in the world, in the Momentum Mod map you
are currently playing. Follow the line instead of memorising a replay.

It reads the `.mtv` demos the game has **already downloaded** for you — no scraping, no API,
nothing you don't have on disk — and turns them into a route you can see while you surf, with
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
- **Horizontal speed at the bottom of every ramp** — the points where a surf line either
  kept its momentum or did not — in each run's own colour.
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
- **It reaches outside your machine in exactly one place.** Name/avatar tags ask the Steam
  client to look up each runner's persona and picture — the same request a scoreboard makes.
  Your local avatar cache only ever holds people Steam already had reason to know about
  (typically one file: yours), so there is no offline route to the pictures. It is a
  checkbox in Diagnostics; with it off, tags fall back to the name already stored in the
  `.wrpath` and a coloured dot, and nothing leaves the process.
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
same latch also froze the velocity vector and the live line's efficiency colouring.

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

Which means the number cannot be made to "slowly rise" while you play well: on a descending surf
map `E_rel` falls for everyone. The figure that rises when you are doing well is the **gap**
against the run you are chasing. The panel now says so instead of implying otherwise.

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
+13142 units/s, up to 350× the ceiling — so anything past 3× is drawn neutral rather than as
perfect play.

### Save-loc times

`momentum\savedlocs.txt` is plain KeyValues with a `time` field per save-loc. It is `"-1"` in
**all 3213 entries across 260 maps** — the field exists and is never populated. So WrLines keeps
its own note in `wrlines_data\savelocs\`, keyed on position rather than index because indices
renumber when a save-loc is deleted. Load a save-loc and the clock returns to what it said when
you made it. The game's file is opened read-only and shared; nothing is ever written into the
game install.

---

## Energy, and what is exact

Internally: `E = z + |v|² / (2g)`. Both terms are heights, so `E` is a height — how high you
would get if you converted everything you have into altitude. It is **absolute**, which is what
makes the comparison free: two players at the same spot with the same `E` have the same total
mechanical energy, so reading the record's energy at your position needs no alignment, just a
lookup.

Absolute `E` is the wrong thing to put on screen, though. Standing still on a pad 1888 units up
it reads 1888, which is true and useless. So what you see is measured from **the last ground you
were standing on** — on a surf map that is the start pad, because you never touch ground again:

```
E_rel = (your height − that ground) + |v|² / 2g     0 when you are standing still on it
v_eq  = sqrt(2g · E_rel)                            the same figure written as a speed
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

The reference is the last ground you were **settled on**, not specifically one you jumped from:
walking off a ledge has to arm it too. The camera has to be vertically steady for a few frames,
so a ramp — where vertical speed is large and constant — never counts as ground. A separate
"since jump" figure in the Energy tab does still require a jump-sized upward exit.

| | |
| --- | --- |
| Loaded runs | **exact.** `.wrpath` stores a real velocity per point |
| You, live | **approximate.** It only knows where the camera is, so your velocity is differenced from camera motion over a few frames and smoothed |

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
wr_limit            the frame cap
wr_extract          counting unextracted demos, running the extractor
wr_matrixlife.h     when a chosen matrix has died -- pure logic, tested
wr_pacing.h         when the next frame may be presented -- pure logic, tested
wr_ui               the panel
tests\              standalone harnesses -- tests\build.bat builds and runs all
                    three. test_energy links the real wr_energy.cpp, because the
                    teleport latch was in that file rather than in the headers.
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
