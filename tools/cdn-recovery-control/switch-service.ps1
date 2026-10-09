param([Parameter(Mandatory=$true)][ValidateSet('Stage','Restore')][string]$Mode)
$ErrorActionPreference = 'Stop'
$installed = 'C:\Program Files\AmneziaVPN\AmneziaVPN-service.exe'
$candidate = Join-Path $PSScriptRoot 'experimental\AmneziaVPN-service.exe'
$backup = Join-Path $PSScriptRoot 'original-service.exe'
$manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'manifest.json') -Raw | ConvertFrom-Json
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script as administrator only after VPN disconnection and closing AmneziaVPN.'
}
if (Get-Process -Name 'AmneziaVPN' -ErrorAction SilentlyContinue) {
    throw 'Close AmneziaVPN first. This script will not disconnect or terminate the client.'
}
function Get-Sha([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Set-ServiceBinary([string]$Source, [string]$ExpectedHash) {
    $temporary = [IO.Path]::GetFullPath($installed + ".cdn-control-$PID.tmp")
    if ([IO.Path]::GetDirectoryName($temporary) -ne [IO.Path]::GetDirectoryName($installed)) {
        throw 'Temporary path escaped the installation directory.'
    }
    try {
        Copy-Item -LiteralPath $Source -Destination $temporary -Force
        if ((Get-Sha $temporary) -ne $ExpectedHash) { throw 'Staged service hash mismatch.' }
        [IO.File]::Replace($temporary, $installed, [NullString]::Value)
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    }
}
function Stop-TestService {
    $service = Get-Service -Name 'AmneziaVPN-service'
    if ($service.Status -ne 'Stopped') {
        Stop-Service -Name 'AmneziaVPN-service' -ErrorAction Stop
        $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
    }
}
function Start-TestService {
    Start-Service -Name 'AmneziaVPN-service' -ErrorAction Stop
    (Get-Service -Name 'AmneziaVPN-service').WaitForStatus('Running', [TimeSpan]::FromSeconds(30))
}
if ($Mode -eq 'Restore') {
    if ((Get-Sha $installed) -eq $manifest.originalServiceSha256) {
        Write-Output 'Original service already present; no files changed.'
        return
    }
    if ((Get-Sha $installed) -ne $manifest.experimentalServiceSha256) {
        throw 'Installed service changed since this kit was prepared. Refusing to overwrite it.'
    }
    if ((Get-Sha $backup) -ne $manifest.originalServiceSha256) { throw 'Backup hash mismatch.' }
    Stop-TestService
    Set-ServiceBinary $backup $manifest.originalServiceSha256
    Start-TestService
    if ((Get-Sha $installed) -ne $manifest.originalServiceSha256) { throw 'Restored binary hash mismatch.' }
    Write-Output 'Original service restored and running. Open AmneziaVPN and reconnect manually.'
    return
}
if ((Get-Sha $installed) -ne $manifest.originalServiceSha256) {
    throw 'Installed service does not match the original service recorded in this kit.'
}
if ((Get-Sha $candidate) -ne $manifest.experimentalServiceSha256) { throw 'Experimental binary hash mismatch.' }
if (Test-Path -LiteralPath $backup) {
    if ((Get-Sha $backup) -ne $manifest.originalServiceSha256) { throw 'Existing backup hash mismatch.' }
} else { Copy-Item -LiteralPath $installed -Destination $backup }
Stop-TestService
$replaced = $false
try {
    $replaced = $true
    Set-ServiceBinary $candidate $manifest.experimentalServiceSha256
    Start-TestService
    if ((Get-Sha $installed) -ne $manifest.experimentalServiceSha256) { throw 'Experimental binary hash mismatch after copy.' }
    Write-Output 'Experimental service running. Open AmneziaVPN and reconnect manually; run --preflight before the 60-second control.'
} catch {
    $failure = $_
    if ($replaced) {
        Stop-TestService
        Set-ServiceBinary $backup $manifest.originalServiceSha256
    }
    Start-TestService
    throw $failure
}
