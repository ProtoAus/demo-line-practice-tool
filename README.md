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

**1. Generate paths for a map.** From this folder:

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
- Split markers are only drawn when the anchoring passed its confidence check. Wrong
  markers are worse than no markers.

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
wr_ui               the panel
```

---

## Licence

MIT — see [LICENSE](LICENSE).

Dear ImGui is MIT (Omar Cornut and contributors). MinHook is BSD-2-Clause (Tsuda Kageyu).
Neither is redistributed here; both are cloned at build time.

Not affiliated with or endorsed by Momentum Mod or Strata Source.
