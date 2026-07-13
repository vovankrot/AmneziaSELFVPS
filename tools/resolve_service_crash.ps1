param(
    [Parameter(Mandatory = $true)]
    [string]$CrashFile,

    # Default derived from this script's location (tools/ under the repo root), so it
    # works on any checkout. Override with -MapFile for a non-standard build dir.
    [string]$MapFile = (Join-Path $PSScriptRoot "..\build-installer\service\server\Release\AmneziaVPN-service.map")
)

$ErrorActionPreference = "Stop"

function Parse-HexAddress {
    param([string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return $null
    }

    $clean = $Value.Trim()
    if ($clean.StartsWith("0x")) {
        $clean = $clean.Substring(2)
    }

    return [UInt64]::Parse($clean, [System.Globalization.NumberStyles]::HexNumber)
}

function Get-ImageBase {
    param([string[]]$Lines)

    foreach ($line in $Lines) {
        if ($line -match 'Preferred load address is ([0-9A-Fa-f`]+)') {
            return Parse-HexAddress $Matches[1]
        }
    }

    throw "Failed to find Preferred load address in $MapFile"
}

function Get-MapSymbols {
    param([string[]]$Lines, [UInt64]$ImageBase)

    $symbols = New-Object System.Collections.Generic.List[object]
    foreach ($line in $Lines) {
        if ($line -match '^\s*[0-9A-Fa-f]+:[0-9A-Fa-f]+\s+(\S+)\s+([0-9A-Fa-f]{8,16})\b') {
            $name = $Matches[1]
            $absolute = Parse-HexAddress $Matches[2]
            if ($null -ne $absolute -and $absolute -ge $ImageBase) {
                $offset = $absolute - $ImageBase
                $symbols.Add([pscustomobject]@{
                    Name = $name
                    Absolute = $absolute
                    Offset = $offset
                })
            }
        }
    }

    return $symbols | Sort-Object Offset
}

function Resolve-Offset {
    param(
        [UInt64]$Offset,
        [object[]]$Symbols
    )

    $candidate = $null
    foreach ($symbol in $Symbols) {
        if ($symbol.Offset -le $Offset) {
            $candidate = $symbol
            continue
        }
        break
    }

    if ($null -eq $candidate) {
        return [pscustomobject]@{
            Symbol = "<no symbol>"
            Delta = $Offset
        }
    }

    return [pscustomobject]@{
        Symbol = $candidate.Name
        Delta = $Offset - $candidate.Offset
    }
}

if (-not (Test-Path $CrashFile)) {
    throw "Crash file not found: $CrashFile"
}

if (-not (Test-Path $MapFile)) {
    throw "Map file not found: $MapFile"
}

$crashLines = Get-Content $CrashFile
$mapLines = Get-Content $MapFile
$imageBase = Get-ImageBase -Lines $mapLines
$symbols = Get-MapSymbols -Lines $mapLines -ImageBase $imageBase

if (-not $symbols -or $symbols.Count -eq 0) {
    throw "No symbols found in map file: $MapFile"
}

$frames = @()
foreach ($line in $crashLines) {
    if ($line -match '^(Exception frame|\s*#\d+): .*\[([^\]]+)\+0x([0-9A-Fa-f]+),') {
        $label = $Matches[1].Trim()
        $module = $Matches[2].Trim()
        $offset = Parse-HexAddress $Matches[3]

        if ($module -match 'AmneziaVPN-service\.exe$') {
            $resolved = Resolve-Offset -Offset $offset -Symbols $symbols
            $symbol = $resolved.Symbol
            $delta = ('0x{0:X}' -f $resolved.Delta)
        } else {
            $symbol = '<external module>'
            $delta = ''
        }

        $frames += [pscustomobject]@{
            Frame = $label
            Module = $module
            Offset = ('0x{0:X}' -f $offset)
            Symbol = $symbol
            Delta = $delta
        }
    }
}

if ($frames.Count -eq 0) {
    throw "No module+0xOFFSET entries found in crash file. Updated service-crash.txt is required."
}

$frames