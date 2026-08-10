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
    [ValidateSet("body", "board", "fetch")]
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
        & $Python -3 $ref @common "--out" $refOut "--api-record" $setupTape `
                  @setupArgs | Out-Null
        Write-Host ("    {0,-26} {1}" -f "a board to pick from", "recorded")
        for ($i = 0; $i -lt $steps.Count; $i++) {
            $before = Snapshot $dest
            $out = & $Python -3 $ref @common "--out" $refOut `
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
    & $Python -3 $ref @common "--out" $refOut "--api-replay" $setupTape `
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

        $r = Run-Side $Python (@("-3", $ref) + $common + @("--out", $refOut,
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
