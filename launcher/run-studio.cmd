@echo off
setlocal
rem SELFVPS Build Studio bootstrap (Windows): ensure the .NET 8 runtime, then launch.
rem The launcher exe (framework-dependent, ~50 MB) sits next to this file.

set "EXE=%~dp0SelfvpsBuildStudio.exe"
if not exist "%EXE%" (
  echo SelfvpsBuildStudio.exe not found next to this script.
  echo Build it:  dotnet publish launcher\SelfvpsBuildStudio -c Release -r win-x64
  pause
  exit /b 1
)

set "HAVE=0"
for /f "delims=" %%r in ('dotnet --list-runtimes 2^>nul ^| findstr /C:"Microsoft.NETCore.App 8."') do set "HAVE=1"
if "%HAVE%"=="0" (
  echo .NET 8 runtime not found - installing via winget...
  winget install --id Microsoft.DotNet.Runtime.8 -e --accept-source-agreements --accept-package-agreements
)

start "" "%EXE%"
endlocal
