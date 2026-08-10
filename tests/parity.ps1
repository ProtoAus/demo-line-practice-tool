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
#
# Exit code 0 if every demo with an oracle matched it.

param(
    [Parameter(Mandatory = $true)]
    [string]$Game,

    # Which comparison to run.
    [ValidateSet("body", "board")]
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
$ref = Join-Path $root "wrpath_extract.py"

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

# The interpreter is part of the result. math.dist is version-sensitive and the
# port is bit-compatible with exactly one CPython, so a report that does not say
# which one was on the other side of the comparison is worth less than it looks.
$pyVersion = (& $Python -3 -c "import sys; print(sys.version.replace(chr(10),' '))")

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
            $out = & $Python -3 $ref @common "--board" "--out" $refOut `
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
        $out = & $Python -3 $ref @common "--board" "--out" $refOut `
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

$same = 0; $skipped = 0; $noOracle = 0; $mismatch = 0
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
        Write-Host ("  [{0}/{1}]  {2} same, {3} skipped, {4} no oracle, {5} MISMATCH  (~{6}s left)" -f `
            $i, $demos.Count, $same, $skipped, $noOracle, $mismatch, $left)
    }

    Remove-Item -Force -ErrorAction SilentlyContinue $outPath, $refBin, $natBin

    $refOut = & $Python -3 $ref --game $Game --file $d.FullName --dump-body $outPath 2>$errPath
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
$lines.Add("MISMATCH:   $mismatch")
Set-Content -Path $report -Value $lines -Encoding UTF8

Write-Host ""
Write-Host ("  identical   {0}" -f $same)
Write-Host ("  skipped     {0}   (both refused, same words)" -f $skipped)
Write-Host ("  no oracle   {0}   (reference threw; the port refused too)" -f $noOracle)
Write-Host ("  MISMATCH    {0}" -f $mismatch)
Write-Host ("  took        {0:n0}s" -f $sw.Elapsed.TotalSeconds)
Write-Host "  report      $report"
Write-Host ""

Remove-Item -Force -ErrorAction SilentlyContinue $outPath, $refBin, $natBin, $errPath
if ($mismatch -gt 0) { Write-Host "=== PARITY FAILED ==="; exit 1 }
Write-Host "=== parity holds ==="
exit 0
