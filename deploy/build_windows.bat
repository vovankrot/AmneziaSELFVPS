@ECHO OFF
SETLOCAL ENABLEEXTENSIONS

CHCP 1252 >NUL

set PATH=%QT_BIN_DIR:"=%;%PATH%

set PROJECT_DIR=%cd%
set APP_NAME=AmneziaVPN
set TARGET_FILENAME=%PROJECT_DIR%\%APP_NAME%_x%BUILD_ARCH:"=%.exe
set BUILD_SCRIPT=%PROJECT_DIR%\build_installer.ps1

if "%BUILD_ARCH%"=="" set BUILD_ARCH=64

if /I not "%BUILD_ARCH%"=="64" if /I not "%BUILD_ARCH%"=="x64" (
    echo "Only x64 builds are supported by the custom Windows installer"
    exit /b 1
)

if "%QT_BIN_DIR%"=="" (
    echo "QT_BIN_DIR is not set"
    exit /b 1
)

if not exist "%BUILD_SCRIPT%" (
    echo "build_installer.ps1 was not found at %BUILD_SCRIPT%"
    exit /b 1
)

for %%I in ("%QT_BIN_DIR%\..") do set QT_DIR=%%~fI

echo "Using Qt in %QT_DIR%"
echo "Using installer script %BUILD_SCRIPT%"

del /Q "%TARGET_FILENAME%" 2>NUL

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%BUILD_SCRIPT%" -QtDir "%QT_DIR%" -BuildDir "build-installer" -SkipAndroidApk
if %errorlevel% neq 0 exit /b %errorlevel%

set GENERATED_EXE=
for /f "delims=" %%I in ('dir /b /a:-d /o:-d "%PROJECT_DIR%\AmneziaVPN_*_x64_setup.exe"') do (
    if not defined GENERATED_EXE set GENERATED_EXE=%PROJECT_DIR%\%%I
)

if "%GENERATED_EXE%"=="" (
    echo "Failed to locate generated setup executable"
    exit /b 1
)

copy /Y "%GENERATED_EXE%" "%TARGET_FILENAME%" >NUL
if %errorlevel% neq 0 exit /b %errorlevel%

where signtool >NUL 2>&1
if %errorlevel% equ 0 (
    signtool sign /v /n "Privacy Technologies OU" /fd sha256 /tr http://timestamp.comodoca.com/?td=sha256 /td sha256 "%TARGET_FILENAME%"
    if %errorlevel% neq 0 exit /b %errorlevel%
)

echo "Finished, see %TARGET_FILENAME%"
exit /b 0
