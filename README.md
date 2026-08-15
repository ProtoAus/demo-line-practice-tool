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
gh attestation verify demo-line-practice-tool-v0.9.4.zip --repo ProtoAus/demo-line-practice-tool
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

## Your data

Nothing leaves your machine unless you press a button that says it will. No telemetry, no analytics,
no phone-home of any kind.

The DLL fetches leaderboards itself, so `WINHTTP.dll` is one of its six imports. That comes to about
120 lines in [src/wr_http.cpp](src/wr_http.cpp) which do GET and nothing else, one host —
`api.momentum-mod.org`, plus the download link its own reply contained — no identifier of any kind
sent, and no request that is not downstream of something you pressed. Demos are only ever downloaded
because you ticked a run.

Everything it creates lives in a `wrlines_data` folder next to the DLL, and nothing is written into
your game install unless you press **send**, **local** or **watch** on a run — those copy one demo
into Momentum's replay folder, and **take out** removes it again.

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

There is no entity access here and no game API: it finds the camera by scanning memory for the
world-to-screen matrix, reads run paths out of the game's own `.mtv` demos, and projects the lines
itself. The long version is in **[docs/how-it-works.md](docs/how-it-works.md)**.

## Licence

MIT — see [LICENSE](LICENSE).

Dear ImGui (MIT, Omar Cornut and contributors) and MinHook (BSD-2-Clause, Tsuda Kageyu) are cloned
at build time, not redistributed here. Redistributed under [third_party/](third_party/), each with
its own licence file beside it: **miniz** 3.1.2 (MIT, Rich Geldreich and contributors), the
**LZMA SDK** 23.01 (public domain, Igor Pavlov) and **zstd** 1.5.7 (BSD-3-Clause, Meta Platforms).

Not affiliated with or endorsed by Momentum Mod or Strata Source.
