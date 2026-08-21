# symbolise.ps1 -- turn a crash backtrace from wrlines.log into function names.
#
# WrCrashFilter writes a fault as "wrlines.dll+0x19FBD" and nothing more, because
# at the moment it fires it is inside a broken process and must not go looking for
# symbols. This does the looking afterwards, from wrlines.map, which build.bat
# emits beside the DLL. No debugger and no PDB reader involved.
#
# The map MUST be the one built alongside the DLL that crashed. A rebuild moves
# the addresses, so if the log is from an older build the names will be confident
# and wrong -- there is no check for it here, and that is the one thing to be
# careful about.
#
#   .\tests\symbolise.ps1                       reads wrlines_data\wrlines.log
#   .\tests\symbolise.ps1 -Log path\to.log
#   .\tests\symbolise.ps1 -Rva 19FBD,17BDE      resolve offsets directly

[CmdletBinding()]
param(
    [string]   $Log = "",
    [string]   $Map = "",
    [string[]] $Rva = @()
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

if (-not $Map) { $Map = Join-Path $root "wrlines.map" }
if (-not (Test-Path $Map)) {
    Write-Error "no map at $Map -- run build.bat, which emits it beside the DLL"
}

# The map's "Preferred load address is 0000000180000000" line, then every
# "0001:00000010  ?Name@@YAXXZ  0000000180001010  f  file.obj" row.
$lines = Get-Content $Map
$baseMatch = $lines | Select-String 'Preferred load address is ([0-9A-Fa-f]+)' |
             Select-Object -First 1
if (-not $baseMatch) { Write-Error "$Map has no preferred load address line" }
$base = [Convert]::ToUInt64($baseMatch.Matches[0].Groups[1].Value, 16)

$re = [regex]'^\s+[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}\s+(\S+)\s+([0-9A-Fa-f]{8,16})\s'
$syms = [System.Collections.Generic.List[object]]::new()
foreach ($l in $lines) {
    $m = $re.Match($l)
    if ($m.Success) {
        # $unwind$, $chain$ and friends are the compiler's exception tables, not
        # code. They live in .xdata but sort in among the functions, so leaving
        # them in lets a lookup name a frame after a data blob.
        if ($m.Groups[1].Value.StartsWith('$')) { continue }
        $syms.Add([pscustomobject]@{
            Name = $m.Groups[1].Value
            Addr = [Convert]::ToUInt64($m.Groups[2].Value, 16)
        })
    }
}
if ($syms.Count -eq 0) { Write-Error "$Map yielded no symbols" }
$sorted = @($syms | Sort-Object Addr)
$addrs  = [UInt64[]]($sorted | ForEach-Object { $_.Addr })

# NOT named -Rva. A function parameter of the same name as the script's own
# makes PowerShell's binder report failures against the wrong one, which cost a
# confusing "cannot convert String[] to UInt64" pointing at the script line.
function Resolve-Offset([UInt64]$offset) {
    $target = $base + $offset
    # Binary search for the last symbol at or below the target.
    $lo = 0; $hi = $addrs.Length - 1; $best = -1
    while ($lo -le $hi) {
        $mid = [int](($lo + $hi) / 2)
        if ($addrs[$mid] -le $target) { $best = $mid; $lo = $mid + 1 }
        else { $hi = $mid - 1 }
    }
    if ($best -lt 0) { return $null }
    [pscustomobject]@{
        Name   = $sorted[$best].Name
        Offset = $target - $sorted[$best].Addr
    }
}

# Where the offsets come from: either -Rva, or the log's own crash lines.
$wanted = [System.Collections.Generic.List[object]]::new()
if ($Rva.Count -gt 0) {
    foreach ($r in $Rva) {
        $wanted.Add([pscustomobject]@{ Hex = $r.TrimStart('0','x','X','+'); Line = "" })
    }
} else {
    if (-not $Log) {
        $Log = Join-Path $root "wrlines_data\wrlines.log"
        if (-not (Test-Path $Log)) {
            $Log = Join-Path $root "wrlines_data\wrlines.prev.log"
        }
    }
    if (-not (Test-Path $Log)) { Write-Error "no log at $Log" }
    Write-Host "log: $Log"
    Write-Host "map: $Map"
    Write-Host ""
    foreach ($l in Get-Content $Log) {
        # "[!] crash: 0xC0000005 reading 0x0 at wrlines.dll+0x19FBD"
        # "crash:   [ 0] wrlines.dll+0x19FBD"
        $m = [regex]::Match($l, 'wrlines\.dll\+0x([0-9A-Fa-f]+)')
        if ($m.Success) {
            $wanted.Add([pscustomobject]@{ Hex = $m.Groups[1].Value; Line = $l.Trim() })
        }
        elseif ($l -match 'crash:') { Write-Host "   $($l.Trim())" }
    }
    if ($wanted.Count -eq 0) {
        Write-Host "no wrlines.dll frames in that log -- nothing to resolve."
        return
    }
}

Write-Host ""
foreach ($w in $wanted) {
    $hit = Resolve-Offset ([Convert]::ToUInt64($w.Hex, 16))
    if ($hit) {
        Write-Host ("+0x{0,-8} {1} +0x{2:X}" -f $w.Hex.ToUpper(), $hit.Name, $hit.Offset)
    } else {
        Write-Host ("+0x{0,-8} <below the first symbol>" -f $w.Hex.ToUpper())
    }
}
Write-Host ""
Write-Host "Names are only right if this map was built with the DLL that crashed."
