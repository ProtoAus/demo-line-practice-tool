# Demo Line Practice Tool

Draws other players' runs as **lines in the world**, in the Momentum Mod map you are playing.
See the route, see where they carried speed and where you don't.

![Six views: aiming at a line on surf_hades2 bonus 4, watching a replay on surf_colin_blaster_69000, the Graphs tab on surf_hades2 bonus 4, the Runs list on surf_utopia, the Board tab downloading from surf_tropic's leaderboard, and the Maps tab on surf_zoomathon](screenshots/tour.jpg)

![Runs drawn on surf_fiellu bonus 4, coloured green where strafing is adding energy and red where it is being lost](screenshots/lines.jpg)

---

## Get it running

1. **[Download the latest release](https://github.com/ProtoAus/demo-line-practice-tool/releases/latest)** — the `.zip`.
2. **Unzip it** anywhere you like.
3. **Start Momentum Mod and load into a map.**
4. **Double-click `wrinject.exe`.**
5. **Press `INSERT`** in game.
6. **Board** tab → tick **Allow downloading** → press **Fastest 50** → tick the rows you want →
   **Download**. That is the map's real leaderboard, and it is where the demos come from.
   Nothing reaches the network until that checkbox is on.
7. **Runs** tab → **Extract new demos**. Wait for it. The lines appear when it finishes.

Step 7 also picks up whatever demos the game had already downloaded by itself, so it is worth
pressing even if you skip step 6. When there is nothing new to do it reads **Re-run extractor**
instead — same button.

> **Downloading** and **extracting** still need **Python** (`py.exe`, `python.exe` or
> `python3.exe` on `PATH`) — the **Download** half of step 6 and all of step 7 run through
> `wrpath_extract.py`, which ships next to the DLL. The panel tells you which interpreter it
> found, or that it found none. This is being folded into the DLL a piece at a time and the
> requirement will go away; the Maps tab and the leaderboard itself no longer need it, so
> **Fastest 50** works with no Python installed at all.

---

## Keys

| key | |
| --- | --- |
| `INSERT` | show / hide the panel |
| `ESC` | close it |
| `Page Down` / `Page Up` | the box at your crosshair — next / previous mode |
| `Home` | *whose line am I looking at* — off by default |
| `End` | the corner block — off by default |

All but `INSERT` are rebindable, and the **About** tab lists them as they are currently bound.
They are **read, not swallowed**, so if one collides with something you have bound, the game still
acts on it too.

---

## What you get

**Colour the lines by whatever you are chasing** — strafing efficiency, raw speed, energy, or where
each run placed. Numbers at every top and bottom say what a line carried *through* a ramp and what
it bought with it. Aim at a line (`Home`) and it tells you whose it is and how you compare at that
exact point.

**Energy across a whole run, plotted.** A line that sags gently was leaking everywhere; a line with
one cliff in it lost the lot at one ramp — and only one of those is worth practising the same way.
Your own last attempt is on there too, and it survives failing, so you can look at what went wrong
*after* it went wrong.

Also worth finding:

- **Save-loc times.** The game has a `time` field for save-locs and never fills it in, so this keeps
  its own. Load a save-loc and your run clock goes back with it.
- **A live strafe gauge** — `Page Down` to it. Green to red on how close you are to the most energy
  air strafing could physically add.
- **Watch any demo you have.** Press **watch** on a run and paste the command it copies into the
  console. Don't hunt for it in the Downloaded tab; that tab is built from leaderboard rows, not
  from the folder, so a demo you dropped in only shows up if the game had already listed it.
- **Your settings stay put**, in `wrlines_data\settings.cfg`, panel position included.

---

## Fair play

This is an injected DLL. It **reads** memory rather than writing it, sets no cvars, runs no console
commands, and never touches `sv_cheats` — but injected code may still invalidate a submitted run
under Momentum's own rules. **If you care about a run counting, don't have this loaded while you set
it.**

## Why your antivirus may flag this

`wrinject.exe` does classic remote-thread injection — `OpenProcess`, `VirtualAllocEx`,
`WriteProcessMemory`, `GetProcAddress(LoadLibraryA)`, `CreateRemoteThread`. That is about sixty
lines of [src/injector.cpp](src/injector.cpp), and it is also, precisely, how a lot of malware
loads itself. Defender cannot tell the two apart from the bytes alone, and it is not being stupid.

Inside the game, `wrlines.dll` installs inline detours on D3D11 `Present` and `ResizeBuffers` with
MinHook — which allocates executable memory for its trampolines — walks its own process's memory
with `VirtualQuery` looking for the world-to-screen matrix, and polls `GetAsyncKeyState` on a timer
for the hotkeys. All three are ordinary for a game overlay. All three are also on every heuristic's
list.

What it does **not** do, which is most of what actually separates the two:

- no registry access at all — `ADVAPI32` is not even in the import list
- no persistence: no autostart, no service, no scheduled task, no self-copy
- no packing, no obfuscation, no encrypted strings, no UPX
- no anti-debug and no anti-VM
- no dynamically resolved API names — every import is declared and visible
- no cross-process writes from the DLL; the only one in the project is the injector writing its
  own path into the target so that `LoadLibraryA` has something to read
- no `SetWindowsHookEx`, no driver, no elevation — the loader is manifested `asInvoker`

**Check the file you downloaded.** Every release publishes `SHA256SUMS.txt`, and both binaries are
built by a public GitHub Actions runner from a tagged commit, with a
[build provenance attestation](https://github.com/ProtoAus/demo-line-practice-tool/attestations):

```
gh attestation verify demo-line-practice-tool-v0.6.0.zip --repo ProtoAus/demo-line-practice-tool
```

VirusTotal for the current release: *(added at tag time)*

**If Defender quarantines it, please report the false positive.** It is the only thing that
actually fixes this, and it fixes it for everyone:
[microsoft.com/en-us/wdsi/filesubmission](https://www.microsoft.com/en-us/wdsi/filesubmission) →
*Software developer* → *Incorrectly detected as malware*. Attach the file and paste this page's URL
and the SHA256. I submit every release myself, but a report from an affected machine carries more
weight than mine.

There is **no code-signing certificate**, so SmartScreen will show "Windows protected your PC" the
first time you run the loader. That message is about reputation, not about the file — an unsigned
binary from a small project never accumulates any. **More info → Run anyway**, once you have
checked the hash.

## Your data

Nothing leaves your machine unless you press a button that says it will. No telemetry, no analytics,
no phone-home of any kind.

Until v0.5.1 that came with a stronger claim: the DLL linked no HTTP client at all, and
`dumpbin /dependents` proved it in one line. **From v0.6.0 that is no longer true.** Leaderboards
are now fetched by the DLL rather than by the script, so the import list is six names and
`WINHTTP.dll` is one of them. There is no way to have both, and pretending otherwise would be
worse than losing it.

What replaced it is smaller but still checkable in a couple of minutes:

- **One file.** `WinHttp` — the prefix every function in that API carries — appears in
  [src/](src/) in exactly two files, [src/wr_http.cpp](src/wr_http.cpp) and its header, and
  `grep` will confirm that faster than reading either. It is about 120 lines and it does GET.
  There is no arm that does anything else: no POST, no cookies, no credentials, no redirect to a
  scheme it did not start with.
- **One host.** Every URL is built in [src/wr_api.cpp](src/wr_api.cpp) from one constant,
  `https://api.momentum-mod.org/v1`, plus the absolute download link Momentum's own reply
  contained. Nothing else in the project constructs a URL.
- **No identifier of any kind.** No machine id, no account, no installation guid, no counter. The
  User-Agent says `WrLines/<version>` and links here, and that is everything it sends that it did
  not have to. The one request that names SteamID64s names your *friends'*, because you pressed a
  button that says it will look them up — and it is the same list the Steam client already has.
- **Never on its own.** Nothing here runs on a timer, at startup, or on a map change. Every request
  is downstream of a button press.

Everything else that reaches the network is still `wrpath_extract.py`, which you can read.

Everything it creates lives in a `wrlines_data` folder next to the DLL. Nothing is written into your
game install unless you press **send**, **local** or **watch** on a run — those copy one demo into
Momentum's replay folder so the game can play it, and **take out** removes it again.

---

## Build it yourself

```
git clone --depth 1 --branch v1.91.9b https://github.com/ocornut/imgui imgui
git clone --depth 1 --branch v1.3.3   https://github.com/TsudaKageyu/MinHook minhook
build.bat
```

Needs MSVC Build Tools and the Windows SDK. Close the game first — the DLL never unloads.
`tests\build.bat` builds and runs the test harnesses.

Those are the only two clones. The other two dependencies — zlib inflate and an LZMA decoder —
are committed under [third_party/](third_party/) instead, because they are a handful of frozen
files that decide what a run path *contains* rather than fifty that draw a panel.
[third_party/VERSION.txt](third_party/VERSION.txt) records the upstream tag, the release
archive's SHA-256 and what each build option switches off, so you can diff them against upstream
yourself.

The C++ is in [src/](src/), the harnesses in [tests/](tests/), and everything that talks to the
network is in [wrpath_extract.py](wrpath_extract.py). Both `.bat` files run from the repo root and
put their output there.

## How it works

There is no entity access here and no game API: it finds the camera by scanning memory for the
world-to-screen matrix, reads run paths out of the game's own `.mtv` demos, and projects the lines
itself. If you want the long version — what is measured, what is approximate, and the things that
were tried and thrown away — it is all in **[docs/how-it-works.md](docs/how-it-works.md)**.

## Licence

MIT — see [LICENSE](LICENSE).

Dear ImGui is MIT (Omar Cornut and contributors). MinHook is BSD-2-Clause (Tsuda Kageyu).
Neither is redistributed here; both are cloned at build time.

Two are redistributed, under [third_party/](third_party/), each with its own licence file beside
it: **miniz** 3.1.2, MIT (Rich Geldreich and contributors), and the **LZMA SDK** 23.01, placed in
the public domain by Igor Pavlov.

Not affiliated with or endorsed by Momentum Mod or Strata Source.
