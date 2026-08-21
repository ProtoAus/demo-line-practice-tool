# Demo Line Practice Tool

Draws other players' runs as **lines in the world**, in the Momentum Mod map you are playing.
See the route, see where they carried speed and where you don't.

![Six views: aiming at a line on surf_hades2 bonus 4, watching a replay on surf_colin_blaster_69000, the Graphs tab on surf_hades2 bonus 4, the Runs list on surf_utopia, the Board tab downloading from surf_tropic's leaderboard, and the Maps tab on surf_zoomathon](screenshots/tour.jpg)

[![Watch the tool in use — a video, on YouTube](https://img.youtube.com/vi/Udn2ilMsZs8/maxresdefault.jpg)](https://youtu.be/Udn2ilMsZs8)

---

## Get it running

1. **[Download the latest release](https://github.com/ProtoAus/demo-line-practice-tool/releases/latest)** — the `.zip`.
2. **Unzip it** anywhere you like.
3. **Start Momentum Mod and load into a map.**
4. **Double-click `wrinject.exe`.**
5. **Press `DELETE`** in game.
6. Press **Allow it to look**, pick a **stage** if the map has any, and **tick a run**.

> **If a ticked run says *gave up***, hover it — the reason is the extractor's own. When it ran out
> of time the cell becomes a **more time** button instead, which reads that one demo with no limit.
> Long marathon runs genuinely take a while.

> **If the lines are in the wrong place** — drifting further out towards the edges of the screen, or
> stuck to the screen while you move — press **Lines in the wrong place?** at the bottom of the
> page and walk around for a couple of seconds. That is the tool looking for the game's camera
> again, and it is most often needed when you injected at the main menu rather than in a map.

---

## On Linux

It runs under Proton, and it is one launch option. Nothing to compile, nothing to install into
the prefix.

1. **Force a Proton version** in the game's Properties → Compatibility. Momentum has a native
   Linux build, and a Windows DLL cannot be injected into a native Linux process — so this one
   is not optional.
2. **Unzip it inside the game's own folder**, e.g.
   `steamapps/common/Momentum Mod Playtest/wrlines/`.
3. **Set one launch option**, with the full path:

   ```
   PROTON_REMOTE_DEBUG_CMD="/full/path/to/wrlines/wrinject.exe --wait" %command%
   ```

Then start the game from Steam as usual and press `DELETE` in game. Do **not** run
`wrinject.exe` yourself — Proton starts it for you, before the game, and `--wait` is what makes
it sit there until the game is up. Running it through `protontricks` or `proton run` cannot
work, and [docs/how-it-works.md](docs/how-it-works.md#linux) explains why, along with a fallback
route if the launch option ever stops working.

If no panel appears, read `wrlines_data/wrinject.log` next to the DLL — the injector writes it
because on Linux its console output goes into the Proton log, where nobody is looking. If that
file is not there at all, the launch option is not running.

---

## Keys

| key | |
| --- | --- |
| `DELETE` | the quick page — the map's top runs, tick one to see it |
| `INSERT` | show / hide the full panel |
| `ESC` | close both |
| `Page Down` / `Page Up` | the box at your crosshair — next / previous mode |
| `Home` | *whose line am I looking at* — off by default |
| `End` | the corner block — off by default |

All but `INSERT` are rebindable, and the **About** tab lists them as they are currently bound.

---

## Fair play

This is an injected DLL. It **reads** memory rather than writing it, sets no cvars, runs no console
commands, and never touches `sv_cheats` — but injected code may still invalidate a submitted run
under Momentum's own rules. **If you care about a run counting, don't have this loaded while you set
it.**

## Why your antivirus may flag this

`wrinject.exe` loads the DLL into the game with `OpenProcess` / `VirtualAllocEx` /
`WriteProcessMemory` / `CreateRemoteThread`. That is also how a lot of malware loads itself, and
neither file is code-signed, so Defender cannot tell the two apart from the bytes alone.

What it does **not** do: no registry access at all — `ADVAPI32` is not even in the import list — no
persistence, no packing or obfuscation, no anti-debug, no hidden API names, no cross-process writes
from the DLL, no driver and no elevation.

**On VirusTotal at v0.9.3.** Every hit is a machine-learning or heuristic verdict; not one is a
match against known malware:

| file | | engine | verdict |
|---|---|---|---|
| [`wrinject.exe`](https://www.virustotal.com/gui/file/42c3734aa9b12bbe7a64972409e1e34a3d62501db06c4d0ae63b31d83ffc8d61) | **2/70** | Microsoft | `Program:Win32/Wacapew.C!ml` |
| | | Trapmine | `Suspicious.low.ml.score` |
| [`wrlines.dll`](https://www.virustotal.com/gui/file/5d3b4cdc2eeb9b7cfe517d3423b25a6ee0c4cfe1d8809121e353ffffed2d2186) | **5/70** | Microsoft | `Trojan:Win32/Wacatac.B!ml` |
| | | ESET-NOD32 | `Win64/GameHack_AGen.CCQ` — *potentially unsafe* |
| | | Symantec | `ML.Attribute.HighConfidence` |
| | | Cynet | `Malicious (score: 100)` |
| | | Bkav Pro | `W32.Malware.CA0BE60C` |

The `!ml` suffix on both Microsoft verdicts means a classifier decided, not a rule. ESET's is the
only specific one, and it files this as a *potentially unsafe application* rather than as malware —
a fair description of an overlay injected into a game.

**Check the file you downloaded.** Every release publishes `SHA256SUMS.txt`, and both binaries are
built by a public GitHub Actions runner from a tagged commit, with a
[build provenance attestation](https://github.com/ProtoAus/demo-line-practice-tool/attestations):

```
gh attestation verify demo-line-practice-tool-v1.1.0.zip --repo ProtoAus/demo-line-practice-tool
```

**If Defender quarantines it, please report the false positive.** It is the only thing that actually
fixes this, and it fixes it for everyone:
[microsoft.com/en-us/wdsi/filesubmission](https://www.microsoft.com/en-us/wdsi/filesubmission) →
*Software developer* → *Incorrectly detected as malware*. I submit every release myself, but a
report from an affected machine carries more weight than mine. ESET has
[its own form](https://www.eset.com/int/support/report-a-false-positive/).

There is **no code-signing certificate**, so SmartScreen will show "Windows protected your PC" the
first time you run the loader. That message is about reputation, not about the file. **More info →
Run anyway**, once you have checked the hash.

One thing worth knowing about the in-game updater: files it downloads do not carry a Mark of the
Web, so a `wrinject.exe` it installed will *not* raise that SmartScreen prompt the way the same file
from your browser would. That is a warning you stop getting, which is why the digests are shown on
screen rather than only compared.

## Your data

Nothing leaves your machine unless you press a button that says it will. No telemetry, no analytics,
no phone-home of any kind.

The DLL fetches leaderboards itself, so `WINHTTP.dll` is one of its six imports. That comes to about
120 lines in [src/wr_http.cpp](src/wr_http.cpp) which do GET and nothing else, two hosts —
`api.momentum-mod.org` and `api.github.com`, plus the download links their own replies contained —
no identifier of any kind sent, and no request that is not downstream of something you pressed.
Demos are only ever downloaded because you ticked a run.

`api.github.com` is only ever asked one question, and only when you press **Check for updates** in
the About tab: what the newest release is. There is no automatic check, no timer and no setting to
turn one on. See [Updating](#updating).

Everything it creates lives in a `wrlines_data` folder next to the DLL, and nothing is written into
your game install unless you press **send**, **local** or **watch** on a run — those copy one demo
into Momentum's replay folder, and **take out** removes it again.

It also **reads** one file out of the install that is not a demo: when the map changes, the map's own
`.bsp`, read-only and shared, on a background thread, so it can show you the angle of a ramp before
you reach it. That is ordinary file I/O and it makes no call into the game — but it is reading
something, so it is a checkbox in **Display → The map's own geometry**, and turning it off frees what
it read immediately.

## Updating

The **About** tab has a **Check for updates** button. It is three presses and each one stops:

1. **Check** asks GitHub what the newest release is and compares it against what you are running.
   Nothing is written and nothing is sent.
2. **Download it** fetches `wrlines.dll` and `wrinject.exe`, checks both against the release's own
   `SHA256SUMS.txt` **before anything reaches your disk**, and puts them in `wrlines_data\update`.
   It shows you the two digests, which are the same ones on the release page.
3. **Install it** renames the pair beside the DLL to `.old` and puts the new pair at their paths.
   Restart the game to run it — the DLL never unloads. If anything fails, the `.old` files go
   straight back and you are exactly where you were.

On Linux the third press is the whole of it: the launch option points at `wrinject.exe` by path,
so the replacement is what runs at the next start and there is nothing else to do.

Nothing happens on a timer, at startup or on a map change, and there is no setting to make it
automatic. That is deliberate: a program that reaches out on its own and replaces its own
executables is the shape antivirus software looks for, and this one has enough of that problem
already.

The digest check means every byte arrived. It is **not** a signature — the list it is checked
against comes from the same place the files do, so what you are trusting is the HTTPS connection to
github.com. Downloading the zip from the release page and checking the attestation is the stronger
option, and it is two commands up in the antivirus section.

---

## Build it yourself

```
git clone --depth 1 --branch v1.91.9b https://github.com/ocornut/imgui imgui
git clone --depth 1 --branch v1.3.3   https://github.com/TsudaKageyu/MinHook minhook
build.bat
```

Needs MSVC Build Tools and the Windows SDK. Close the game first — the DLL never unloads.
`tests\build.bat` builds and runs the test harnesses. Both `.bat` files run from the repo root.

Those are the only two clones. The decompressors — zlib inflate, LZMA and zstd — are committed under
[third_party/](third_party/) instead, and [VERSION.txt](third_party/VERSION.txt) records the
upstream tag, the release archive's SHA-256 and what each build option switches off, so you can diff
them against upstream yourself.

The C++ is in [src/](src/) and the harnesses in [tests/](tests/). `wrpath_extract.py`, the Python
program this was ported from, is not shipped but survives under
[tests/reference/](tests/reference/) as the oracle the port is checked against;
[tests/parity.ps1](tests/parity.ps1) runs the two over a real library and compares the files byte
for byte.

## How it works

No game API is called and no game code is ever executed. It finds the camera by scanning memory for
the world-to-screen matrix, reads run paths out of the game's own `.mtv` demos, and projects the
lines itself.

It also looks for the player's own origin and velocity the same way — `ReadProcessMemory` on its own
process, nothing written, nothing called — because the alternative was inferring both from camera
motion, and that carries errors nothing downstream can remove: the eye height is a setting rather
than a read, so a crouched player's feet were taken 36 units too low, and the crouch itself lerps
into the velocity as though you had moved. Nothing is believed until it has predicted the camera for
ninety frames running, and it is allowed to find nothing — everything keeps working on the camera
estimate, and the Diagnostics tab says which is in use. The long version is in
**[docs/how-it-works.md](docs/how-it-works.md)**.

## Licence

MIT — see [LICENSE](LICENSE).

Dear ImGui (MIT, Omar Cornut and contributors) and MinHook (BSD-2-Clause, Tsuda Kageyu) are cloned
at build time, not redistributed here. Redistributed under [third_party/](third_party/), each with
its own licence file beside it: **miniz** 3.1.2 (MIT, Rich Geldreich and contributors), the
**LZMA SDK** 23.01 (public domain, Igor Pavlov) and **zstd** 1.5.7 (BSD-3-Clause, Meta Platforms).

Not affiliated with or endorsed by Momentum Mod or Strata Source.
