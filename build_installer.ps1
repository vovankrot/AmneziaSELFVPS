<#
.SYNOPSIS
    Build AmneziaVPN Windows installer with the custom WPF setup UI (and optionally Android APK).
.DESCRIPTION
    1. CMake configure + build (Release)
    2. windeployqt for client and service
    3. Copy prebuilt binaries, deploy data
    4. Pack staged payload into the WPF installer and publish the setup executable
    5. (Optional) Build Android APK via build_android.ps1
.PARAMETER SkipBuild
    Skip CMake build step (use existing build).
.PARAMETER SkipAndroidApk
    Skip Android APK build step.
.PARAMETER QtDir
    Path to Qt MSVC directory. Default: C:\Qt\6.8.3\msvc2022_64.
.PARAMETER BuildDir
    Path to the native Windows CMake build directory. Default: .\build-installer.
.PARAMETER AndroidQtRoot
    Path to Qt Android root. Default: C:\QtAndroid\6.8.3.
.PARAMETER AndroidApkAbi
    Android ABI list for APK build. Supports a single ABI, a semicolon-delimited list, or 'all'.
.PARAMETER InstallerProjectDir
    Path to the custom WPF installer project. Default: .\installer\AmneziaVPN.Installer.
.PARAMETER AndroidBuildType
    Build type for Android APK: Debug or Release. Default: Debug.
    Release requires a signing keystore.
#>
param(
    [switch]$SkipBuild,
    [switch]$SkipAndroidApk,
    [switch]$NoElevate,
    [string]$QtDir = "C:\Qt\6.8.3\msvc2022_64",
    [string]$BuildDir,
    [string]$AndroidQtRoot = "C:\QtAndroid\6.8.3",
    [string]$AndroidSdkRoot = "C:\android-sdk",
    [string]$AndroidNdkRoot = "C:\android-sdk\ndk\27.2.12479018",
    [string]$AndroidApkAbi = "arm64-v8a",
    [string]$InstallerProjectDir,
    [string]$AndroidBuildPlatform,
    [string]$AndroidBuildRoot,
    [string]$AndroidJavaHome,
    [ValidateSet('Debug', 'Release')]
    [string]$AndroidBuildType = 'Debug'
)

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-RelaunchArgumentList {
    param(
        [hashtable]$BoundParameters,
        [object[]]$UnboundArguments
    )

    $argumentList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath)

    foreach ($entry in ($BoundParameters.GetEnumerator() | Sort-Object Key)) {
        $name = "-$($entry.Key)"
        $value = $entry.Value

        if ($value -is [System.Management.Automation.SwitchParameter]) {
            if ($value.IsPresent) {
                $argumentList += $name
            }
            continue
        }

        if ($value -is [System.Array]) {
            foreach ($item in $value) {
                $argumentList += $name
                $argumentList += [string]$item
            }
            continue
        }

        $argumentList += $name
        $argumentList += [string]$value
    }

    if ($UnboundArguments) {
        $argumentList += $UnboundArguments
    }

    return ,$argumentList
}

if (-not $NoElevate -and -not (Test-IsAdministrator)) {
    $relaunchArgumentList = Get-RelaunchArgumentList -BoundParameters $PSBoundParameters -UnboundArguments $MyInvocation.UnboundArguments
    $process = Start-Process -FilePath 'powershell.exe' -ArgumentList $relaunchArgumentList -Verb RunAs -Wait -PassThru
    $exitCode = if ($null -ne $process.ExitCode) { $process.ExitCode } else { 1 }
    exit $exitCode
}

$ErrorActionPreference = "Stop"
$ProjectDir = $PSScriptRoot
if (-not $BuildDir) {
    $BuildDir = Join-Path $ProjectDir "build-installer"
} elseif (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $ProjectDir $BuildDir
}

if (-not $InstallerProjectDir) {
    $InstallerProjectDir = Join-Path $ProjectDir "installer\AmneziaVPN.Installer"
} elseif (-not [System.IO.Path]::IsPathRooted($InstallerProjectDir)) {
    $InstallerProjectDir = Join-Path $ProjectDir $InstallerProjectDir
}

$QtBinDir = Join-Path $QtDir "bin"

if (-not $AndroidBuildRoot) {
    $AndroidBuildRoot = "C:\avpn"
} elseif (-not [System.IO.Path]::IsPathRooted($AndroidBuildRoot)) {
    $AndroidBuildRoot = Join-Path $ProjectDir $AndroidBuildRoot
}

# Derived paths
$ClientRelease = Join-Path $BuildDir "client\Release"
$ServiceRelease = Join-Path $BuildDir "service\server\Release"
$StageDir = Join-Path $BuildDir "installer_stage"
$PrebuiltDir = Join-Path $ProjectDir "client\3rd-prebuilt\deploy-prebuilt\windows\x64"
$DeployDataDir = Join-Path $ProjectDir "deploy\data\windows\x64"
$AppIcon = Join-Path $ProjectDir "client\images\app.ico"
$InstallerProject = Join-Path $InstallerProjectDir "AmneziaVPN.Installer.csproj"
$InstallerPayloadDir = Join-Path $InstallerProjectDir "Payload"
$InstallerPayloadZip = Join-Path $InstallerPayloadDir "payload.zip"
$InstallerPublishDir = Join-Path $BuildDir "installer_publish"
$InstallerPublishExe = Join-Path $InstallerPublishDir "AmneziaVPN.Setup.exe"
$TotalSteps = if ($SkipAndroidApk) { 6 } else { 7 }

function Write-Step {
    param(
        [int]$StepNumber,
        [string]$Message
    )

    Write-Host ("[{0}/{1}] {2}" -f $StepNumber, $TotalSteps, $Message) -ForegroundColor Yellow
}

function Get-CMakeCacheValue {
    param(
        [string]$CacheContent,
        [string]$Name
    )

    $escapedName = [regex]::Escape($Name)
    if ($CacheContent -match "(?m)^$escapedName(?::[^=]*)?=(.*)$") {
        return $Matches[1].Trim()
    }
    return $null
}

function Ensure-CompatibleBuildDir {
    $cachePath = Join-Path $BuildDir "CMakeCache.txt"
    if (-not (Test-Path $cachePath)) { return }

    $cache = Get-Content $cachePath -Raw
    $generator = Get-CMakeCacheValue $cache "CMAKE_GENERATOR"
    $qt6Dir = Get-CMakeCacheValue $cache "Qt6_DIR"
    $expectedQtPrefix = ($QtDir -replace '\\', '/')
    $reasons = @()

    if ($generator -and $generator -ne "Visual Studio 17 2022") {
        $reasons += "generator is '$generator'"
    }
    if ($qt6Dir -and (($qt6Dir -replace '\\', '/') -notlike "$expectedQtPrefix/*")) {
        $reasons += "Qt cache points to '$qt6Dir'"
    }

    if ($reasons.Count -gt 0) {
        Write-Host "  Recreating incompatible build dir: $($reasons -join '; ')" -ForegroundColor Yellow
        Remove-Item $BuildDir -Recurse -Force
    }
}


# Read version from CMakeLists.txt
$cmakeContent = Get-Content (Join-Path $ProjectDir "CMakeLists.txt") -Raw
if ($cmakeContent -match 'set\(AMNEZIAVPN_VERSION\s+([0-9.]+)\)') {
    $AppVersion = $Matches[1]
} else {
    $AppVersion = "0.0.0.0"
}
$AppVersionShort = ($AppVersion -split '\.')[0..2] -join '.'
$Artifacts = @()

if ($AndroidApkAbi -notmatch '^(all|((x86|x86_64|armeabi-v7a|arm64-v8a);)*(x86|x86_64|armeabi-v7a|arm64-v8a))$') {
    Write-Error "AndroidApkAbi must be 'all' or a semicolon-delimited list of x86, x86_64, armeabi-v7a, arm64-v8a."
    exit 1
}

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  AmneziaVPN Release Builder v1.1" -ForegroundColor Cyan
Write-Host "  Version: $AppVersion" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""

# --- Validate tools ---
Write-Step 1 "Checking tools..."

if (-not (Test-Path $InstallerProject)) {
    Write-Error "Installer project not found: $InstallerProject"
    exit 1
}

$windeployqt = Join-Path $QtBinDir "windeployqt.exe"
if (-not (Test-Path $windeployqt)) {
    Write-Error "windeployqt not found at: $windeployqt`nCheck QtDir parameter."
    exit 1
}

$dotnetCommand = Get-Command dotnet -ErrorAction SilentlyContinue
if (-not $dotnetCommand) {
    Write-Error "dotnet SDK not found in PATH. Install .NET 8 SDK or adjust PATH."
    exit 1
}

Write-Host "  Qt:         $QtDir" -ForegroundColor Gray
Write-Host "  dotnet:     $($dotnetCommand.Source)" -ForegroundColor Gray
Write-Host "  Installer:  $InstallerProjectDir" -ForegroundColor Gray

if (-not $SkipAndroidApk) {
    Write-Host "  Android APK: will build via build_android.ps1" -ForegroundColor Gray
}

# --- Build ---
if (-not $SkipBuild) {
    Write-Host ""
    Write-Step 2 "Building project (Release)..."

    Ensure-CompatibleBuildDir

    & cmake -S $ProjectDir -B $BuildDir -G "Visual Studio 17 2022" -A x64 `
        "-DCMAKE_PREFIX_PATH=$QtDir"
    if ($LASTEXITCODE -ne 0) { Write-Error "CMake configure failed"; exit 1 }

    & cmake --build $BuildDir --config Release -- /p:UseMultiToolTask=true /m
    if ($LASTEXITCODE -ne 0) {
        Write-Error "CMake build failed with exit code $LASTEXITCODE"
        exit 1
    }
    Write-Host "  Build OK" -ForegroundColor Green
} else {
    Write-Host ""
    Write-Step 2 "Skipping build (--SkipBuild)"
}

# Verify executables exist
$clientExe = Join-Path $ClientRelease "AmneziaVPN.exe"
$serviceExe = Join-Path $ServiceRelease "AmneziaVPN-service.exe"

if (-not (Test-Path $clientExe)) {
    Write-Error "Client exe not found: $clientExe"
    exit 1
}
if (-not (Test-Path $serviceExe)) {
    Write-Error "Service exe not found: $serviceExe"
    exit 1
}

# --- Prepare staging directory ---
Write-Host ""
Write-Step 3 "Preparing staging directory..."

if (Test-Path $StageDir) {
    Remove-Item $StageDir -Recurse -Force
}
New-Item $StageDir -ItemType Directory -Force | Out-Null

# Copy main executables
Copy-Item $clientExe $StageDir -Force
Copy-Item $serviceExe $StageDir -Force

# Copy icon
if (Test-Path $AppIcon) {
    Copy-Item $AppIcon (Join-Path $StageDir "AmneziaVPN.ico") -Force
}

Write-Host "  Executables copied" -ForegroundColor Gray

# --- windeployqt ---
Write-Host ""
Write-Step 4 "Running windeployqt..."

$env:PATH = "$QtBinDir;$env:PATH"

$stageClientExe = Join-Path $StageDir "AmneziaVPN.exe"
$stageServiceExe = Join-Path $StageDir "AmneziaVPN-service.exe"

$ErrorActionPreference = "Continue"

# Pass 1: deploy DLLs and plugins (WITHOUT --qmldir to avoid ShaderTools scan failure)
& $windeployqt --release --force --no-translations --force-openssl `
    --ignore-library-errors $stageClientExe 2>&1 |
    Where-Object { $_ -notmatch "Unable to find dependent|Cannot open" } |
    Out-Null

& $windeployqt --release --ignore-library-errors $stageServiceExe 2>&1 |
    Where-Object { $_ -notmatch "Unable to find dependent|Cannot open" } |
    Out-Null

# Create qt.conf so Qt finds plugins/QML relative to the exe (not compiled-in paths)
@"
[Paths]
Prefix = .
Plugins = .
Imports = qml
Qml2Imports = qml
"@ | Set-Content (Join-Path $StageDir "qt.conf") -Encoding UTF8

# Copy sqldrivers plugin (needed for Qt6Sql.dll used by QmlLocalStorage)
$sqlPluginSrc = Join-Path $QtDir "plugins\sqldrivers"
$sqlPluginDst = Join-Path $StageDir "sqldrivers"
if (Test-Path $sqlPluginSrc) {
    if (-not (Test-Path $sqlPluginDst)) { New-Item -ItemType Directory $sqlPluginDst -Force | Out-Null }
    Get-ChildItem $sqlPluginSrc -Filter "*.dll" | Where-Object { $_.Name -notmatch 'd\.dll$' } |
        Copy-Item -Destination $sqlPluginDst -Force
}

# Pass 2: deploy QML modules via qmlimportscanner
$qmlScanner = Join-Path $QtBinDir "qmlimportscanner.exe"
$qtQmlDir = Join-Path $QtDir "qml"
$stageQmlDir = Join-Path $StageDir "qml"

$scanJson = & $qmlScanner -rootPath (Join-Path $ProjectDir "client") `
    -importPath $qtQmlDir 2>$null | Out-String
$qmlModules = $scanJson | ConvertFrom-Json |
    Where-Object { $_.type -eq "module" -and $_.relativePath -and $_.relativePath -ne "" } |
    ForEach-Object { $_.relativePath } | Sort-Object -Unique

foreach ($relPath in $qmlModules) {
    $src = Join-Path $qtQmlDir $relPath
    $dst = Join-Path $stageQmlDir $relPath
    if (Test-Path $src) {
        # Replicate directory structure
        Get-ChildItem "$src" -Directory -Recurse | ForEach-Object {
            $subDir = Join-Path $dst $_.FullName.Substring($src.Length)
            if (-not (Test-Path $subDir)) { New-Item -ItemType Directory -Path $subDir -Force | Out-Null }
        }
        if (-not (Test-Path $dst)) { New-Item -ItemType Directory -Path $dst -Force | Out-Null }
        # Copy only release files (skip debug *d.dll, *d.pdb)
        Get-ChildItem "$src" -File -Recurse | Where-Object {
            $_.Name -notmatch '(?<=[a-z0-9])d\.(dll|pdb)$'
        } | ForEach-Object {
            $destFile = Join-Path $dst $_.FullName.Substring($src.Length)
            Copy-Item $_.FullName $destFile -Force
        }
    }
}
Write-Host "  QML modules: $($qmlModules.Count) deployed"

# Force-copy Qt5Compat.GraphicalEffects (qmlimportscanner misses it)
$qt5CompatSrc = Join-Path $qtQmlDir "Qt5Compat"
$qt5CompatDst = Join-Path $stageQmlDir "Qt5Compat"
if (Test-Path $qt5CompatSrc) {
    if (-not (Test-Path $qt5CompatDst)) { New-Item -ItemType Directory -Path $qt5CompatDst -Force | Out-Null }
    Get-ChildItem $qt5CompatSrc -Directory -Recurse | ForEach-Object {
        $subDir = Join-Path $qt5CompatDst $_.FullName.Substring($qt5CompatSrc.Length)
        if (-not (Test-Path $subDir)) { New-Item -ItemType Directory -Path $subDir -Force | Out-Null }
    }
    Get-ChildItem $qt5CompatSrc -File -Recurse | Where-Object {
        $_.Name -notmatch '(?<=[a-z0-9])d\.(dll|pdb)$'
    } | ForEach-Object {
        $destFile = Join-Path $qt5CompatDst $_.FullName.Substring($qt5CompatSrc.Length)
        Copy-Item $_.FullName $destFile -Force
    }
    Write-Host "  Qt5Compat.GraphicalEffects: force-copied"
}

# Pass 3: iteratively copy Qt6 DLLs required by staged DLLs (transitive closure)
# Each iteration scans ALL staged DLLs (not just QML plugins) and copies missing Qt6 deps.
# Repeats until no new DLLs are added — handles chains like Plugin→Qt6A.dll→Qt6B.dll.
$msvcToolsDir = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC" -Directory |
    Sort-Object Name -Descending | Select-Object -First 1
$dumpbin = if ($msvcToolsDir) { Join-Path $msvcToolsDir.FullName "bin\Hostx64\x64\dumpbin.exe" } else { $null }
if (-not $dumpbin -or -not (Test-Path $dumpbin)) { Write-Warning "dumpbin.exe not found"; $dumpbin = $null }
$totalCopied = 0
if ($dumpbin) {
    $iteration = 0
    do {
        $iteration++
        $copiedThisRound = 0
        # Scan ALL DLLs in staging (root + qml subdirs), skip debug
        $allStagedDlls = Get-ChildItem $StageDir -Filter "*.dll" -Recurse |
            Where-Object { $_.Name -notmatch '(?<=[a-z0-9])d\.dll$' }
        foreach ($dll in $allStagedDlls) {
            $deps = & $dumpbin /DEPENDENTS $dll.FullName 2>$null |
                ForEach-Object { $_.Trim() } | Where-Object { $_ -match '^Qt6.*\.dll$' }
            foreach ($dep in $deps) {
                $destPath = Join-Path $StageDir $dep
                if (-not (Test-Path $destPath)) {
                    $srcPath = Join-Path $QtBinDir $dep
                    if (Test-Path $srcPath) {
                        Copy-Item $srcPath $destPath -Force
                        $copiedThisRound++
                    }
                }
            }
        }
        $totalCopied += $copiedThisRound
    } while ($copiedThisRound -gt 0 -and $iteration -lt 10)
}
Write-Host "  Qt6 transitive DLLs: $totalCopied added (${iteration} iterations)"

# Remove plugins whose Qt6 dependencies are unavailable (e.g. Qt6ShaderTools not installed)
Get-ChildItem $stageQmlDir -Filter "*.dll" -Recurse |
    Where-Object { $_.Name -notmatch '(?<=[a-z0-9])d\.dll$' } | ForEach-Object {
    if ($dumpbin) {
        $deps = & $dumpbin /DEPENDENTS $_.FullName 2>$null |
            ForEach-Object { $_.Trim() } | Where-Object { $_ -match '^Qt6.*\.dll$' }
        foreach ($dep in $deps) {
            if (-not (Test-Path (Join-Path $StageDir $dep)) -and -not (Test-Path (Join-Path $QtBinDir $dep))) {
                Write-Host "  Removing $($_.Name) (needs unavailable $dep)"
                Remove-Item $_.FullName -Force
                break
            }
        }
    }
}

# Remove debug DLLs that may have leaked into staging
$debugRemoved = 0
Get-ChildItem $stageQmlDir -Filter "*d.dll" -Recurse | Where-Object {
    $_.Name -match '(?<=[a-z0-9])d\.dll$' -and
    (Test-Path (Join-Path $_.DirectoryName ($_.Name -replace 'd\.dll$', '.dll')))
} | ForEach-Object { Remove-Item $_.FullName -Force; $debugRemoved++ }
if ($debugRemoved -gt 0) { Write-Host "  Debug DLLs removed: $debugRemoved" }

# Remove test QML modules (not needed in production)
$testModules = @("QtTest", "QtQuick\QuickTest")
foreach ($tm in $testModules) {
    $tmPath = Join-Path $stageQmlDir $tm
    if (Test-Path $tmPath) {
        Remove-Item $tmPath -Recurse -Force
        Write-Host "  Removed test module: $tm"
    }
}
# Also remove Qt6QuickTest.dll and Qt6Test.dll from root (test-only)
foreach ($testDll in @("Qt6QuickTest.dll", "Qt6Test.dll")) {
    $tdPath = Join-Path $StageDir $testDll
    if (Test-Path $tdPath) { Remove-Item $tdPath -Force }
}

$ErrorActionPreference = "Stop"

# Verify critical Qt DLLs were deployed
$requiredDlls = @("Qt6Core.dll", "Qt6Quick.dll", "Qt6QuickControls2.dll", "Qt6Qml.dll", "Qt6Gui.dll", "Qt6Network.dll", "Qt6Sql.dll")
foreach ($dll in $requiredDlls) {
    if (-not (Test-Path (Join-Path $StageDir $dll))) {
        Write-Error "windeployqt failed - $dll not found in staging"
        exit 1
    }
}

Write-Host "  windeployqt OK" -ForegroundColor Green

# --- Copy prebuilt and deploy data ---
Write-Host ""
Write-Step 5 "Copying prebuilt binaries and deploy data..."

if (Test-Path $PrebuiltDir) {
    # Copy root-level files
    Get-ChildItem $PrebuiltDir -File | Where-Object { $_.Extension -ne '.sha256' } |
        ForEach-Object { Copy-Item $_.FullName $StageDir -Force }
    # Copy subdirectories (cloak, openvpn, cygwin, ss, tap, xray) preserving structure
    Get-ChildItem $PrebuiltDir -Directory | ForEach-Object {
        $destSub = Join-Path $StageDir $_.Name
        Copy-Item $_.FullName $destSub -Recurse -Force
    }
    $prebuiltCount = (Get-ChildItem $PrebuiltDir -Recurse -File | Where-Object { $_.Extension -ne '.sha256' }).Count
    Write-Host "  Prebuilt: $prebuiltCount files (incl. subdirs)" -ForegroundColor Gray
}

if (Test-Path $DeployDataDir) {
    Copy-Item (Join-Path $DeployDataDir "*") $StageDir -Force
    Write-Host "  Deploy data copied" -ForegroundColor Gray
}

# Copy VC++ Redistributable (auto-detect version)
$vcRedistSrc = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\2022\*\VC\Redist\MSVC" -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | Select-Object -First 1 |
    ForEach-Object { Get-ChildItem $_.FullName -Filter "vc_redist.x64.exe" -Recurse | Select-Object -First 1 -ExpandProperty FullName }
if ($vcRedistSrc -and (Test-Path $vcRedistSrc)) {
    Copy-Item $vcRedistSrc $StageDir -Force
    Write-Host "  VC++ Redistributable copied" -ForegroundColor Gray
} else {
    Write-Warning "vc_redist.x64.exe not found at: $vcRedistSrc"
}

$stageFileCount = (Get-ChildItem $StageDir -Recurse -File).Count
Write-Host "  Total staged: $stageFileCount files" -ForegroundColor Gray

# --- Build custom WPF installer ---
Write-Host ""
Write-Step 6 "Building custom WPF installer..."

$outputExe = "AmneziaVPN_${AppVersionShort}_x64_setup.exe"
$outputPath = Join-Path $ProjectDir $outputExe

if (Test-Path $InstallerPublishDir) {
        Remove-Item $InstallerPublishDir -Recurse -Force
}

New-Item $InstallerPayloadDir -ItemType Directory -Force | Out-Null
if (Test-Path $InstallerPayloadZip) {
        Remove-Item $InstallerPayloadZip -Force
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
        $StageDir,
        $InstallerPayloadZip,
        [System.IO.Compression.CompressionLevel]::Optimal,
        $false)

Write-Host "  Payload archive: $InstallerPayloadZip" -ForegroundColor Gray

& dotnet publish $InstallerProject -c Release -r win-x64 --self-contained true `
        -p:Version=$AppVersionShort `
        -p:FileVersion=$AppVersion `
        -p:AssemblyVersion=$AppVersion `
        -o $InstallerPublishDir
if ($LASTEXITCODE -ne 0) {
        Write-Error "dotnet publish failed for installer project"
        exit 1
}

if (-not (Test-Path $InstallerPublishExe)) {
        $fallbackExe = Get-ChildItem $InstallerPublishDir -Filter "*.exe" -File | Select-Object -First 1 -ExpandProperty FullName
        if ($fallbackExe) {
                $InstallerPublishExe = $fallbackExe
        } else {
                Write-Error "Published installer executable not found in: $InstallerPublishDir"
                exit 1
        }
}

Copy-Item $InstallerPublishExe $outputPath -Force

if (Test-Path $outputPath) {
        $Artifacts += $outputPath
        Write-Host "  Windows installer ready: $outputPath" -ForegroundColor Green
} else {
        Write-Error "Installer file not found after publish"
        exit 1
}

if (-not $SkipAndroidApk) {
    Write-Host ""
    Write-Step 7 "Building Android APK via build_android.ps1..."

    $androidBuildScript = Join-Path $ProjectDir "build_android.ps1"
    if (-not (Test-Path $androidBuildScript)) {
        Write-Error "build_android.ps1 not found at: $androidBuildScript"
        exit 1
    }

    $androidArgs = @{
        AndroidQtRoot      = $AndroidQtRoot
        AndroidSdkRoot     = $AndroidSdkRoot
        AndroidNdkRoot     = $AndroidNdkRoot
        AndroidApkAbi      = $AndroidApkAbi
        AndroidBuildRoot   = $AndroidBuildRoot
        AndroidBuildType   = $AndroidBuildType
        NoElevate          = $true
    }
    if ($AndroidBuildPlatform) { $androidArgs.AndroidBuildPlatform = $AndroidBuildPlatform }
    if ($AndroidJavaHome)      { $androidArgs.AndroidJavaHome = $AndroidJavaHome }

    $androidArtifacts = & $androidBuildScript @androidArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Android APK build failed"
        exit 1
    }
    $Artifacts += @($androidArtifacts)

    if (@($androidArtifacts).Count -eq 0) {
        Write-Error "Android APK build returned no artifacts"
        exit 1
    }

    foreach ($artifact in @($androidArtifacts)) {
        if (Test-Path $artifact) {
            Write-Host "  Android APK ready: $artifact" -ForegroundColor Green
        }
    }
}

Write-Host ""
Write-Host "============================================" -ForegroundColor Green
Write-Host "  DONE! Artifacts ready:" -ForegroundColor Green
foreach ($artifact in $Artifacts) {
    $size = [math]::Round((Get-Item $artifact).Length / 1MB, 1)
    Write-Host "  $artifact (${size} MB)" -ForegroundColor White
}
Write-Host "============================================" -ForegroundColor Green
