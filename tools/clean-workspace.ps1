<#
.SYNOPSIS
    Preview or remove known generated build directories. Releases and private files are preserved.
.EXAMPLE
    .\tools\clean-workspace.ps1
.EXAMPLE
    .\tools\clean-workspace.ps1 -Apply
.EXAMPLE
    .\tools\clean-workspace.ps1 -IncludeBuildCaches -Apply
#>
[CmdletBinding(SupportsShouldProcess)]
param([switch]$Apply, [switch]$IncludeBuildCaches)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\')
if (!(Test-Path -LiteralPath (Join-Path $root 'CMakeLists.txt')) -or
    !(Test-Path -LiteralPath (Join-Path $root 'build_installer.ps1'))) {
    throw 'Repository root could not be verified.'
}
$relativePaths = @(
    'installer/AmneziaVPN.Installer/bin', 'installer/AmneziaVPN.Installer/obj',
    'installer/AmneziaVPN.Installer/Payload',
    'launcher/SelfvpsBuildStudio/bin', 'launcher/SelfvpsBuildStudio/obj',
    'tests/installer-regressions/bin', 'tests/installer-regressions/obj',
    'tests/__pycache__', 'tools/__pycache__', 'tools/anytls-security/__pycache__',
    'build-installer/installer_stage', 'build-installer/installer_publish',
    'build-installer/installer_publish_test', '.mypy_cache', '.pytest_cache'
)
if ($IncludeBuildCaches) { $relativePaths += @('build', 'build-installer', 'deploy/build', 'deploy/build-android-release', 'deploy/build-android-verify') }
$buildRoot = Join-Path $root 'build-installer'
if (Test-Path -LiteralPath $buildRoot) {
    $relativePaths += @(Get-ChildItem -LiteralPath $buildRoot -Directory |
        Where-Object Name -Match '^installer_stage_[0-9]+$' |
        ForEach-Object { 'build-installer/' + $_.Name })
}
function Assert-SafeTree([string]$path) {
    $full = [IO.Path]::GetFullPath($path)
    if (!$full.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing path outside repository: $full"
    }
    # Reject junctions/symlinks on the path and in its contents before recursion.
    $parent = $full
    while ($parent.Length -gt $root.Length) {
        if ((Get-Item -LiteralPath $parent -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Refusing reparse point: $parent"
        }
        $parent = Split-Path -Parent $parent
    }
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($full)
    while ($pending.Count) {
        foreach ($item in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Refusing reparse point: $($item.FullName)"
            }
            if ($item.PSIsContainer) { $pending.Push($item.FullName) }
        }
    }
}
$targets = @($relativePaths | Sort-Object -Unique | ForEach-Object {
    $path = [IO.Path]::GetFullPath((Join-Path $root $_))
    if (Test-Path -LiteralPath $path) { $path }
})
# Skip descendants when a full build cache is selected.
$targets = @($targets | Where-Object {
    $candidate = $_
    !($targets | Where-Object { $candidate.StartsWith($_ + '\', [StringComparison]::OrdinalIgnoreCase) })
})
if ($Apply -and (Get-Process -Name cmake,MSBuild,ninja,dotnet -ErrorAction SilentlyContinue)) {
    throw 'Build processes are running. Stop builds before applying cleanup.'
}
foreach ($target in $targets) {
    Assert-SafeTree $target
    $tracked = @(& git -C $root ls-files -- ($target.Substring($root.Length + 1).Replace('\','/')))
    if ($LASTEXITCODE -ne 0 -or $tracked.Count) { throw "Refusing tracked or unverified directory: $target" }
}
foreach ($target in $targets) {
    $bytes = (Get-ChildItem -LiteralPath $target -Recurse -File -Force | Measure-Object Length -Sum).Sum
    [pscustomobject]@{ Directory = $target; MiB = [math]::Round($bytes / 1MB, 1); Apply = [bool]$Apply }
    if ($Apply -and $PSCmdlet.ShouldProcess($target, 'Remove generated directory')) {
        Remove-Item -LiteralPath $target -Recurse -Force
    }
}
if (!$Apply) { Write-Host 'Preview only. Add -Apply to remove these directories; dist/ and .local/ are preserved.' }
