<#
.SYNOPSIS
    Publish SELFVPS Build Studio (Avalonia, cross-platform launcher).
.DESCRIPTION
    Framework-dependent single-file (needs the .NET 8 runtime on the target; the
    run-build-studio wrapper installs it). No ReadyToRun + compression + native libs
    bundled keeps it as small as an Avalonia app gets (~50 MB - Skia is bundled).
    Ship the exe via GitHub Releases (it is .gitignored, never committed).
.PARAMETER Rid
    Runtime id: win-x64 (default), linux-x64, osx-x64, osx-arm64.
.PARAMETER SelfContained
    Bundle the .NET runtime too (no runtime needed on target, ~+40 MB).
#>
param(
    [string]$Rid = 'win-x64',
    [switch]$SelfContained
)
$ErrorActionPreference = 'Stop'
$proj = Join-Path $PSScriptRoot 'SelfvpsBuildStudio'
$out  = Join-Path $PSScriptRoot 'dist'

$publishArgs = @(
    'publish', $proj, '-c', 'Release', '-r', $Rid,
    "--self-contained=$($SelfContained.IsPresent.ToString().ToLower())",
    '-p:PublishSingleFile=true',
    '-p:PublishReadyToRun=false',
    '-p:EnableCompressionInSingleFile=true',
    '-p:IncludeNativeLibrariesForSelfExtract=true',
    '-o', $out
)
Write-Host "dotnet $($publishArgs -join ' ')" -ForegroundColor Cyan
& dotnet @publishArgs
if ($LASTEXITCODE -ne 0) { Write-Error 'publish failed'; exit 1 }

$exeName = if ($Rid -like 'win-*') { 'SelfvpsBuildStudio.exe' } else { 'SelfvpsBuildStudio' }
$exe = Join-Path $out $exeName
if (Test-Path $exe) {
    $mb = [math]::Round((Get-Item $exe).Length / 1MB, 1)
    Write-Host "OK: $exe ($mb MB)" -ForegroundColor Green
    Write-Host "Distribute this + the run-studio wrapper via GitHub Releases." -ForegroundColor Gray
} else {
    Write-Error "published exe not found in $out"
}
