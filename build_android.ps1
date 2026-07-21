<#
.SYNOPSIS
    Build AmneziaVPN Android APK (standalone).
.DESCRIPTION
    Configures, compiles, and packages an Android APK for AmneziaVPN.
    Can be called directly or invoked from build_installer.ps1.
.PARAMETER AndroidQtRoot
    Path to Qt Android root. Default: C:\QtAndroid\6.8.3.
.PARAMETER AndroidSdkRoot
    Path to Android SDK root. Default: C:\android-sdk.
.PARAMETER AndroidNdkRoot
    Path to Android NDK root. Default: C:\android-sdk\ndk\27.2.12479018.
.PARAMETER AndroidApkAbi
    ABI list: a single ABI, semicolon-delimited list, or 'all'. Default: arm64-v8a.
.PARAMETER AndroidBuildPlatform
    Android SDK platform (e.g. 'android-35'). Auto-detected from deployment settings if omitted.
.PARAMETER AndroidBuildRoot
    Build output directory. Default: C:\avpn.
.PARAMETER AndroidJavaHome
    Path to JDK 17+ root. Auto-detected if omitted.
.PARAMETER AndroidBuildType
    Build type: Debug or Release. Default: Debug.
.PARAMETER NoElevate
    Do not relaunch as administrator. Used when called from another build script.
#>
param(
    [string]$AndroidQtRoot = "C:\QtAndroid\6.8.3",
    [string]$AndroidSdkRoot = "C:\android-sdk",
    [string]$AndroidNdkRoot = "C:\android-sdk\ndk\27.2.12479018",
    [string]$AndroidApkAbi = "arm64-v8a",
    [string]$AndroidBuildPlatform,
    [string]$AndroidBuildRoot,
    [string]$AndroidJavaHome,
    [ValidateSet('Debug', 'Release')]
    [string]$AndroidBuildType = 'Debug',
    [switch]$NoElevate
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

if (-not $AndroidBuildRoot) {
    $AndroidBuildRoot = "C:\avpn"
} elseif (-not [System.IO.Path]::IsPathRooted($AndroidBuildRoot)) {
    $AndroidBuildRoot = Join-Path $ProjectDir $AndroidBuildRoot
}

# -- Helper functions ----------------------

function Get-FirstExistingPath {
    param([string[]]$Candidates)
    foreach ($candidate in $Candidates) {
        if (Test-Path $candidate) { return $candidate }
    }
    return $null
}

function Resolve-AndroidAbiList {
    param([string]$AbiSpec)
    if ($AbiSpec -eq "all") { return @("x86", "x86_64", "armeabi-v7a", "arm64-v8a") }
    return $AbiSpec -split ';'
}

function Resolve-AndroidQtBinDir {
    param([string]$QtRoot, [string]$AbiSpec)
    $primaryAbi = if ($AbiSpec -eq "all") { "x86_64" } else { ($AbiSpec -split ';')[0] }
    $qtBinSuffix = switch ($primaryAbi) {
        "armeabi-v7a" { "armv7" }
        "arm64-v8a"   { "arm64_v8a" }
        default       { $primaryAbi }
    }
    return (Join-Path $QtRoot "android_$qtBinSuffix\bin")
}

function Get-JavaMajorVersion {
    param([string]$JavaHome)
    $javaExe = Join-Path $JavaHome "bin\java.exe"
    if (-not (Test-Path $javaExe)) { return $null }

    $releaseFile = Join-Path $JavaHome "release"
    if (Test-Path $releaseFile) {
        $releaseContent = Get-Content $releaseFile -Raw
        if ($releaseContent -match 'JAVA_VERSION="([^"]+)"') {
            $versionToken = ($Matches[1] -replace '^1\.', '') -split '[\._-]'
            return [int]$versionToken[0]
        }
    }

    $versionOutput = & $javaExe -version 2>&1 | Out-String
    if ($versionOutput -match 'version "([^"]+)"') {
        $versionToken = ($Matches[1] -replace '^1\.', '') -split '[\._-]'
        return [int]$versionToken[0]
    }
    return $null
}

function Resolve-AndroidJavaHome {
    param([string]$ExplicitJavaHome)

    foreach ($candidate in @($ExplicitJavaHome) | Select-Object -Unique) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        $javaMajorVersion = Get-JavaMajorVersion -JavaHome $candidate
        if ($null -ne $javaMajorVersion -and $javaMajorVersion -ge 17) { return $candidate }
    }

    $discoveredCandidates = New-Object System.Collections.Generic.List[object]
    $candidatePaths = New-Object System.Collections.Generic.List[string]
    if (-not [string]::IsNullOrWhiteSpace($env:JAVA_HOME)) { $candidatePaths.Add($env:JAVA_HOME) }

    foreach ($pattern in @(
        'C:\Program Files\Microsoft\jdk-*',
        'C:\Program Files\Java\jdk-*',
        'C:\Program Files\Eclipse Adoptium\jdk-*',
        'C:\Program Files\OpenJDK\jdk-*',
        'C:\Program Files\Zulu\zulu-*'
    )) {
        foreach ($match in (Get-ChildItem $pattern -Directory -ErrorAction SilentlyContinue)) {
            $candidatePaths.Add($match.FullName)
        }
    }

    foreach ($candidate in ($candidatePaths | Select-Object -Unique)) {
        $javaMajorVersion = Get-JavaMajorVersion -JavaHome $candidate
        if ($null -ne $javaMajorVersion -and $javaMajorVersion -ge 17) {
            $discoveredCandidates.Add([PSCustomObject]@{ JavaHome = $candidate; MajorVersion = $javaMajorVersion })
        }
    }

    $resolvedCandidate = $discoveredCandidates |
        Sort-Object -Property @{ Expression = 'MajorVersion'; Descending = $true },
                               @{ Expression = 'JavaHome'; Descending = $true } |
        Select-Object -First 1
    if ($null -ne $resolvedCandidate) { return $resolvedCandidate.JavaHome }

    throw "JDK 17+ not found. Install JDK 17 or newer, or pass -AndroidJavaHome <path-to-jdk>."
}

# -- Validate parameters ------------------

if ($AndroidApkAbi -notmatch '^(all|((x86|x86_64|armeabi-v7a|arm64-v8a);)*(x86|x86_64|armeabi-v7a|arm64-v8a))$') {
    Write-Error "AndroidApkAbi must be 'all' or a semicolon-delimited list of x86, x86_64, armeabi-v7a, arm64-v8a."
    exit 1
}

# -- Read version -------------------------

$cmakeContent = Get-Content (Join-Path $ProjectDir "CMakeLists.txt") -Raw
if ($cmakeContent -match 'set\(AMNEZIAVPN_VERSION\s+([0-9.]+)\)') {
    $AppVersion = $Matches[1]
} else {
    $AppVersion = "0.0.0.0"
}
$AppVersionShort = ($AppVersion -split '\.')[0..2] -join '.'

# -- Resolve tools ------------------------

$androidQtBinDir = Resolve-AndroidQtBinDir -QtRoot $AndroidQtRoot -AbiSpec $AndroidApkAbi
$androidHostPath = Join-Path $AndroidQtRoot "mingw_64"
$androidToolsRoot = Join-Path (Split-Path $AndroidQtRoot -Parent) "Tools"

$qtCmake = Get-FirstExistingPath -Candidates @(
    (Join-Path $androidQtBinDir "qt-cmake.bat"),
    (Join-Path $androidQtBinDir "qt-cmake.exe"),
    (Join-Path $androidQtBinDir "qt-cmake")
)
$androidDeployQt = Join-Path $androidHostPath "bin\androiddeployqt.exe"
$cmakeExe = Join-Path $androidToolsRoot "CMake_64\bin\cmake.exe"
$ninjaExe = Join-Path $androidToolsRoot "Ninja\ninja.exe"

$ResolvedJavaHome = Resolve-AndroidJavaHome -ExplicitJavaHome $AndroidJavaHome
$javaMajorVersion = Get-JavaMajorVersion -JavaHome $ResolvedJavaHome

foreach ($requiredPath in @($AndroidQtRoot, $AndroidSdkRoot, $AndroidNdkRoot, $androidHostPath, $androidQtBinDir)) {
    if (-not (Test-Path $requiredPath)) { Write-Error "Android path not found: $requiredPath"; exit 1 }
}
foreach ($requiredFile in @($androidDeployQt, $cmakeExe, $ninjaExe)) {
    if (-not (Test-Path $requiredFile)) { Write-Error "Android tool not found: $requiredFile"; exit 1 }
}
if (-not $qtCmake) { Write-Error "qt-cmake not found under: $androidQtBinDir"; exit 1 }
if ($null -eq $javaMajorVersion -or $javaMajorVersion -lt 17) {
    Write-Error "Android build requires JDK 17 or newer. Failed to validate Java at: $ResolvedJavaHome"
    exit 1
}

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  AmneziaVPN Android Builder" -ForegroundColor Cyan
Write-Host "  Version: $AppVersion" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  Qt:   $AndroidQtRoot" -ForegroundColor Gray
Write-Host "  SDK:  $AndroidSdkRoot" -ForegroundColor Gray
Write-Host "  NDK:  $AndroidNdkRoot" -ForegroundColor Gray
Write-Host "  ABI:  $AndroidApkAbi" -ForegroundColor Gray
Write-Host "  JDK:  $ResolvedJavaHome (major $javaMajorVersion)" -ForegroundColor Gray
Write-Host "  Build root: $AndroidBuildRoot" -ForegroundColor Gray
Write-Host "  Build type: $AndroidBuildType" -ForegroundColor Gray
Write-Host ""

# -- Build --------------------------------

$androidBuildDir = $AndroidBuildRoot
$androidGradleHome = Join-Path ([System.IO.Path]::GetPathRoot($androidBuildDir)) ".gradle-avpn"
$Artifacts = @()

if (Test-Path $androidBuildDir) {
    Remove-Item $androidBuildDir -Recurse -Force
}
New-Item $androidBuildDir -ItemType Directory -Force | Out-Null

$previousEnv = @{
    QT_HOST_PATH = $env:QT_HOST_PATH
    QT_ANDROID_ROOT = $env:QT_ANDROID_ROOT
    ANDROID_SDK_ROOT = $env:ANDROID_SDK_ROOT
    ANDROID_HOME = $env:ANDROID_HOME
    ANDROID_NDK_ROOT = $env:ANDROID_NDK_ROOT
    ANDROIDDEPLOYQT_RUN = $env:ANDROIDDEPLOYQT_RUN
    CMAKE_MAKE_PROGRAM = $env:CMAKE_MAKE_PROGRAM
    JAVA_HOME = $env:JAVA_HOME
    AMNEZIA_JAVA_TOOLCHAIN_VERSION = $env:AMNEZIA_JAVA_TOOLCHAIN_VERSION
    GRADLE_USER_HOME = $env:GRADLE_USER_HOME
    PATH = $env:PATH
}

try {
    $env:QT_HOST_PATH = $androidHostPath
    $env:QT_ANDROID_ROOT = $AndroidQtRoot
    $env:ANDROID_SDK_ROOT = $AndroidSdkRoot
    $env:ANDROID_HOME = $AndroidSdkRoot
    $env:ANDROID_NDK_ROOT = $AndroidNdkRoot
    $env:CMAKE_MAKE_PROGRAM = $ninjaExe
    $env:JAVA_HOME = $ResolvedJavaHome
    $env:AMNEZIA_JAVA_TOOLCHAIN_VERSION = [string]$javaMajorVersion
    $env:GRADLE_USER_HOME = $androidGradleHome
    $env:PATH = "$ResolvedJavaHome\bin;$($androidHostPath)\bin;$($androidToolsRoot)\CMake_64\bin;$($androidToolsRoot)\Ninja;$env:PATH"

    # -- Step 1: CMake configure --
    Write-Host "[1/4] Configuring with qt-cmake..." -ForegroundColor Yellow

    $qtCmakeArgs = @(
        '-G', 'Ninja',
        '-S', $ProjectDir,
        '-B', $androidBuildDir,
        '-DQT_NO_GLOBAL_APK_TARGET_PART_OF_ALL=ON',
        '-DCMAKE_BUILD_TYPE=Release'
    )
    if ($AndroidApkAbi -eq 'all') {
        $qtCmakeArgs += '-DQT_ANDROID_BUILD_ALL_ABIS=ON'
    } else {
        $qtCmakeArgs += "-DQT_ANDROID_ABIS=$AndroidApkAbi"
    }

    & $qtCmake @qtCmakeArgs | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Android qt-cmake configure failed with exit code $LASTEXITCODE" }

    # -- Step 2: CMake build --
    Write-Host "[2/4] Building with CMake + Ninja..." -ForegroundColor Yellow

    & $cmakeExe --build $androidBuildDir --config Release | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Android CMake build failed with exit code $LASTEXITCODE" }

    # -- Step 3: androiddeployqt --
    Write-Host "[3/4] Running androiddeployqt..." -ForegroundColor Yellow

    $deploymentSettings = Join-Path $androidBuildDir "client\android-AmneziaVPN-deployment-settings.json"
    $androidBuildOutputDir = Join-Path $androidBuildDir "client\android-build"

    if (-not (Test-Path $deploymentSettings)) {
        throw "Android deployment settings not found: $deploymentSettings"
    }

    $deployQtArgs = @('--input', $deploymentSettings, '--output', $androidBuildOutputDir)
    if ($AndroidBuildType -eq 'Release') { $deployQtArgs += '--release' }
    if ($AndroidBuildPlatform) { $deployQtArgs += @('--android-platform', $AndroidBuildPlatform) }

    $env:ANDROIDDEPLOYQT_RUN = '1'
    & $androidDeployQt @deployQtArgs | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "androiddeployqt failed with exit code $LASTEXITCODE" }

    $generatedGradleProperties = Join-Path $androidBuildOutputDir 'gradle.properties'
    if (-not (Test-Path $generatedGradleProperties)) {
        throw "Generated gradle.properties not found: $generatedGradleProperties"
    }

    $deploymentConfig = Get-Content $deploymentSettings -Raw | ConvertFrom-Json
    $resolvedAbiList = @(Resolve-AndroidAbiList -AbiSpec $AndroidApkAbi)
    $qtGradlePrimaryAbi = if ($AndroidApkAbi -eq 'all') { 'x86_64' } else { $resolvedAbiList[0] }
    $qtGradleQtProperty = $deploymentConfig.qt.PSObject.Properties[$qtGradlePrimaryAbi]
    if ($null -eq $qtGradleQtProperty -or [string]::IsNullOrWhiteSpace($qtGradleQtProperty.Value)) {
        throw "Qt root for ABI '$qtGradlePrimaryAbi' was not found in deployment settings."
    }

    $qtGradleAndroidDir = "$($qtGradleQtProperty.Value)/./src/android/java"
    $qtGradleCompileSdkVersion = $AndroidBuildPlatform
    if ([string]::IsNullOrWhiteSpace($qtGradleCompileSdkVersion)) {
        $qtGradleCompileSdkVersion = "android-$($deploymentConfig.'android-target-sdk-version')"
    }

    $javaHomeForGradle = $ResolvedJavaHome -replace '\\', '/'
    Add-Content -Path $generatedGradleProperties -Value @(
        '',
        '# Restored by build_android.ps1 after androiddeployqt package generation.',
        "androidBuildToolsVersion=$($deploymentConfig.sdkBuildToolsRevision)",
        "androidCompileSdkVersion=$qtGradleCompileSdkVersion",
        "androidNdkVersion=$(Split-Path $AndroidNdkRoot -Leaf)",
        'androidPackageName=org.amnezia.vpn',
        'buildDir=build',
        "qt5AndroidDir=$qtGradleAndroidDir",
        "qtAndroidDir=$qtGradleAndroidDir",
        'qtGradlePluginType=com.android.application',
        "qtMinSdkVersion=$($deploymentConfig.'android-min-sdk-version')",
        "qtTargetAbiList=$($resolvedAbiList -join ',')",
        "qtTargetSdkVersion=$($deploymentConfig.'android-target-sdk-version')",
        '',
        '# Pinned by build_android.ps1 to avoid stale JDK discovery on Windows hosts.',
        "org.gradle.java.home=$javaHomeForGradle",
        'org.gradle.java.installations.auto-detect=false',
        'org.gradle.java.installations.fromEnv=JAVA_HOME',
        "org.gradle.java.installations.paths=$javaHomeForGradle"
    )

    # -- Step 4: Gradle assemble --
    Write-Host "[4/4] Running Gradle $AndroidBuildType assemble..." -ForegroundColor Yellow

    $gradlew = Join-Path $androidBuildOutputDir "gradlew.bat"
    if (-not (Test-Path $gradlew)) { throw "Gradle wrapper not found: $gradlew" }

    $gradleTask = "assemble$AndroidBuildType"
    & $gradlew `
        --no-daemon `
        --project-dir $androidBuildOutputDir `
        "-PamneziaJavaToolchainVersion=$javaMajorVersion" `
        -DexplicitRun=1 `
        $gradleTask | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Gradle $gradleTask failed with exit code $LASTEXITCODE" }

    # -- Collect APK artifacts --
    $apkTypeSuffix = $AndroidBuildType.ToLower()
    $apkOutputDir = Join-Path $androidBuildOutputDir "build\outputs\apk\$apkTypeSuffix"
    $apkMetadataPath = Join-Path $apkOutputDir 'output-metadata.json'
    $apkMetadata = $null
    if (Test-Path $apkMetadataPath) {
        $apkMetadata = Get-Content $apkMetadataPath -Raw | ConvertFrom-Json
    }

    foreach ($abi in (Resolve-AndroidAbiList -AbiSpec $AndroidApkAbi)) {
        $sourceApk = $null

        if ($null -ne $apkMetadata -and $null -ne $apkMetadata.elements) {
            $matchingElement = $null
            foreach ($element in @($apkMetadata.elements)) {
                $elementFilters = @($element.filters)
                if ($elementFilters.Count -eq 0 -and @($resolvedAbiList).Count -eq 1) {
                    $matchingElement = $element
                    break
                }
                $abiFilter = $elementFilters | Where-Object {
                    $_.filterType -eq 'ABI' -and $_.value -eq $abi
                } | Select-Object -First 1
                if ($null -ne $abiFilter) {
                    $matchingElement = $element
                    break
                }
            }
            if ($null -ne $matchingElement -and -not [string]::IsNullOrWhiteSpace($matchingElement.outputFile)) {
                $candidateApk = Join-Path $apkOutputDir $matchingElement.outputFile
                if (Test-Path $candidateApk) { $sourceApk = $candidateApk }
            }
        }

        if (-not $sourceApk) {
            $expectedApk = Join-Path $apkOutputDir "AmneziaVPN-$abi-$apkTypeSuffix.apk"
            if (Test-Path $expectedApk) { $sourceApk = $expectedApk }
        }
        if (-not $sourceApk) {
            $discoveredApks = @(Get-ChildItem $apkOutputDir -Filter '*.apk' -File -ErrorAction SilentlyContinue)
            if ($discoveredApks.Count -eq 1) { $sourceApk = $discoveredApks[0].FullName }
        }

        if (-not (Test-Path $sourceApk)) {
            throw "APK not found for ABI '$abi' under: $apkOutputDir"
        }

        $targetApk = Join-Path $ProjectDir ("AmneziaVPN_{0}_android_{1}_{2}.apk" -f $AppVersionShort, $abi, $apkTypeSuffix)
        Copy-Item $sourceApk $targetApk -Force
        $Artifacts += $targetApk
    }
}
finally {
    foreach ($entry in $previousEnv.GetEnumerator()) {
        if ([string]::IsNullOrEmpty($entry.Value)) {
            Remove-Item "Env:\$($entry.Key)" -ErrorAction SilentlyContinue
        } else {
            Set-Item "Env:\$($entry.Key)" $entry.Value
        }
    }
}

# -- Report -------------------------------

if ($Artifacts.Count -eq 0) {
    Write-Error "Android APK build completed but no artifacts were copied to project root."
    exit 1
}

Write-Host ""
Write-Host "============================================" -ForegroundColor Green
Write-Host "  Android build complete!" -ForegroundColor Green
foreach ($artifact in $Artifacts) {
    $size = [math]::Round((Get-Item $artifact).Length / 1MB, 1)
    Write-Host "  $artifact (${size} MB)" -ForegroundColor White
}
Write-Host "============================================" -ForegroundColor Green

# Return artifact paths (for caller scripts)
return $Artifacts
