<#
.SYNOPSIS
    SELFVPS build-environment engine: detect and (best-effort) install the
    prerequisites needed to build the Windows / Android / Linux artifacts.
.DESCRIPTION
    Used both headless and by the Build Studio launcher (launcher\SelfvpsBuildStudio).
    ASCII-only on purpose: Cyrillic breaks Windows PowerShell 5.1 parsing.

    Modes:
      -Detect  <windows|android|linux>   Print a component report. Add -Json for the GUI.
      -Install <componentId> -Platform <p>  Install one component (streams output).
      -InstallAll <windows|android|linux>   Install every missing component for a platform.

    Auto-install uses winget (VS Build Tools, CMake, .NET SDK, JDK, Python, 7-Zip),
    aqtinstall (Qt desktop/android), the Android cmdline-tools sdkmanager, and
    'wsl --install' for the Linux path. Anything that cannot be fully automated prints
    the exact manual command in its 'installHint'.
.NOTES
    Qt desktop default: C:\Qt\6.8.3\msvc2022_64   (matches build_installer.ps1 -QtDir)
    Qt android  default: C:\QtAndroid\6.8.3        (matches build_android.ps1)
#>
[CmdletBinding(DefaultParameterSetName = 'Detect')]
param(
    [Parameter(ParameterSetName = 'Detect', Mandatory = $true)]
    [ValidateSet('windows', 'android', 'linux')]
    [string]$Detect,

    [Parameter(ParameterSetName = 'Install', Mandatory = $true)]
    [string]$Install,

    [Parameter(ParameterSetName = 'Install', Mandatory = $true)]
    [ValidateSet('windows', 'android', 'linux')]
    [string]$Platform,

    [Parameter(ParameterSetName = 'InstallAll', Mandatory = $true)]
    [ValidateSet('windows', 'android', 'linux')]
    [string]$InstallAll,

    [Parameter(ParameterSetName = 'Detect')]
    [switch]$Json,

    [string]$QtVersion = '6.8.3',
    [string]$QtDesktopDir = 'C:\Qt',
    [string]$QtAndroidDir = 'C:\QtAndroid',
    [string]$AndroidSdkRoot = 'C:\android-sdk',
    [string]$AndroidNdkVersion = '27.2.12479018'
)

$ErrorActionPreference = 'Stop'

# ------------------------------------------------------------------ helpers ----

function Test-Command {
    param([string]$Name)
    return [bool](Get-Command $Name -ErrorAction SilentlyContinue)
}

function Get-VsInstallPath {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $p = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null | Select-Object -First 1
        if ($p) { return $p }
    }
    $probe = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\2022' -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'VC\Tools\MSVC') } |
        Select-Object -First 1 -ExpandProperty FullName
    return $probe
}

function Get-DotnetSdks {
    if (-not (Test-Command 'dotnet')) { return @() }
    return (& dotnet --list-sdks 2>$null)
}

function Get-Jdk17Home {
    $patterns = @(
        'C:\Program Files\Microsoft\jdk-*',
        'C:\Program Files\Eclipse Adoptium\jdk-*',
        'C:\Program Files\Java\jdk-*',
        'C:\Program Files\OpenJDK\jdk-*',
        'C:\Program Files\Zulu\zulu-*'
    )
    $candidates = New-Object System.Collections.Generic.List[string]
    if ($env:JAVA_HOME) { $candidates.Add($env:JAVA_HOME) }
    foreach ($pat in $patterns) {
        foreach ($m in (Get-ChildItem $pat -Directory -ErrorAction SilentlyContinue)) {
            $candidates.Add($m.FullName)
        }
    }
    foreach ($c in ($candidates | Select-Object -Unique)) {
        $release = Join-Path $c 'release'
        if (Test-Path $release) {
            $raw = Get-Content $release -Raw
            if ($raw -match 'JAVA_VERSION="([^"]+)"') {
                $major = [int](($Matches[1] -replace '^1\.', '') -split '[\._-]')[0]
                if ($major -ge 17) { return $c }
            }
        }
    }
    return $null
}

function Get-PythonExe {
    foreach ($name in @('python', 'py')) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        # Skip the Windows Store execution-alias stub that only opens the Store.
        if ($cmd -and $cmd.Source -and $cmd.Source -notmatch 'WindowsApps') { return $cmd.Source }
    }
    return $null
}

function Get-WslDistros {
    if (-not (Test-Command 'wsl')) { return @() }
    try {
        $raw = & wsl.exe -l -q 2>$null
        # wsl output is UTF-16; normalise and drop blanks.
        return @($raw | ForEach-Object { ($_ -replace "`0", '').Trim() } | Where-Object { $_ })
    } catch { return @() }
}

# ------------------------------------------------------------- component set ---
# Each component: id, name, group, detect (scriptblock -> $true/$false + sets $script:detail),
# installHint (human), and install (scriptblock that actually runs the install, or $null).

function Get-Components {
    param([string]$TargetPlatform)

    # NOTE: detect scriptblocks are invoked later from Get-Report via '&', so they only
    # see SCRIPT-scope variables (the params below), not Get-Components locals. Compute
    # every path inside the scriptblock from the script params.

    $list = New-Object System.Collections.Generic.List[object]

    $add = {
        param($id, $name, $group, $detect, $hint, $install)
        $list.Add([ordered]@{ id = $id; name = $name; group = $group; detect = $detect; hint = $hint; install = $install })
    }

    # ---- shared: winget presence (info only) ----
    & $add 'winget' 'winget (package manager)' 'base' `
        { $script:detail = if (Test-Command 'winget') { (& winget --version 2>$null) } else { '' }; Test-Command 'winget' } `
        'Ships with Windows 11 / App Installer from the Microsoft Store.' $null

    if ($TargetPlatform -eq 'windows' -or $TargetPlatform -eq 'android') {
        & $add 'python' 'Python 3 (for Qt aqtinstall)' 'base' `
            { $p = Get-PythonExe; $script:detail = $p; [bool]$p } `
            'winget install --id Python.Python.3.12 -e' `
            { Invoke-Winget 'Python.Python.3.12' }
    }

    if ($TargetPlatform -eq 'windows') {
        & $add 'vs2022' 'Visual Studio 2022 (C++ toolset)' 'windows' `
            { $p = Get-VsInstallPath; $script:detail = $p; [bool]$p } `
            'winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.Windows11SDK.22621"' `
            { Install-VsBuildTools }

        & $add 'cmake' 'CMake' 'windows' `
            { if (Test-Command 'cmake') { $script:detail = ((& cmake --version 2>$null) | Select-Object -First 1) } ; Test-Command 'cmake' } `
            'winget install --id Kitware.CMake -e' `
            { Invoke-Winget 'Kitware.CMake' }

        & $add 'dotnet8' '.NET 8+ SDK' 'windows' `
            { $sdks = Get-DotnetSdks; $script:detail = ($sdks | Select-Object -Last 1); [bool]($sdks | Where-Object { $_ -match '^(8|9|1[0-9])\.' }) } `
            'winget install --id Microsoft.DotNet.SDK.8 -e' `
            { Invoke-Winget 'Microsoft.DotNet.SDK.8' }

        & $add 'qt_desktop' "Qt $QtVersion (msvc2022_64)" 'windows' `
            { $wd = Join-Path $QtDesktopDir "$QtVersion\msvc2022_64\bin\windeployqt.exe"
              $script:detail = if (Test-Path $wd) { Split-Path (Split-Path $wd -Parent) -Parent } else { '' }; Test-Path $wd } `
            "aqt install-qt windows desktop $QtVersion win64_msvc2022_64 -O $QtDesktopDir" `
            { Install-QtDesktop }
    }

    if ($TargetPlatform -eq 'android') {
        & $add 'jdk17' 'JDK 17+' 'android' `
            { $h = Get-Jdk17Home; $script:detail = $h; [bool]$h } `
            'winget install --id Microsoft.OpenJDK.17 -e' `
            { Invoke-Winget 'Microsoft.OpenJDK.17' }

        & $add 'cmake' 'CMake' 'android' `
            { if (Test-Command 'cmake') { $script:detail = ((& cmake --version 2>$null) | Select-Object -First 1) } ; Test-Command 'cmake' } `
            'winget install --id Kitware.CMake -e' `
            { Invoke-Winget 'Kitware.CMake' }

        & $add 'qt_android' "Qt $QtVersion (android + host + tools)" 'android' `
            { $armBin  = Join-Path $QtAndroidDir "$QtVersion\android_arm64_v8a\bin"
              $hostQt  = Join-Path $QtAndroidDir "$QtVersion\mingw_64\bin\androiddeployqt.exe"
              $toolsCm = Join-Path $QtAndroidDir 'Tools\CMake_64\bin\cmake.exe'
              $ok = (Test-Path $armBin) -and (Test-Path $hostQt) -and (Test-Path $toolsCm)
              $script:detail = if ($ok) { Join-Path $QtAndroidDir $QtVersion } else { '' }; $ok } `
            "aqt install-qt windows android $QtVersion android_arm64_v8a -O $QtAndroidDir  (+ host win64_mingw + tools_cmake/tools_ninja)" `
            { Install-QtAndroid }

        & $add 'android_sdk' 'Android SDK (cmdline-tools)' 'android' `
            { $ok = (Test-Path (Join-Path $AndroidSdkRoot 'cmdline-tools')) -or (Test-Path (Join-Path $AndroidSdkRoot 'platform-tools'))
              $script:detail = if ($ok) { $AndroidSdkRoot } else { '' }; $ok } `
            "Download commandlinetools, then sdkmanager 'platform-tools' 'platforms;android-35' 'build-tools;34.0.0'" `
            { Install-AndroidSdk }

        & $add 'android_ndk' "Android NDK $AndroidNdkVersion" 'android' `
            { $ndk = Join-Path $AndroidSdkRoot "ndk\$AndroidNdkVersion"
              $script:detail = if (Test-Path $ndk) { $ndk } else { '' }; Test-Path $ndk } `
            "sdkmanager 'ndk;$AndroidNdkVersion'" `
            { Install-AndroidNdk }
    }

    if ($TargetPlatform -eq 'linux') {
        & $add 'wsl' 'WSL2 + Linux distro' 'linux' `
            { $d = Get-WslDistros; $script:detail = ($d -join ', '); [bool]$d } `
            'wsl --install -d Ubuntu   (first run needs a reboot)' `
            { Install-Wsl }

        & $add 'wsl_toolchain' 'Linux build toolchain (in WSL: gcc, cmake, qt6, 7z)' 'linux' `
            { Test-WslToolchain } `
            'wsl sudo apt-get install -y build-essential cmake qt6-base-dev qt6-declarative-dev p7zip-full wget unzip' `
            { Install-WslToolchain }
    }

    return $list
}

# --------------------------------------------------------------- installers ----

function Invoke-Winget {
    param([string]$Id, [string]$Override)
    if (-not (Test-Command 'winget')) { throw "winget is not available. Install 'App Installer' from the Microsoft Store, then retry." }
    Write-Host ">> winget install --id $Id -e --accept-source-agreements --accept-package-agreements" -ForegroundColor Cyan
    $args = @('install', '--id', $Id, '-e', '--accept-source-agreements', '--accept-package-agreements')
    if ($Override) { $args += @('--override', $Override) }
    & winget @args
    # winget returns non-zero for "already installed / no upgrade" too; treat those as success.
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189 -and $LASTEXITCODE -ne -1978335135) {
        throw "winget install $Id failed (exit $LASTEXITCODE)."
    }
}

function Install-VsBuildTools {
    Invoke-Winget 'Microsoft.VisualStudio.2022.BuildTools' `
        '--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
    Write-Host 'VS Build Tools installed (this pulls several GB and may take a while).' -ForegroundColor Green
}

function Ensure-Aqt {
    $py = Get-PythonExe
    if (-not $py) { throw 'Python 3 is required for aqtinstall. Install the Python component first.' }
    Write-Host ">> $py -m pip install --user -U aqtinstall" -ForegroundColor Cyan
    & $py -m pip install --user -U aqtinstall
    if ($LASTEXITCODE -ne 0) { throw 'pip install aqtinstall failed.' }
    return $py
}

function Install-QtDesktop {
    $py = Ensure-Aqt
    Write-Host ">> aqt install-qt windows desktop $QtVersion win64_msvc2022_64 -O $QtDesktopDir" -ForegroundColor Cyan
    & $py -m aqt install-qt windows desktop $QtVersion win64_msvc2022_64 -O $QtDesktopDir
    if ($LASTEXITCODE -ne 0) { throw 'aqt install-qt (desktop) failed.' }
    Write-Host "Qt desktop installed to $QtDesktopDir\$QtVersion\msvc2022_64" -ForegroundColor Green
}

function Install-QtAndroid {
    $py = Ensure-Aqt
    # Android target libs + the mingw host Qt (needed by androiddeployqt/qt-cmake) + tools.
    & $py -m aqt install-qt windows android $QtVersion android_arm64_v8a -O $QtAndroidDir
    if ($LASTEXITCODE -ne 0) { throw 'aqt install-qt (android) failed.' }
    & $py -m aqt install-qt windows desktop $QtVersion win64_mingw -O $QtAndroidDir
    if ($LASTEXITCODE -ne 0) { throw 'aqt install-qt (mingw host) failed.' }
    & $py -m aqt install-tool windows desktop tools_cmake -O $QtAndroidDir
    & $py -m aqt install-tool windows desktop tools_ninja -O $QtAndroidDir
    Write-Host "Qt android stack installed under $QtAndroidDir\$QtVersion" -ForegroundColor Green
    Write-Host "NOTE: verify $QtAndroidDir\Tools\CMake_64 and \Ninja exist; if aqt used different tool names, move them there." -ForegroundColor Yellow
}

function Install-AndroidSdk {
    $tmp = Join-Path $env:TEMP 'avpn-cmdline-tools.zip'
    $url = 'https://dl.google.com/android/repository/commandlinetools-win-11076708_latest.zip'
    Write-Host ">> download $url" -ForegroundColor Cyan
    Invoke-WebRequest -Uri $url -OutFile $tmp
    $cmdlineDst = Join-Path $AndroidSdkRoot 'cmdline-tools\latest'
    New-Item $cmdlineDst -ItemType Directory -Force | Out-Null
    $extract = Join-Path $env:TEMP 'avpn-cmdline-tools'
    if (Test-Path $extract) { Remove-Item $extract -Recurse -Force }
    Expand-Archive $tmp -DestinationPath $extract -Force
    Copy-Item (Join-Path $extract 'cmdline-tools\*') $cmdlineDst -Recurse -Force
    $sdkmanager = Join-Path $cmdlineDst 'bin\sdkmanager.bat'
    $jdk = Get-Jdk17Home
    if ($jdk) { $env:JAVA_HOME = $jdk; $env:PATH = "$jdk\bin;$env:PATH" }
    Write-Host '>> sdkmanager platform-tools platforms;android-35 build-tools;34.0.0' -ForegroundColor Cyan
    & cmd /c "echo y| `"$sdkmanager`" --sdk_root=`"$AndroidSdkRoot`" `"platform-tools`" `"platforms;android-35`" `"build-tools;34.0.0`""
    Write-Host "Android SDK set up at $AndroidSdkRoot" -ForegroundColor Green
}

function Install-AndroidNdk {
    $sdkmanager = Join-Path $AndroidSdkRoot 'cmdline-tools\latest\bin\sdkmanager.bat'
    if (-not (Test-Path $sdkmanager)) { throw 'Android cmdline-tools not found. Install the Android SDK component first.' }
    $jdk = Get-Jdk17Home
    if ($jdk) { $env:JAVA_HOME = $jdk; $env:PATH = "$jdk\bin;$env:PATH" }
    Write-Host ">> sdkmanager ndk;$AndroidNdkVersion" -ForegroundColor Cyan
    & cmd /c "echo y| `"$sdkmanager`" --sdk_root=`"$AndroidSdkRoot`" `"ndk;$AndroidNdkVersion`""
    Write-Host "Android NDK $AndroidNdkVersion installed" -ForegroundColor Green
}

function Install-Wsl {
    Write-Host '>> wsl --install -d Ubuntu' -ForegroundColor Cyan
    & wsl.exe --install -d Ubuntu
    Write-Host 'If this is the first WSL install, REBOOT and finish the Ubuntu user setup, then re-run detection.' -ForegroundColor Yellow
}

function Test-WslToolchain {
    if (-not (Get-WslDistros)) { $script:detail = 'no distro'; return $false }
    try {
        $probe = & wsl.exe bash -lc "command -v gcc >/dev/null && command -v cmake >/dev/null && (command -v qmake6 >/dev/null || dpkg -s qt6-base-dev >/dev/null 2>&1) && echo OK || echo NO" 2>$null
        $probe = ($probe -replace "`0", '').Trim()
        $script:detail = if ($probe -match 'OK') { 'gcc + cmake + qt6' } else { 'missing packages' }
        return ($probe -match 'OK')
    } catch { $script:detail = 'wsl probe failed'; return $false }
}

function Install-WslToolchain {
    if (-not (Get-WslDistros)) { throw 'No WSL distro. Install the WSL component first (and reboot if it is the first time).' }
    Write-Host '>> wsl sudo apt-get update && apt-get install build tools + qt6' -ForegroundColor Cyan
    & wsl.exe bash -lc "sudo apt-get update && sudo apt-get install -y build-essential cmake ninja-build qt6-base-dev qt6-declarative-dev qt6-tools-dev libgl1-mesa-dev p7zip-full wget unzip"
    if ($LASTEXITCODE -ne 0) { throw 'apt-get install failed inside WSL.' }
    Write-Host 'Linux toolchain ready inside WSL.' -ForegroundColor Green
}

# ------------------------------------------------------------------- report ----

function Get-Report {
    param([string]$TargetPlatform)
    $components = Get-Components -TargetPlatform $TargetPlatform
    $report = New-Object System.Collections.Generic.List[object]
    foreach ($c in $components) {
        $script:detail = ''
        $present = $false
        try { $present = [bool](& $c.detect) } catch { $present = $false }
        $report.Add([ordered]@{
            id            = $c.id
            name          = $c.name
            group         = $c.group
            present       = $present
            detail        = ("$script:detail").Trim()
            installHint   = $c.hint
            canAutoInstall = [bool]$c.install
        })
    }
    return $report
}

# -------------------------------------------------------------------- main -----

switch ($PSCmdlet.ParameterSetName) {
    'Detect' {
        $report = Get-Report -TargetPlatform $Detect
        if ($Json) {
            $report | ConvertTo-Json -Depth 5
        } else {
            Write-Host "Prerequisites for '$Detect' build:`n" -ForegroundColor Cyan
            foreach ($r in $report) {
                $mark = if ($r.present) { '[ OK ]' } else { '[ -- ]' }
                $color = if ($r.present) { 'Green' } else { 'Yellow' }
                Write-Host ("  {0} {1}" -f $mark, $r.name) -ForegroundColor $color
                if ($r.detail)      { Write-Host ("         {0}" -f $r.detail) -ForegroundColor Gray }
                if (-not $r.present) { Write-Host ("         install: {0}" -f $r.installHint) -ForegroundColor DarkGray }
            }
            $missing = @($report | Where-Object { -not $_.present -and $_.id -ne 'winget' })
            Write-Host ""
            if ($missing.Count -eq 0) {
                Write-Host "All set - you can build the '$Detect' target." -ForegroundColor Green
            } else {
                Write-Host ("Missing {0} component(s). Run:  .\build_env.ps1 -InstallAll {1}" -f $missing.Count, $Detect) -ForegroundColor Yellow
            }
        }
    }

    'Install' {
        $components = Get-Components -TargetPlatform $Platform
        $c = $components | Where-Object { $_.id -eq $Install } | Select-Object -First 1
        if (-not $c) { Write-Error "Unknown component '$Install' for platform '$Platform'."; exit 1 }
        if (-not $c.install) { Write-Error "Component '$Install' cannot be auto-installed. Do it manually: $($c.hint)"; exit 1 }
        Write-Host "Installing: $($c.name)" -ForegroundColor Cyan
        & $c.install
        Write-Host "Done: $($c.name)" -ForegroundColor Green
    }

    'InstallAll' {
        $report = Get-Report -TargetPlatform $InstallAll
        $components = Get-Components -TargetPlatform $InstallAll
        $missing = @($report | Where-Object { -not $_.present -and $_.canAutoInstall })
        if ($missing.Count -eq 0) {
            Write-Host "Nothing to install - all auto-installable prerequisites for '$InstallAll' are present." -ForegroundColor Green
            exit 0
        }
        Write-Host ("Installing {0} missing component(s) for '{1}'..." -f $missing.Count, $InstallAll) -ForegroundColor Cyan
        foreach ($m in $missing) {
            $c = $components | Where-Object { $_.id -eq $m.id } | Select-Object -First 1
            Write-Host "`n=== $($c.name) ===" -ForegroundColor Cyan
            try { & $c.install } catch { Write-Warning "Failed: $($c.name) - $($_.Exception.Message)`n    Manual: $($c.hint)" }
        }
        Write-Host "`nInstall pass complete. Re-run detection to confirm." -ForegroundColor Green
    }
}
