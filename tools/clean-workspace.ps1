<#
.SYNOPSIS
    Preview or remove verified generated files. Private diagnostics and current builds are preserved by default.
.EXAMPLE
    .\tools\clean-workspace.ps1
.EXAMPLE
    .\tools\clean-workspace.ps1 -Apply
.EXAMPLE
    .\tools\clean-workspace.ps1 -IncludeBuildCaches -Apply
#>
[CmdletBinding(SupportsShouldProcess)]
param(
    [switch]$Apply,
    [switch]$IncludeBuildCaches,
    [switch]$IncludeObsoleteBuilds,
    [switch]$IncludeCompilerCaches,
    [switch]$PruneOldInstallers
)
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
    'launcher/dist', 'launcher/dist-fd',
    'tests/installer-regressions/bin', 'tests/installer-regressions/obj',
    'tests/__pycache__', 'tools/__pycache__', 'tools/anytls-security/__pycache__',
    'build-installer/installer_stage', 'build-installer/installer_publish',
    'build-installer/installer_publish_test', '.mypy_cache', '.pytest_cache',
    '.local/__pycache__', '.local/installer-integration/bin', '.local/installer-integration/obj'
)
if ($IncludeBuildCaches) { $relativePaths += @('build', 'build-installer', 'deploy/build', 'deploy/build-android-release', 'deploy/build-android-verify') }
if ($IncludeObsoleteBuilds) {
    # These are snapshots made by the builder, not the current CMake tree.
    $relativePaths += @(Get-ChildItem -LiteralPath $root -Directory -Force |
        Where-Object { $_.Name -match '^build-installer\.previous-[0-9]{8}-[0-9]{6}$' -and
            (Test-Path -LiteralPath (Join-Path $_.FullName 'CMakeCache.txt')) } |
        ForEach-Object { $_.Name })
}
if ($IncludeCompilerCaches) { $relativePaths += '.local/go-cache' }
if ($PruneOldInstallers) {
    $releaseRoot = Join-Path $root 'dist'
    if (Test-Path -LiteralPath $releaseRoot) {
        $installers = @(Get-ChildItem -LiteralPath $releaseRoot -File | ForEach-Object {
            if ($_.Name -match '^AmneziaVPN_([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)_x64_setup(?:_[a-zA-Z0-9-]+)?\.exe$') {
                [pscustomobject]@{ File = $_; Version = [version]$Matches[1] }
            }
        })
        # Preserve all variants of the newest two versions, including test builds.
        $keptVersions = @($installers.Version | Sort-Object -Unique -Descending | Select-Object -First 2)
        foreach ($installer in $installers) {
            if ($installer.Version -notin $keptVersions) {
                $relativePaths += 'dist/' + $installer.File.Name
                if (Test-Path -LiteralPath ($installer.File.FullName + '.sha256')) {
                    $relativePaths += 'dist/' + $installer.File.Name + '.sha256'
                }
            }
        }
        foreach ($checksum in Get-ChildItem -LiteralPath $releaseRoot -File -Filter 'AmneziaVPN_*_x64_setup*.exe.sha256') {
            if (!(Test-Path -LiteralPath $checksum.FullName.Substring(0, $checksum.FullName.Length - 7))) {
                $relativePaths += 'dist/' + $checksum.Name
            }
        }
    }
}
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
if ($Apply -and (Get-Process -Name cmake,MSBuild,ninja,dotnet,ctest,go -ErrorAction SilentlyContinue)) {
    throw 'Build processes are running. Stop builds before applying cleanup.'
}
# Do not remove a directory containing an executable that is still running.
if ($Apply) {
    foreach ($process in Get-Process) {
        $executable = $null
        try { $executable = $process.Path } catch { continue }
        if (!$executable) { continue }
        foreach ($target in $targets) {
            if ($executable.Equals($target, [StringComparison]::OrdinalIgnoreCase) -or
                $executable.StartsWith($target + '\', [StringComparison]::OrdinalIgnoreCase)) {
                throw "Cleanup target contains a running process: $($process.Id) in $target"
            }
        }
    }
}
foreach ($target in $targets) {
    Assert-SafeTree $target
    $tracked = @(& git -C $root ls-files -- ($target.Substring($root.Length + 1).Replace('\','/')))
    if ($LASTEXITCODE -ne 0 -or $tracked.Count) { throw "Refusing tracked or unverified directory: $target" }
}
foreach ($target in $targets) {
    $bytes = (Get-ChildItem -LiteralPath $target -Recurse -File -Force | Measure-Object Length -Sum).Sum
    [pscustomobject]@{ Directory = $target; MiB = [math]::Round($bytes / 1MB, 1); Apply = [bool]$Apply }
    if ($Apply -and $PSCmdlet.ShouldProcess($target, 'Remove generated artifact')) {
        Remove-Item -LiteralPath $target -Recurse -Force
    }
}
if (!$Apply) { Write-Host 'Preview only. Add -Apply to remove the selected generated files. Private diagnostics and current releases are preserved.' }
