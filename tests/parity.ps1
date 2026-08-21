# parity.ps1  --  run the port and the reference over the same demos and diff.
#
# This is the evidence the C++ port is correct. The harnesses in tests\ check
# properties that can be stated in a sentence; this checks the only property
# that actually matters, which is that six thousand real demos come out the same
# on both sides.
#
# NOT A HARNESS, AND DELIBERATELY NOT RUN BY tests\build.bat
#
# Three reasons, and they are worth stating because "why is this not in the
# suite" is the obvious question:
#
#   - It needs a Python interpreter. tests\build.bat is the script CI runs
#     verbatim on a fresh clone, and the entire point of the port is that
#     nothing needed to run this project requires an interpreter.
#   - It needs a game install and a demo library. Neither exists on a runner.
#   - It takes half an hour. That is a release gate, not a build step.
#
# WHAT IT COMPARES, AND WHY THAT IS THE RIGHT THING
#
# One verb at a time, growing with the port.
#
#   -Verb body    --dump-body over every demo in the library. The decompressed
#                 run body is the whole container-and-LZMA question with a
#                 perfect oracle and not one float involved. Bytes are either
#                 the same bytes or they are not, so a failure here is never
#                 something to argue about -- which is exactly why it is the
#                 checkpoint everything downstream depends on. Do not proceed
#                 past a partial pass.
#
#   -Verb board   a sequence of leaderboard fetches, byte-compared. See below.
#
#   -Verb fetch   demos actually downloaded, byte-compared, off the same kind of
#                 recording -- the tape covers the demo bodies too, because both
#                 implementations fetch them through the same getter the
#                 leaderboard goes through. Plus the one contract whose failure
#                 has no symptom: that the --into-game copy carries the source's
#                 write time. See -Verb fetch below.
#
# Both sides are handed the same argv and, for -Verb body, write to the same
# output path, so the line each prints is comparable character for character
# rather than after some normalisation that could hide the difference it was
# meant to reveal.
#
# A LEADERBOARD IS NOT AN ORACLE, SO IT IS RECORDED FIRST
#
# Ranks move as runs land, so the same board fetched twice a minute apart
# produces two different files that are both correct. Running the two
# implementations against the live API would therefore compare nothing.
#
# So -Verb board goes in three passes over the same sequence of fetches:
#
#   1. the reference, against the real API, with --api-record. This is the only
#      pass that touches the network, and it produces both the recording and
#      the answer everything else is compared against.
#   2. the reference again, with --api-replay of what it just recorded. If this
#      does not reproduce pass 1 exactly, the RECORDING is wrong and nothing
#      after it means anything -- so it is checked before the port is.
#   3. the port, with --api-replay of the same recording.
#
# The sequence ACCUMULATES on purpose: a window, a second window further down,
# the slow end, a spread, and finally a --refresh. Each step reads the file the
# last one wrote, so it exercises the merge, the dedupe by replay hash, and the
# rank sort's tie-breaking over insertion order -- none of which a single fetch
# into an empty file would touch at all.
#
# The recording is written under -Work, not into the repository. It is a real
# board, which means a hundred real players' names and SteamID64s, and that is
# the same reason wrlines_data is gitignored at any depth. The recording checked
# in under tests\fixtures\api is synthetic and is a different thing.
#
# THE REFERENCE IS ALLOWED TO FAIL
#
# wrpath_extract.py's dump path does not catch a header error, so a demo it
# cannot parse ends in a traceback rather than a message. That is not a
# mismatch: it is the absence of an oracle for that demo. Those are counted
# separately, and the only thing asserted about them is that the port refused
# the demo too -- an implementation that sailed past a header the reference
# choked on would be the more worrying result.
#
# Usage:
#     tests\parity.ps1 -Game "C:\...\Momentum Mod Playtest"
#     tests\parity.ps1 -Game ... -Limit 50               # a smoke run
#     tests\parity.ps1 -Game ... -Verb board -Map surf_demise
#     tests\parity.ps1 -Game ... -Verb board -Map ... -Rerecord
#     tests\parity.ps1 -Game ... -Verb fetch -Map surf_demise
#
# Exit code 0 if every demo with an oracle matched it.

param(
    [Parameter(Mandatory = $true)]
    [string]$Game,

    # Which comparison to run.
    [ValidateSet("body", "board", "fetch", "extract", "budget")]
    [string]$Verb = "body",

    # Where the two outputs are written and compared. Somewhere with room for
    # two copies of the largest body in the library; the default is under the
    # user's temp, not the working tree.
    [string]$Work = (Join-Path $env:TEMP "wrlines-parity"),

    # First N demos only, for a smoke run. 0 is the whole library.
    [int]$Limit = 0,

    # -Verb board: which leaderboard to fetch. The map has to exist and have
    # runs, or every step is a correct comparison of two empty boards.
    [string]$Map = "",
    [int]$MapId = 0,
    [int]$Gamemode = 1,
    [int]$TrackType = 0,
    [int]$TrackNum = 1,

    # -Verb fetch: where in the board to take the two demos it downloads from.
    # Not rank 1: the fastest run on a popular map is the one demo everybody
    # already has, and a step that finds nothing to fetch proves nothing about
    # fetching. Lower this on a quiet map.
    [int]$FetchRank = 200,

    # -Verb extract: how many workers each side uses. 1 is the comparable one
    # -- both implementations then process demos in iteration order and print a
    # line each, so stdout can be diffed line for line. 0 lets each side decide,
    # which is faster and compares stdout as a multiset instead.
    [int]$Jobs = 1,

    # -Verb board: fetch the API again even if a recording is already there.
    # Off by default, because a recording is reusable for ever and re-recording
    # is the only part of this that costs somebody else anything.
    [switch]$Rerecord,

    # -Verb board: also compare a friends lookup, using the friends.txt this
    # machine's DLL wrote. Opt-in because it puts your friends' SteamID64s in
    # the recording -- which stays under -Work and never goes near the repo,
    # but is still yours to decide about.
    [switch]$Friends,

    [string]$Python = "py"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "tests\wrextract.exe"

# The reference lives under tests\reference\ from v0.7.0. It sat at the repo
# root for as long as it was the thing that shipped; it is now the oracle this
# script drives, so it sits with the harness that drives it. Moving it does not
# change where it RUNS -- every verb copies it into the staging directory,
# because DEFAULT_OUT and its third demo tree are derived from the directory the
# script is in, and both sides have to agree on what is on disk.
$ref = Join-Path $root "tests\reference\wrpath_extract.py"

foreach ($needed in @($exe, $ref)) {
    if (-not (Test-Path $needed)) {
        Write-Host "[!] missing: $needed"
        if ($needed -eq $exe) { Write-Host "    run tests\build.bat first" }
        exit 2
    }
}
if (-not (Test-Path $Game)) { Write-Host "[!] no game directory: $Game"; exit 2 }

New-Item -ItemType Directory -Force -Path $Work | Out-Null
$outPath = Join-Path $Work "out.bin"
$refBin  = Join-Path $Work "ref.bin"
$natBin  = Join-Path $Work "nat.bin"
$errPath = Join-Path $Work "stderr.txt"
$report  = Join-Path $Work "report.txt"

# `-3` picks a version out of the py LAUNCHER and is not an interpreter flag, so
# passing -Python a real python.exe used to die with "Unknown option: -3" at the
# first call. The parameter has been documented and unusable since it was added.
# [string[]] AND NOT `$x = if (...) { @("-3") }`. PowerShell unrolls a
# single-element array on assignment, so that form leaves a STRING, and splatting
# a string is not the same operation as splatting a one-element array -- the
# interpreter ended up launching with no -c at all and sat in the REPL, which
# presents as the script hanging rather than as an error. The type constraint is
# what stops the unroll.
[string[]]$PyPre = @()
if ($Python -match '(^|[\\/])py(\.exe)?$') { $PyPre = @("-3") }

# The interpreter is part of the result. math.dist is version-sensitive and the
# port is bit-compatible with exactly one CPython, so a report that does not say
# which one was on the other side of the comparison is worth less than it looks.
$pyVersion = (& $Python @PyPre -c "import sys; print(sys.version.replace(chr(10),' '))")

# WHICH CPython, AND DOES IT HAVE THE CODEC. BOTH BEFORE THE EIGHTEEN MINUTES.
#
# This block exists because the gate spent 1,082 seconds and then reported 317
# MISMATCH, none of which were byte differences: the oracle had no `zstandard`,
# so it produced NO BYTES for every zstd-bodied demo while the port -- which
# links zstd -- read them all. Nothing was compared, and a difference in stdout
# wording was filed as a difference in output.
#
# A missing codec is a HARD STOP. A gate that cannot read 5% of the corpus must
# say so in the first second rather than in the eighteenth minute, and it must
# never say it in the vocabulary of a failure.
$pyHasZstd = $true
try {
    & $Python @PyPre -c "import zstandard" 2>$null
    if ($LASTEXITCODE -ne 0) { $pyHasZstd = $false }
} catch { $pyHasZstd = $false }

if (-not $pyHasZstd) {
    Write-Host "[!] this interpreter cannot decompress zstd demo bodies:"
    Write-Host "      $Python  ($pyVersion)"
    Write-Host ""
    Write-Host "    The reference would produce no output for every zstd demo"
    Write-Host "    while the port reads them, and each one would be counted as"
    Write-Host "    a MISMATCH it never actually compared. Install it:"
    Write-Host ""
    Write-Host "      $Python $($PyPre -join ' ') -m pip install zstandard"
    exit 2
}

# The version, on the other hand, is a WARNING and not a stop.
#
# The pin is wr_dp.h:120 -- the port transcribes CPython's vector_norm and its
# Neumaier-compensated sum(), and the second of those only exists since 3.12, so
# this code "would have been correct against 3.11". Running the oracle on
# anything else is a legitimate thing to want to do deliberately and a disaster
# to do by accident, which is what a loud line and a note in the report are for.
$PY_PIN = "3.13.9"      # keep in step with wr_dp.h:120
$pyOffPin = ($pyVersion -notmatch [regex]::Escape($PY_PIN))
if ($pyOffPin) {
    Write-Host ""
    Write-Host "[!] WARNING: this is not the pinned interpreter."
    Write-Host "      running $pyVersion"
    Write-Host "      pinned  CPython $PY_PIN  (wr_dp.h:120)"
    Write-Host "    Byte parity is version-sensitive -- sum() gained compensated"
    Write-Host "    summation in 3.12 and the port implements it. Mismatches from"
    Write-Host "    here may be the interpreter rather than the port."
    Write-Host ""
}

# ---------------------------------------------------------------------------
# -Verb board
# ---------------------------------------------------------------------------

if ($Verb -eq "board") {
    if (-not $Map -and -not $MapId) {
        Write-Host "[!] -Verb board needs -Map NAME (or -MapId N)"
        exit 2
    }

    $tape    = Join-Path $Work "tape"
    $refRoot = Join-Path $Work "ref"
    # board_path() is dirname(--out)/boards, so --out has to be one level in.
    $refOut  = Join-Path $refRoot "paths"
    $refDir  = Join-Path $refRoot "boards"
    # The port has no --out: WrDataPath for an exe under tests\ lands here.
    $natDir  = Join-Path $root "tests\wrlines_data\boards"

    $name = if ($Map) { $Map } else { "map$MapId" }
    $leaf = "{0}_g{1}_t{2}{3}.tsv" -f $name, $Gamemode, $TrackType, $TrackNum

    # Both stamps pinned, or the "fetched" line differs by however long the
    # three passes took. See WrNowEpoch and the reference's _now().
    $env:WRLINES_FAKE_NOW = "1700000000"

    $common = @("--game", $Game, "--gamemode", "$Gamemode",
                "--track-type", "$TrackType", "--track-num", "$TrackNum")
    if ($Map)   { $common += @("--map", $Map) }
    if ($MapId) { $common += @("--map-id", "$MapId") }

    # Accumulating on purpose: each step reads what the last one wrote. A single
    # fetch into an empty file would never touch the merge, the dedupe or the
    # rank sort's tie-breaking, which is where the interesting mistakes are.
    $steps = @(
        @{ name = "a window of fifty";       args = @("--count", "50") },
        @{ name = "a window further down";   args = @("--from-rank", "101", "--count", "150") },
        @{ name = "the slow end";            args = @("--slowest", "--count", "25") },
        @{ name = "eight samples across";    args = @("--spread", "8") },
        @{ name = "and start again";         args = @("--refresh", "--count", "10") }
    )
    if ($Friends) {
        $steps += @{ name = "your friends' runs"; args = @("--friends") }
    }

    Write-Host ""
    Write-Host "=== wrlines parity: --board ==="
    Write-Host "  reference   $ref"
    Write-Host "  python      $pyVersion"
    Write-Host "  port        $exe"
    Write-Host "  board       $name  g$Gamemode t$TrackType$TrackNum"
    Write-Host "  recording   $tape"
    Write-Host ""

    function Clear-Boards($dir) {
        if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }

    # A track with no runs on it is not an error and not a bug: cmd_board says
    # so and returns 0 having written nothing. "(none)" is therefore a real
    # answer and has to compare equal to the other side's "(none)", rather than
    # stopping the run.
    function Board-Hash($dir) {
        $p = Join-Path $dir $leaf
        if (-not (Test-Path $p)) { return "(none)" }
        return (Get-FileHash -Algorithm SHA256 $p).Hash
    }

    # A friends lookup reads dirname(--out)\friends.txt on the reference side
    # and wrlines_data\friends.txt on ours, so both need a copy of the same one.
    if ($Friends) {
        $mine = Join-Path $root "wrlines_data\friends.txt"
        if (-not (Test-Path $mine)) {
            Write-Host "[!] -Friends needs $mine -- press `"Refresh my friends`" in the panel first"
            exit 2
        }
        New-Item -ItemType Directory -Force -Path $refRoot | Out-Null
        Copy-Item -Force $mine (Join-Path $refRoot "friends.txt")
        $natData = Join-Path $root "tests\wrlines_data"
        New-Item -ItemType Directory -Force -Path $natData | Out-Null
        Copy-Item -Force $mine (Join-Path $natData "friends.txt")
    }

    # ONE RECORDING PER STEP, and this is not tidiness.
    #
    # api_tape_open truncates index.txt and restarts the response numbering at
    # 0001 every time the process starts, so five invocations recording into one
    # directory leave an index describing only the last of them and .bin files
    # from all five overwriting each other. Each step gets its own directory,
    # which is also what makes a single step re-recordable on its own.
    function Step-Tape($i) { Join-Path $tape ("s{0:d2}" -f $i) }

    # --- pass 1: the reference, against the real API, recording as it goes ----
    $recorded = @()
    if ($Rerecord -or -not (Test-Path (Step-Tape 0))) {
        if (Test-Path $tape) { Remove-Item -Recurse -Force $tape }
        Clear-Boards $refDir
        Write-Host "  recording from the live API -- this is the only pass that"
        Write-Host "  touches the network, and it paces itself"
        for ($i = 0; $i -lt $steps.Count; $i++) {
            $out = & $Python @PyPre $ref @common "--board" "--out" $refOut `
                             "--api-record" (Step-Tape $i) @($steps[$i].args) 2>&1
            $recorded += ,@(($out | Out-String).TrimEnd() -split "`r?`n")
            Write-Host ("    {0,-24} {1}" -f $steps[$i].name, "recorded")
        }
        $liveTsv = Join-Path $refDir $leaf
        if (Test-Path $liveTsv) {
            Copy-Item -Force $liveTsv (Join-Path $tape "expected.tsv")
        } else {
            Write-Host "  [i] this track has no runs, so nothing was written. That is"
            Write-Host "      still a comparison, but a thin one -- pick a busier track."
        }
    } else {
        Write-Host "  reusing the recording already in $tape (-Rerecord to refetch)"
    }

    # --- pass 2: the reference again, off the recording ----------------------
    Clear-Boards $refDir
    $refLines = @()
    $refFiles = @()
    for ($i = 0; $i -lt $steps.Count; $i++) {
        $out = & $Python @PyPre $ref @common "--board" "--out" $refOut `
                         "--api-replay" (Step-Tape $i) @($steps[$i].args) 2>&1
        $refLines += ,@(($out | Out-String).TrimEnd() -split "`r?`n")
        $refFiles += (Board-Hash $refDir)
    }

    # If the recording does not reproduce what recording it produced, the
    # recording is wrong and every comparison after this one is meaningless.
    # Checked before the port is looked at, and only when pass 1 actually ran.
    $tapeOk = $true
    if ($recorded.Count -gt 0) {
        $expected = Join-Path $tape "expected.tsv"
        $a = if (Test-Path $expected) {
                 (Get-FileHash -Algorithm SHA256 $expected).Hash
             } else { "(none)" }
        if ($a -ne $refFiles[-1]) { $tapeOk = $false }
        for ($i = 0; $i -lt $steps.Count; $i++) {
            if (($recorded[$i] -join "`n") -ne ($refLines[$i] -join "`n")) { $tapeOk = $false }
        }
        Write-Host ("  the recording reproduces the live run: {0}" -f `
                    $(if ($tapeOk) { "yes" } else { "NO" }))
        if (-not $tapeOk) {
            Write-Host "=== the RECORDING is wrong; nothing after this means anything ==="
            exit 1
        }
    }

    # --- pass 3: the port, off the same recording ----------------------------
    Clear-Boards $natDir
    $same = 0; $mismatch = 0
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("wrlines parity --board")
    $lines.Add("python: $pyVersion")
    $lines.Add("board:  $name g$Gamemode t$TrackType$TrackNum")
    $lines.Add("")

    for ($i = 0; $i -lt $steps.Count; $i++) {
        $s = $steps[$i]
        $out = & $exe @common "--board" "--api-replay" (Step-Tape $i) @($s.args) 2>&1
        $got = ($out | Out-String).TrimEnd() -split "`r?`n"

        # Every line has to match except the last, which names the file each
        # implementation wrote and therefore names two different directories.
        # The BASENAME still has to match: it encodes the map, the gamemode and
        # the track, and getting it wrong would write a perfectly good cache
        # where nothing will ever look for it.
        $want = $refLines[$i]
        $ok = ($want.Count -eq $got.Count)
        if ($ok) {
            for ($k = 0; $k -lt $want.Count; $k++) {
                if ($want[$k].StartsWith("-> ") -and $got[$k].StartsWith("-> ")) {
                    $wl = Split-Path -Leaf ($want[$k].Substring(3))
                    $gl = Split-Path -Leaf ($got[$k].Substring(3))
                    if ($wl -ne $gl) {
                        $ok = $false
                        $lines.Add("NAME`t$($s.name)`tref: $wl`tport: $gl")
                    }
                } elseif ($want[$k] -ne $got[$k]) {
                    $ok = $false
                    $lines.Add("LINE`t$($s.name)`tref: $($want[$k])`tport: $($got[$k])")
                }
            }
        } else {
            $lines.Add("COUNT`t$($s.name)`tref: $($want.Count) lines`tport: $($got.Count)")
        }

        $b = (Board-Hash $natDir)
        $bytes = ($b -eq $refFiles[$i])
        if (-not $bytes) {
            $lines.Add("BYTES`t$($s.name)`tref: $($refFiles[$i])`tport: $b")
        }

        if ($ok -and $bytes) { $same++ } else { $mismatch++ }
        Write-Host ("    {0,-24} {1}" -f $s.name,
                    $(if ($ok -and $bytes) { "identical" }
                      elseif ($bytes) { "SAME FILE, DIFFERENT OUTPUT" }
                      else { "DIFFERENT FILE" }))
    }

    $lines.Add("")
    $lines.Add("identical: $same")
    $lines.Add("MISMATCH:  $mismatch")
    $boardReport = Join-Path $Work "report-board.txt"
    Set-Content -Path $boardReport -Value $lines -Encoding UTF8

    Write-Host ""
    Write-Host ("  identical   {0} of {1} steps" -f $same, $steps.Count)
    Write-Host ("  MISMATCH    {0}" -f $mismatch)
    Write-Host "  report      $boardReport"
    Write-Host ""
    if ($mismatch -gt 0) { Write-Host "=== PARITY FAILED ==="; exit 1 }
    Write-Host "=== parity holds ==="
    exit 0
}

# ---------------------------------------------------------------------------
# -Verb fetch
# ---------------------------------------------------------------------------
#
# WHY THIS ONE RUNS THE PORT FROM THE REPOSITORY ROOT
#
# Both implementations decide "do I already have this run" by walking two trees:
# the game's momtv, and their own demos directory. The game's is the same one for
# both. Their own is not, and the reason is a detail of the reference: demo_roots
# derives it from DEFAULT_OUT -- the script's own directory -- and NOT from
# --out. So no value of --out can move it.
#
# That number is printed twice ("N demos on disk across every map", "N of M are
# already here") and it decides what gets fetched, so two different sets are not
# a comparison at all. The only way to make them the same set is to put the port
# where the reference already is, which is what the copy to the repository root
# does. It is deleted afterwards.
#
# WHAT THIS RUN TOUCHES, AND WHAT IT PUTS BACK
#
# A fetch writes real files: demos into wrlines_data\demos\<map>\, and with
# --into-game a copy into the game's own replay folder. Both sides write the
# same filename, because the filename is the replay hash, so each step runs the
# reference, moves what it produced aside, runs the port, moves that aside, and
# compares. Anything that was there BEFORE the run is left exactly where it was;
# anything the run created is removed. The board cache the --ranks step needs is
# backed up and restored for the same reason.
#
# THE TIMESTAMP, WHICH IS THE POINT OF THE --into-game STEP
#
# wr_intogame.h decides whether a file in the game's folder is one of ours by
# matching size AND write time against our own copy. shutil.copy2 carries that
# across; CopyFileA does not, and the port has to do it by hand. Nothing looks
# wrong when it breaks -- the demo downloads, plays, and draws lines -- so it is
# asserted here rather than trusted: on each side separately, the copy in the
# game's folder must have the same write time, to the tick, as the copy in ours.

if ($Verb -eq "fetch") {
    if (-not $Map -and -not $MapId) {
        Write-Host "[!] -Verb fetch needs -Map NAME (or -MapId N)"
        exit 2
    }

    $tape    = Join-Path $Work "tape-fetch"
    $refKeep = Join-Path $Work "fetch-ref"
    $natKeep = Join-Path $Work "fetch-nat"

    # Both sides, deliberately, in the same place. See the essay above.
    $dataDir = Join-Path $root "wrlines_data"
    $refOut  = Join-Path $dataDir "paths"
    $portExe = Join-Path $root "wrextract.exe"

    $name = if ($Map) { $Map } else { "map$MapId" }
    $dest = Join-Path $dataDir "demos\$name"
    $leaf = "{0}_g{1}_t{2}{3}.tsv" -f $name, $Gamemode, $TrackType, $TrackNum
    $boardTsv = Join-Path $dataDir "boards\$leaf"
    $boardBak = Join-Path $Work "boards-backup.tsv"

    $env:WRLINES_FAKE_NOW = "1700000000"

    $common = @("--game", $Game, "--gamemode", "$Gamemode",
                "--track-type", "$TrackType", "--track-num", "$TrackNum")
    if ($Map)   { $common += @("--map", $Map) }
    if ($MapId) { $common += @("--map-id", "$MapId") }

    Write-Host ""
    Write-Host "=== wrlines parity: --fetch ==="
    Write-Host "  reference   $ref"
    Write-Host "  python      $pyVersion"
    Write-Host "  port        $portExe  (a copy of tests\wrextract.exe)"
    Write-Host "  map         $name  g$Gamemode t$TrackType$TrackNum"
    Write-Host "  demos into  $dest"
    Write-Host "  recording   $tape"
    Write-Host ""

    foreach ($d in @($refKeep, $natKeep)) {
        if (Test-Path $d) { Remove-Item -Recurse -Force $d }
        New-Item -ItemType Directory -Force -Path $d | Out-Null
    }
    Copy-Item -Force $exe $portExe
    if (Test-Path $boardTsv) { Copy-Item -Force $boardTsv $boardBak }

    # Everything already on disk in the two places a fetch writes to. Whatever
    # is in these sets at the end has to still be there, untouched.
    function Snapshot($dir) {
        if (-not (Test-Path $dir)) { return @{} }
        $h = @{}
        foreach ($f in Get-ChildItem -File $dir) { $h[$f.Name] = $true }
        return $h
    }

    $gameDir = Join-Path $Game "momentum\momtv\online"
    $mapIdResolved = 0
    $recordGameBefore = @{}

    function Step-Tape($i) { Join-Path $tape ("f{0:d2}" -f $i) }

    # A rank nobody is likely to already hold, so the download step actually
    # downloads. Chosen from the middle of the board rather than the top: rank 1
    # of a popular map is the one demo everybody has.
    $steps = @(
        @{ name  = "browse the top twelve"
           args  = @("--fetch", "--dry-run", "--count", "12")
           files = $false },
        @{ name  = "download two of them"
           args  = @("--fetch", "--from-rank", "$FetchRank", "--count", "2")
           files = $true },
        # The same rank the step above took, deliberately. Each step's downloads
        # are moved out of the way as soon as it finishes, so by the time this
        # one runs that run is missing again and it is certain to download --
        # where a fresh rank is a guess about what this machine already holds,
        # and a step that finds nothing to fetch never reaches the copy this is
        # here to check.
        @{ name  = "and into the game folder"
           args  = @("--fetch", "--from-rank", "$FetchRank", "--count", "1",
                     "--into-game")
           files = $true; game = $true },
        # The Board tab's tick-and-download: named places out of the cached
        # board, costing no leaderboard request at all. 99999 is in there on
        # purpose -- a rank outside the cached window is named rather than
        # silently dropped, and that sentence has to match too.
        @{ name  = "named places off the cache"
           args  = @("--fetch", "--ranks", "3,7,99999")
           files = $true }
    )

    # The cached board the last step reads. Written by the reference, once,
    # from its own recording, so both sides read one identical file -- and put
    # back afterwards, because it is the user's real cache.
    $setupTape = Join-Path $tape "board"
    $setupArgs = @("--board", "--count", "30")

    # --- pass 1: the reference, live, recording ------------------------------
    #
    # Recorded into one directory per step for the same reason -Verb board does
    # it: api_tape_open truncates index.txt and renumbers from 0001 every time
    # the process starts.
    $recorded = @()
    if ($Rerecord -or -not (Test-Path (Step-Tape 0))) {
        if (Test-Path $tape) { Remove-Item -Recurse -Force $tape }
        Write-Host "  recording from the live API -- the only pass that touches"
        Write-Host "  the network, and it pays for the demo bodies as well"
        & $Python @PyPre $ref @common "--out" $refOut "--api-record" $setupTape `
                  @setupArgs | Out-Null
        Write-Host ("    {0,-26} {1}" -f "a board to pick from", "recorded")
        for ($i = 0; $i -lt $steps.Count; $i++) {
            $before = Snapshot $dest
            $out = & $Python @PyPre $ref @common "--out" $refOut `
                             "--api-record" (Step-Tape $i) @($steps[$i].args) 2>&1
            $recorded += ,@(($out | Out-String).TrimEnd() -split "`r?`n")

            # Recording downloads them FOR REAL, so take them straight back out
            # -- both copies. Leaving them behind is not merely untidy: a demo
            # still on disk is a demo both replay passes then find in the held
            # set, and the download step they were meant to drive quietly
            # becomes "already here; 0 to fetch" and proves nothing.
            foreach ($f in (Get-ChildItem -File $dest -ErrorAction SilentlyContinue)) {
                if (-not $before.ContainsKey($f.Name)) { Remove-Item -Force $f.FullName }
            }
            if (-not $mapIdResolved) {
                foreach ($l in $recorded[$i]) {
                    if ($l -match '^map \S+ \(id (\d+)\)') {
                        $mapIdResolved = [int]$matches[1]
                        break
                    }
                }
                if ($mapIdResolved) {
                    $recordGameBefore = Snapshot (Join-Path $gameDir "$mapIdResolved")
                }
            }
            if ($steps[$i].game -and $mapIdResolved) {
                $gp = Join-Path $gameDir "$mapIdResolved"
                foreach ($f in (Get-ChildItem -File $gp -ErrorAction SilentlyContinue)) {
                    if (-not $recordGameBefore.ContainsKey($f.Name)) {
                        Remove-Item -Force $f.FullName
                    }
                }
            }
            Write-Host ("    {0,-26} {1}" -f $steps[$i].name, "recorded")
        }
    } else {
        Write-Host "  reusing the recording already in $tape (-Rerecord to refetch)"
    }

    # The board the --ranks step reads, rebuilt from the recording so it holds
    # the same thirty rows however long ago the recording was made.
    & $Python @PyPre $ref @common "--out" $refOut "--api-replay" $setupTape `
              @setupArgs | Out-Null

    # The map id, so the game's replay folder can be found. Taken from what the
    # reference PRINTS rather than resolved again here, so this script cannot
    # have a second opinion about which map it is comparing.
    if ($MapId) { $mapIdResolved = $MapId }

    # --- passes 2 and 3, step by step ----------------------------------------
    $same = 0; $mismatch = 0
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("wrlines parity --fetch")
    $lines.Add("python: $pyVersion")
    $lines.Add("map:    $name g$Gamemode t$TrackType$TrackNum")
    $lines.Add("")

    # Run one side of one step: returns the stdout lines, and moves whatever it
    # created out of the way into $keep so the other side starts from the same
    # disk. Returns the created names too, so the two sides can be compared by
    # name rather than by position.
    function Run-Side($cmd, $argv, $keep, $wantGame) {
        $beforeDest = Snapshot $dest
        $gpath = if ($wantGame -and $mapIdResolved) {
                     Join-Path $gameDir "$mapIdResolved" } else { $null }
        $beforeGame = if ($gpath) { Snapshot $gpath } else { @{} }

        $out = & $cmd @argv 2>&1
        $text = ($out | Out-String).TrimEnd() -split "`r?`n"

        $made = @()
        foreach ($f in (Get-ChildItem -File $dest -ErrorAction SilentlyContinue)) {
            if ($beforeDest.ContainsKey($f.Name)) { continue }
            $made += $f.Name
            Move-Item -Force $f.FullName (Join-Path $keep $f.Name)
        }
        $gmade = @()
        if ($gpath -and (Test-Path $gpath)) {
            foreach ($f in (Get-ChildItem -File $gpath)) {
                if ($beforeGame.ContainsKey($f.Name)) { continue }
                $gmade += $f.Name
                Move-Item -Force $f.FullName (Join-Path $keep "game-$($f.Name)")
            }
        }
        return @{ lines = $text; made = $made; gameMade = $gmade }
    }

    for ($i = 0; $i -lt $steps.Count; $i++) {
        $s = $steps[$i]
        $wantGame = [bool]$s.game

        $r = Run-Side $Python ($PyPre + @($ref) + $common + @("--out", $refOut,
                      "--api-replay", (Step-Tape $i)) + $s.args) $refKeep $wantGame
        if (-not $mapIdResolved) {
            foreach ($l in $r.lines) {
                if ($l -match '^map \S+ \(id (\d+)\)') {
                    $mapIdResolved = [int]$matches[1]
                    break
                }
            }
        }
        $p = Run-Side $portExe ($common + @("--api-replay", (Step-Tape $i)) +
                      $s.args) $natKeep $wantGame

        $ok = ($r.lines.Count -eq $p.lines.Count)
        if ($ok) {
            for ($k = 0; $k -lt $r.lines.Count; $k++) {
                if ($r.lines[$k] -ne $p.lines[$k]) {
                    $ok = $false
                    $lines.Add("LINE`t$($s.name)`tref: $($r.lines[$k])`tport: $($p.lines[$k])")
                }
            }
        } else {
            $lines.Add("COUNT`t$($s.name)`tref: $($r.lines.Count) lines`tport: $($p.lines.Count)")
        }

        # The demos themselves, by name, byte for byte.
        $bytes = $true
        if ($s.files) {
            $wantNames = @($r.made | Sort-Object)
            $gotNames  = @($p.made | Sort-Object)
            if (($wantNames -join ",") -ne ($gotNames -join ",")) {
                $bytes = $false
                $lines.Add("NAMES`t$($s.name)`tref: $($wantNames -join ',')`tport: $($gotNames -join ',')")
            } else {
                if ($wantNames.Count -eq 0) {
                    # Not a failure -- both sides agreed there was nothing to
                    # fetch, which is a real answer. Recorded because it means
                    # this step compared two messages and no bytes, and a run
                    # where EVERY step says this has not checked a download.
                    $lines.Add("NOTE`t$($s.name)`tboth sides already held every run asked for")
                }
                foreach ($n in $wantNames) {
                    $a = (Get-FileHash -Algorithm SHA256 (Join-Path $refKeep $n)).Hash
                    $b = (Get-FileHash -Algorithm SHA256 (Join-Path $natKeep $n)).Hash
                    if ($a -ne $b) {
                        $bytes = $false
                        $lines.Add("BYTES`t$($s.name)`t$n`tref: $a`tport: $b")
                    }
                }
            }
        }

        # And the contract with no symptom: within each side, the copy in the
        # game's folder carries the write time of the copy in ours. Compared to
        # the TICK -- a copy that quietly took a fresh stamp would still land in
        # the same second as the download that produced it.
        $stamped = $true
        if ($wantGame) {
            foreach ($side in @(@{k=$refKeep; who="ref"; made=$r.made},
                                @{k=$natKeep; who="port"; made=$p.made})) {
                if (@($side.made).Count -eq 0) {
                    $stamped = $false
                    $lines.Add("NOTHING`t$($s.name)`t$($side.who) downloaded nothing, so the copy was never made")
                    continue
                }
                foreach ($n in $side.made) {
                    $ours = Join-Path $side.k $n
                    $g    = Join-Path $side.k "game-$n"
                    if (-not (Test-Path $g)) {
                        $stamped = $false
                        $lines.Add("NOGAME`t$($s.name)`t$($side.who)`t$n never reached the game folder")
                        continue
                    }
                    $t1 = (Get-Item $ours).LastWriteTimeUtc.Ticks
                    $t2 = (Get-Item $g).LastWriteTimeUtc.Ticks
                    if ($t1 -ne $t2) {
                        $stamped = $false
                        $lines.Add("MTIME`t$($s.name)`t$($side.who)`t$n`tours: $t1`tgame: $t2")
                    }
                }
            }
        }

        if ($ok -and $bytes -and $stamped) { $same++ } else { $mismatch++ }
        $verdict = if ($ok -and $bytes -and $stamped) { "identical" }
                   elseif (-not $stamped) { "THE WRITE TIME DID NOT CARRY" }
                   elseif (-not $bytes) { "DIFFERENT BYTES" }
                   else { "DIFFERENT OUTPUT" }
        Write-Host ("    {0,-26} {1}" -f $s.name, $verdict)
    }

    # --- put the disk back ----------------------------------------------------
    Remove-Item -Force $portExe -ErrorAction SilentlyContinue
    if (Test-Path $boardBak) {
        Copy-Item -Force $boardBak $boardTsv
    } else {
        # There was no cache for this board before, so the one the setup step
        # wrote is ours to take away again.
        Remove-Item -Force $boardTsv -ErrorAction SilentlyContinue
    }
    if ((Test-Path $dest) -and -not (Get-ChildItem -Force $dest)) {
        Remove-Item -Force $dest
    }

    $lines.Add("")
    $lines.Add("identical: $same")
    $lines.Add("MISMATCH:  $mismatch")
    $lines.Add("")
    $lines.Add("what was downloaded is under $refKeep and $natKeep; nothing was")
    $lines.Add("left in wrlines_data or in the game install.")
    $fetchReport = Join-Path $Work "report-fetch.txt"
    Set-Content -Path $fetchReport -Value $lines -Encoding UTF8

    Write-Host ""
    Write-Host ("  identical   {0} of {1} steps" -f $same, $steps.Count)
    Write-Host ("  MISMATCH    {0}" -f $mismatch)
    Write-Host "  report      $fetchReport"
    Write-Host ""
    if ($mismatch -gt 0) { Write-Host "=== PARITY FAILED ==="; exit 1 }
    Write-Host "=== parity holds ==="
    exit 0
}

# ---------------------------------------------------------------------------
# -Verb extract   --   the release gate
# ---------------------------------------------------------------------------
#
# The whole extractor, both implementations, over real demos, compared as
# files. This is the one that decides whether the port ships, because the
# cutover is straight: wrpath_extract.py stops being installed in the same tag
# that turns native extraction on, and there is no switch to put it back.
#
# WHY BOTH SIDES RUN FROM A STAGING DIRECTORY
#
# The reference derives two of its three paths from the SCRIPT'S OWN directory
# and not from --out:
#
#     DEFAULT_OUT = <script dir>\wrlines_data\paths
#     iter_demos' third tree = <script dir>\wrlines_data\demos
#
# So no value of --out can move the demo tree, and running the reference in
# place would make it read the working copy of wrlines_data -- while the port,
# whose WrDataPath is relative to its own .exe, read a different one. The two
# would then be comparing different sets of demos and agreeing about it.
#
# The fix is to give both sides the same home: copy the .py and the .exe into a
# scratch directory and run them there. Then DEFAULT_OUT and WrDataPath are the
# same place, --out separates the two outputs, and neither side can see the
# working tree at all.
#
# The demo tree is reached through a DIRECTORY JUNCTION rather than a copy --
# there are two gigabytes of demos and copying them would be the slowest part of
# the run by an order of magnitude. Both sides walk it transparently: os.walk
# only refuses to descend into a link it FINDS, and this one is the root it is
# handed. The junction is removed before the staging directory is, so nothing
# can delete through it.
#
# WHAT IS COMPARED
#
#   every .wrpath      byte for byte, both directions, so a file only one side
#                      wrote is a failure and not an absence
#   every _failed.txt  byte for byte, including the reason strings -- a record
#                      with a different reason reads as a different failure and
#                      would be re-derived for ever
#   stdout             at --jobs 1, line for line after the two wall-clock
#                      fields are blanked. At --jobs 0 the order is whatever the
#                      pool produced, so it is compared as a sorted multiset
#                      instead, which still catches a line that differs in
#                      anything but position -- and with the "[i/n]" completion
#                      counter blanked as well, because above one worker that
#                      number is not a property of either implementation. See
#                      Normalise. The n is still compared.
#
# WRLINES_FAKE_NOW is set on both sides. The .wrpath header carries a timestamp
# at 0xF4 and the CRC covers it, so without pinning the clock two runs of the
# SAME implementation differ and the comparison needs a tool that knows to skip
# four bytes and recompute a checksum -- one more thing that can be wrong.
#
#     tests\parity.ps1 -Game "..." -Verb extract -Map surf_demise
#     tests\parity.ps1 -Game "..." -Verb extract -Jobs 0        # the whole library

if ($Verb -eq "extract") {
    Write-Host ""
    Write-Host "=== wrlines parity: extraction ==="
    Write-Host "  reference   $ref"
    Write-Host "  python      $pyVersion"
    Write-Host "  port        $exe"
    Write-Host "  game        $Game"
    if ($Map) { Write-Host "  map         $Map" } else { Write-Host "  map         (every map)" }
    Write-Host "  jobs        $Jobs"

    $stage    = Join-Path $Work "stage"
    $stageDat = Join-Path $stage "wrlines_data"
    $stageJn  = Join-Path $stageDat "demos"
    $refOut   = Join-Path $stage "ref"
    $natOut   = Join-Path $stageDat "paths"
    $ourDemos = Join-Path $root "wrlines_data\demos"

    # --- put the staging directory back, whatever happened last time ---------
    if (Test-Path $stageJn) { cmd /c rmdir "$stageJn" | Out-Null }
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force -Path $stageDat | Out-Null

    Copy-Item -Force $exe (Join-Path $stage "wrextract.exe")
    Copy-Item -Force $ref (Join-Path $stage "wrpath_extract.py")
    $stageExe = Join-Path $stage "wrextract.exe"
    $stagePy  = Join-Path $stage "wrpath_extract.py"

    # The fetched-demo tree, read-only, through a link. Skipped if this machine
    # has never fetched anything -- and said out loud, because a run that only
    # covered the game's own tree covered two thirds of the corpus.
    if (Test-Path $ourDemos) {
        cmd /c mklink /J "$stageJn" "$ourDemos" | Out-Null
        if (-not (Test-Path $stageJn)) {
            Write-Host "[!] could not link $ourDemos into the staging directory"
            exit 2
        }
        $fetched = @(Get-ChildItem -Path $ourDemos -Recurse -Filter *.mtv -File).Count
        Write-Host "  demos       $fetched fetched (linked, read only)"
    } else {
        Write-Host "  demos       none fetched; the game's own tree only"
    }

    $env:WRLINES_FAKE_NOW = "1700000000"

    $common = @("--game", $Game, "--skip-existing", "--timeout", "0",
                "--jobs", "$Jobs")
    if ($Map)   { $common += @("--map", $Map) } else { $common += "--all" }
    if ($Limit -gt 0) { $common += @("--limit", "$Limit") }

    # --- run the reference ----------------------------------------------------
    #
    # Captured with *> rather than raw bytes, which is safe here for a reason
    # worth writing down: cmd_extract's output is ASCII by construction. It
    # prints demo file names -- hex, or the game's own ASCII names for local
    # recordings -- and reason strings, and never a player alias. The board and
    # fetch verbs, which DO print aliases, compare bytes instead.
    Write-Host ""
    Write-Host "  reference running..."
    $swR = [System.Diagnostics.Stopwatch]::StartNew()
    $refLog = Join-Path $Work "extract-ref.txt"
    & $Python @PyPre $stagePy @common --out $refOut *> $refLog
    $refCode = $LASTEXITCODE
    $swR.Stop()
    Write-Host ("  reference   exit {0}, {1:n0}s" -f $refCode, $swR.Elapsed.TotalSeconds)

    # --- and the port ---------------------------------------------------------
    Write-Host "  port running..."
    $swP = [System.Diagnostics.Stopwatch]::StartNew()
    $natLog = Join-Path $Work "extract-port.txt"
    & $stageExe @common *> $natLog
    $natCode = $LASTEXITCODE
    $swP.Stop()
    Write-Host ("  port        exit {0}, {1:n0}s" -f $natCode, $swP.Elapsed.TotalSeconds)

    $env:WRLINES_FAKE_NOW = $null

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("wrlines parity: extraction")
    $lines.Add("python: $pyVersion")
    $lines.Add("game:   $Game")
    $lines.Add("map:    $(if ($Map) { $Map } else { '(all)' })")
    $lines.Add("jobs:   $Jobs")
    $lines.Add("")

    $problems = 0

    if ($refCode -ne $natCode) {
        $problems++
        $lines.Add("EXITCODE`tref $refCode`tport $natCode")
    }

    # --- the files ------------------------------------------------------------
    #
    # Both directions. A .wrpath only one side wrote is the most interesting
    # possible difference -- it means the two disagreed about whether a demo is
    # extractable at all -- so it is a failure and not an absence.
    function Relative([string]$root, [string]$full) {
        return $full.Substring($root.Length).TrimStart('\')
    }

    $refFiles = @{}
    $natFiles = @{}
    if (Test-Path $refOut) {
        foreach ($f in Get-ChildItem -Path $refOut -Recurse -File) {
            $refFiles[(Relative $refOut $f.FullName)] = $f.FullName
        }
    }
    if (Test-Path $natOut) {
        foreach ($f in Get-ChildItem -Path $natOut -Recurse -File) {
            $natFiles[(Relative $natOut $f.FullName)] = $f.FullName
        }
    }

    $same = 0; $differ = 0; $onlyRef = 0; $onlyNat = 0
    foreach ($rel in ($refFiles.Keys | Sort-Object)) {
        if (-not $natFiles.ContainsKey($rel)) {
            $onlyRef++
            $problems++
            $lines.Add("ONLY-REF`t$rel")
            continue
        }
        $a = (Get-FileHash -Algorithm SHA256 $refFiles[$rel]).Hash
        $b = (Get-FileHash -Algorithm SHA256 $natFiles[$rel]).Hash
        if ($a -eq $b) {
            $same++
        } else {
            $differ++
            $problems++
            $lines.Add("BYTES`t$rel`tref $a`tport $b")
        }
    }
    foreach ($rel in ($natFiles.Keys | Sort-Object)) {
        if (-not $refFiles.ContainsKey($rel)) {
            $onlyNat++
            $problems++
            $lines.Add("ONLY-PORT`t$rel")
        }
    }

    # --- stdout ---------------------------------------------------------------
    #
    # Two wall-clock fields are blanked and nothing else is: the per-demo
    # "%.1fs" at the end of an OK line, and the "%d processed in %.1fs" of the
    # summary. Everything else in those lines -- point counts, coverage, the
    # match error to four decimal places -- is compared as printed.
    #
    # The third rule is not a clock. The epilogue names the directory it wrote
    # to, and the two sides write to DIFFERENT directories on purpose -- that is
    # what --out is for, and it is the only reason the run can compare anything
    # at all. Comparing that path would be comparing this script's own argument.
    # The COUNT in front of it is left alone, and it is the part that means
    # something: it is the number of files each side believes it wrote.
    #
    # AND A FOURTH RULE THAT APPLIES ONLY WHEN THERE IS A POOL.
    #
    # The "[i/n]" a progress line opens with is a COMPLETION counter, not the
    # target's position in the list -- both implementations increment it as
    # results come back. At --jobs 1 that is iteration order, it is
    # deterministic, and it is part of what this compares. Above one worker it
    # is whatever the pool did, so the same demo is [9/6416] in one run and
    # [10/6416] in the next OF THE SAME BINARY.
    #
    # Sorting raw lines therefore made the pooled pass compare two shuffled
    # numberings and report that every line differed, which says nothing about
    # either implementation. Measured here: two six-worker runs of one binary
    # over 6,416 demos produced 6,422 lines whose multisets differ completely
    # before this rule and are identical after it.
    #
    # The DENOMINATOR stays compared. It is the number of targets selected, it
    # is decided before any work starts, and two sides disagreeing about it is
    # exactly the kind of divergence this verb exists to catch.
    function Normalise([string[]]$text, [bool]$pooled) {
        $out = New-Object System.Collections.Generic.List[string]
        foreach ($l in $text) {
            $x = $l -replace '(\s)\d+\.\d(s?)$', '$1<t>$2'
            $x = $x -replace '^(\d+ processed in )[\d.]+s', '$1<t>s'
            $x = $x -replace '^(wrote \d+ \.wrpath files under ).*$', '$1<out>'
            if ($pooled) { $x = $x -replace '^\[\d+/(\d+)\]', '[<i>/$1]' }
            $out.Add($x)
        }
        return $out
    }

    $refText = Normalise (Get-Content -Path $refLog) ($Jobs -ne 1)
    $natText = Normalise (Get-Content -Path $natLog) ($Jobs -ne 1)

    $stdoutOk = $true
    if ($Jobs -eq 1) {
        # Serial: the reference processes targets in iteration order and prints
        # a line each, so the ORDER is part of the artefact.
        if ($refText.Count -ne $natText.Count) {
            $stdoutOk = $false
            $lines.Add("STDOUT`tline counts differ: ref $($refText.Count), port $($natText.Count)")
        }
        $n = [Math]::Min($refText.Count, $natText.Count)
        $shown = 0
        for ($i = 0; $i -lt $n; $i++) {
            if ($refText[$i] -ne $natText[$i]) {
                $stdoutOk = $false
                if ($shown -lt 20) {
                    $lines.Add("LINE $($i+1)`tref : $($refText[$i])")
                    $lines.Add("LINE $($i+1)`tport: $($natText[$i])")
                    $shown++
                }
            }
        }
    } else {
        # Parallel: completion order is whatever the pool produced, on both
        # sides, so position means nothing. Everything else still does.
        $a = @($refText | Sort-Object)
        $b = @($natText | Sort-Object)
        if (($a -join "`n") -ne ($b -join "`n")) {
            $stdoutOk = $false
            $lines.Add("STDOUT`tthe two line multisets differ")
            $onlyA = @(Compare-Object $a $b | Where-Object { $_.SideIndicator -eq "<=" })
            $onlyB = @(Compare-Object $a $b | Where-Object { $_.SideIndicator -eq "=>" })
            foreach ($x in ($onlyA | Select-Object -First 20)) { $lines.Add("ONLY-REF-LINE`t$($x.InputObject)") }
            foreach ($x in ($onlyB | Select-Object -First 20)) { $lines.Add("ONLY-PORT-LINE`t$($x.InputObject)") }
        }
    }
    if (-not $stdoutOk) { $problems++ }

    # --- put the disk back ----------------------------------------------------
    #
    # The junction FIRST, so nothing below can walk through it into two
    # gigabytes of somebody's demos.
    if (Test-Path $stageJn) { cmd /c rmdir "$stageJn" | Out-Null }
    if (Test-Path $stageJn) {
        Write-Host "[!] the demo junction is still there; NOT deleting $stage"
    } else {
        Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
    }

    $lines.Add("")
    $lines.Add("identical files: $same")
    $lines.Add("differing:       $differ")
    $lines.Add("only reference:  $onlyRef")
    $lines.Add("only port:       $onlyNat")
    $lines.Add("stdout:          $(if ($stdoutOk) { 'identical' } else { 'DIFFERS' })")
    $exReport = Join-Path $Work "report-extract.txt"
    Set-Content -Path $exReport -Value $lines -Encoding UTF8

    Write-Host ""
    Write-Host ("  identical   {0} files" -f $same)
    Write-Host ("  DIFFERING   {0}" -f $differ)
    Write-Host ("  only ref    {0}" -f $onlyRef)
    Write-Host ("  only port   {0}" -f $onlyNat)
    Write-Host ("  stdout      {0}" -f $(if ($stdoutOk) { "identical" } else { "DIFFERS" }))
    Write-Host ("  report      {0}" -f $exReport)
    Write-Host ("  logs        {0}" -f $refLog)
    Write-Host ("              {0}" -f $natLog)
    Write-Host ""
    if ($problems -gt 0) { Write-Host "=== PARITY FAILED ==="; exit 1 }
    if ($same -eq 0) {
        Write-Host "=== NOTHING WAS COMPARED (no demos selected?) ==="
        exit 1
    }
    Write-Host "=== parity holds ==="
    exit 0
}

# ---------------------------------------------------------------------------
# -Verb budget   --   where the numbers in wr_jobs.h came from
# ---------------------------------------------------------------------------
#
# Not a comparison: the port only, over a real library, reporting the
# distribution of what one demo costs. WR_JOBS_COST_MULT is the one constant in
# this project that could not be derived from anything, and a guessed memory
# budget inside somebody else's game is not a thing to guess.
#
# The peak is the larger of two moments: the whole file and its decompressed
# body alive at once, and the body plus the candidate arena plus the points.
# 70 bytes per candidate is the arena: 16 for the candidate itself and 54 for
# the eight index arrays, two bitmaps and three scratch buffers the dynamic
# program needs beside it.
#
#     tests\parity.ps1 -Game "..." -Verb budget

if ($Verb -eq "budget") {
    $tsv = Join-Path $Work "budget.tsv"
    Write-Host ""
    Write-Host "=== wrlines memory budget ==="
    Write-Host "  port        $exe"
    Write-Host "  game        $Game"
    Write-Host ""

    $args = @("--game", $Game, "--all", "--dump-info", $tsv)
    if ($Map) { $args = @("--game", $Game, "--map", $Map, "--dump-info", $tsv) }
    if ($Limit -gt 0) { $args += @("--limit", "$Limit") }
    & $exe @args

    $rows = Import-Csv -Path $tsv -Delimiter "`t" | Where-Object { $_.status -eq "ok" }
    $mult = @()
    foreach ($r in $rows) {
        $fb = [double]$r.file_bytes
        if ($fb -le 0) { continue }
        $bb = [double]$r.body_bytes
        $peak = [Math]::Max($fb + $bb, $bb + 70 * [double]$r.candidates + 48 * [double]$r.samples)
        $mult += ($peak / $fb)
    }
    $mult = @($mult | Sort-Object)
    if ($mult.Count -eq 0) { Write-Host "[!] no rows"; exit 1 }

    function Pct($a, $p) { return $a[[int]([Math]::Floor($p * ($a.Count - 1)))] }
    Write-Host ""
    Write-Host ("  demos       {0}" -f $mult.Count)
    Write-Host ("  peak/file   min {0:n1}  median {1:n1}  p95 {2:n1}  p99 {3:n1}  max {4:n1}" -f `
        $mult[0], (Pct $mult 0.5), (Pct $mult 0.95), (Pct $mult 0.99), $mult[-1])
    Write-Host ""
    Write-Host ("  WR_JOBS_COST_MULT should be at least {0}" -f [int][Math]::Ceiling((Pct $mult 0.99)))
    Write-Host ("  the largest single demo would want {0:n0} MB" -f `
        (($rows | ForEach-Object { [double]$_.file_bytes } | Measure-Object -Maximum).Maximum * (Pct $mult 0.99) / 1MB))
    Write-Host ""
    exit 0
}

# ---------------------------------------------------------------------------
# -Verb body
# ---------------------------------------------------------------------------

Write-Host ""
Write-Host "=== wrlines parity: --dump-body ==="
Write-Host "  reference   $ref"
Write-Host "  python      $pyVersion"
Write-Host "  port        $exe"
Write-Host "  game        $Game"
Write-Host "  scratch     $Work"

# The two trees a demo can live in: the game's own, and the ones --fetch
# downloaded. Sorted so that two runs cover them in the same order and a report
# from one can be compared with a report from the next.
$demos = @()
$momtv = Join-Path $Game "momentum\momtv"
if (Test-Path $momtv) {
    $demos += Get-ChildItem -Path $momtv -Recurse -Filter *.mtv -File
}
$ours = Join-Path $root "wrlines_data\demos"
if (Test-Path $ours) {
    $demos += Get-ChildItem -Path $ours -Recurse -Filter *.mtv -File
}
$demos = $demos | Sort-Object FullName
if ($Limit -gt 0 -and $demos.Count -gt $Limit) { $demos = $demos[0..($Limit - 1)] }

Write-Host "  demos       $($demos.Count)"
Write-Host ""

$same = 0; $skipped = 0; $noOracle = 0; $mismatch = 0; $uncompared = 0
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("wrlines parity --dump-body")
$lines.Add("python: $pyVersion")
$lines.Add("game:   $Game")
$lines.Add("")

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$i = 0
foreach ($d in $demos) {
    $i++
    if ($i % 100 -eq 0) {
        $rate = $i / [Math]::Max($sw.Elapsed.TotalSeconds, 0.001)
        $left = [int](($demos.Count - $i) / [Math]::Max($rate, 0.001))
        Write-Host ("  [{0}/{1}]  {2} same, {3} skipped, {4} no oracle, {5} uncompared, {6} MISMATCH  (~{7}s left)" -f `
            $i, $demos.Count, $same, $skipped, $noOracle, $uncompared, $mismatch, $left)
    }

    Remove-Item -Force -ErrorAction SilentlyContinue $outPath, $refBin, $natBin

    $refOut = & $Python @PyPre $ref --game $Game --file $d.FullName --dump-body $outPath 2>$errPath
    $refExit = $LASTEXITCODE
    $refOut = ($refOut | Out-String).Trim()
    if (Test-Path $outPath) { Move-Item -Force $outPath $refBin }

    $natOut = & $exe --game $Game --file $d.FullName --dump-body $outPath 2>$errPath
    $natExit = $LASTEXITCODE
    $natOut = ($natOut | Out-String).Trim()
    if (Test-Path $outPath) { Move-Item -Force $outPath $natBin }

    # The reference could not read this one at all. No oracle exists, so the
    # only claim to check is that the port did not read it either.
    if ($refExit -ne 0 -and -not $refOut) {
        if ($natExit -eq 0) {
            $mismatch++
            $lines.Add("EXTRACTED-BUT-REFERENCE-THREW`t$($d.FullName)`t$natOut")
        } else {
            $noOracle++
        }
        continue
    }

    # THE ORACLE HAS NO CODEC FOR THIS ONE AND THE PORT DOES. That is not a
    # mismatch -- nothing was compared, because the reference produced no bytes.
    #
    # This case did not exist when the `skipped` bucket below was written: it
    # assumed that if the reference could not read a zstd body then neither
    # could the port, so both would refuse in the same words and fall through
    # together. The port links zstd now, so the two sides are asymmetric and
    # every zstd demo was landing in LINE -- 317 of them in one run, reported in
    # the vocabulary of a byte difference.
    #
    # The pre-flight at the top of this script is what stops that happening at
    # all. This is the second line of defence, and the reason it must be its own
    # bucket rather than a quiet pass: a REAL regression in these files would
    # otherwise be indistinguishable from a missing pip package.
    if ($refOut -match 'no zstandard installed' -and $natExit -eq 0) {
        $uncompared++
        $lines.Add("UNCOMPARED`t$($d.FullName)`tref: $refOut`tport: $natOut")
        continue
    }

    if ($refOut -ne $natOut) {
        $mismatch++
        $lines.Add("LINE`t$($d.FullName)`tref: $refOut`tport: $natOut")
        continue
    }
    if ($refExit -ne $natExit) {
        $mismatch++
        $lines.Add("EXIT`t$($d.FullName)`tref: $refExit`tport: $natExit")
        continue
    }

    # Both refused for the same stated reason, e.g. a zstd body. Nothing was
    # written, and nothing is being claimed about what would have been.
    if ($refExit -ne 0) { $skipped++; continue }

    $a = (Get-FileHash -Algorithm SHA256 $refBin).Hash
    $b = (Get-FileHash -Algorithm SHA256 $natBin).Hash
    if ($a -ne $b) {
        $mismatch++
        $lines.Add("BYTES`t$($d.FullName)`tref: $a`tport: $b")
    } else {
        $same++
    }
}
$sw.Stop()

$lines.Add("")
$lines.Add("identical:  $same")
$lines.Add("skipped:    $skipped")
$lines.Add("no oracle:  $noOracle")
$lines.Add("UNCOMPARED: $uncompared")
$lines.Add("MISMATCH:   $mismatch")
Set-Content -Path $report -Value $lines -Encoding UTF8

Write-Host ""
Write-Host ("  identical   {0}" -f $same)
Write-Host ("  skipped     {0}   (both refused, same words)" -f $skipped)
Write-Host ("  no oracle   {0}   (reference threw; the port refused too)" -f $noOracle)
Write-Host ("  UNCOMPARED  {0}   (oracle has no codec; NOTHING was checked)" -f $uncompared)
Write-Host ("  MISMATCH    {0}" -f $mismatch)
Write-Host ("  took        {0:n0}s" -f $sw.Elapsed.TotalSeconds)
Write-Host "  report      $report"
Write-Host ""

Remove-Item -Force -ErrorAction SilentlyContinue $outPath, $refBin, $natBin, $errPath
if ($mismatch -gt 0) { Write-Host "=== PARITY FAILED ==="; exit 1 }

# UNCOMPARED is not a pass. It is a hole in the corpus, and a run that ended
# with one has not made the claim this script exists to make -- so it exits
# non-zero, in its own words rather than in the words of a byte difference. The
# pre-flight should mean this is never reached.
if ($uncompared -gt 0) {
    Write-Host "=== PARITY INCOMPLETE: $uncompared demos were never compared ==="
    Write-Host "    The reference could not decompress them. See the report."
    exit 1
}
Write-Host "=== parity holds ==="
exit 0
