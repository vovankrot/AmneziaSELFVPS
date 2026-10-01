param(
    [string]$Root = (Split-Path $PSScriptRoot -Parent),
    [string]$ManifestPath
)
$ErrorActionPreference = 'Stop'
$Root = [IO.Path]::GetFullPath($Root)
if (-not $ManifestPath) { $ManifestPath = Join-Path $Root 'deploy\prebuilt-selfvps\manifest.json' }
$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
if ($manifest.formatVersion -ne 1 -or -not $manifest.files) { throw 'Invalid prebuilt manifest' }
$seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
foreach ($entry in $manifest.files) {
    $path = [IO.Path]::GetFullPath((Join-Path $Root $entry.path))
    if (-not $path.StartsWith($Root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Manifest path escapes repository: $($entry.path)"
    }
    if (-not $seen.Add($path)) { throw "Duplicate manifest file: $($entry.path)" }
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing prebuilt: $($entry.path)" }
    $stream = [IO.File]::OpenRead($path)
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { $actual = ([BitConverter]::ToString($hasher.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $hasher.Dispose() }
    if ($actual -cne $entry.sha256) { throw "SHA-256 mismatch: $($entry.path)" }
}
foreach ($directory in @('deploy\prebuilt-selfvps\windows\x64', 'deploy\prebuilt-selfvps\android', 'client\3rd-prebuilt\deploy-prebuilt\windows\x64')) {
    $folder = Join-Path $Root $directory
    if (-not (Test-Path -LiteralPath $folder)) { continue }
    foreach ($file in (Get-ChildItem -LiteralPath $folder -Recurse -File)) {
        if ($file.Extension -in @('.exe', '.dll', '.sys', '.so', '.inf', '.cat') -and -not $seen.Contains($file.FullName)) {
            throw "Unrecorded prebuilt: $($file.FullName)"
        }
    }
}
Write-Host "Verified $($seen.Count) prebuilt files against release manifest."

$driverEntries = @($manifest.files | Where-Object { $_.path -like 'deploy/prebuilt-selfvps/windows/x64/mullvad-split-tunnel.*' })
if ($driverEntries.Count -gt 0) {
    foreach ($extension in @('sys', 'inf', 'cat')) {
        $required = [IO.Path]::GetFullPath((Join-Path $Root "deploy\prebuilt-selfvps\windows\x64\mullvad-split-tunnel.$extension"))
        if (-not $seen.Contains($required)) { throw "Incomplete driver package: $extension" }
    }
    Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Security\Microsoft.PowerShell.Security.psd1') -ErrorAction Stop
    foreach ($name in @('mullvad-split-tunnel.sys', 'mullvad-split-tunnel.cat')) {
        $file = Join-Path $Root "deploy\prebuilt-selfvps\windows\x64\$name"
        $signature = Get-AuthenticodeSignature -LiteralPath $file
        if ($signature.Status -ne 'Valid') { throw "Invalid driver signature: $name ($($signature.Status))" }
        $publisher = if ($name.EndsWith('.cat')) { 'Microsoft Windows Hardware Compatibility Publisher' } else { 'Mullvad' }
        $expectedSigner = $signature.SignerCertificate.Subject -like "*$publisher*"
        # Windows prefers a registered kernel catalog over the embedded signature.
        # After this package is installed, SYS legitimately reports the WHCP signer.
        $kernelCatalog = $name.EndsWith('.sys') -and $signature.SignatureType -eq 'Catalog' -and
            $signature.SignerCertificate.Subject -like '*CN=Microsoft Windows Hardware Compatibility Publisher,*'
        if (-not ($expectedSigner -or $kernelCatalog)) { throw "Unexpected driver publisher: $name" }
    }
}
