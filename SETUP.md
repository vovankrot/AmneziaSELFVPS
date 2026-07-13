# SELFVPS — сборка на новой машине

Есть два пути: **графический** (Build Studio) и **консольный** (скрипты).

---

## 🖥️ Быстрый путь — Build Studio (кросс-платформенный, Avalonia)

Лаунчер `SelfvpsBuildStudio` работает и на **Windows**, и на **Linux**. Он **framework-dependent** (~43 МБ, нужен .NET 8 runtime) — запускай через обёртку, она сама докачает рантайм:

- Windows: **`launcher/run-studio.cmd`**
- Linux: **`launcher/run-studio.sh`**

(Если .NET 8 runtime уже стоит — можно запускать `SelfvpsBuildStudio.exe` напрямую.)

Дальше:
1. Выбери платформу — **Windows / macOS / Android / Linux**. Доступность зависит от хоста: Windows-хост → Windows/Android/Linux(WSL); Linux-хост → Linux/Android; **macOS собирается только на Mac** (нужен Xcode + подпись). Недоступные на текущей ОС карточки притушены.
2. Studio проверит тулчейн и покажет чеклист (✓ / нужно доустановить).
3. **«Доустановить»** — докачает недостающее (Windows: winget/aqt/sdkmanager/WSL; Linux: apt).
4. **«Собрать»** — соберёт артефакт, лог в окне.

> Тяжёлые установки (VS Build Tools, WSL) поднимут UAC сами. WSL при первой установке требует **перезагрузку** — после неё «Обновить».

**Сборка самого лаунчера** (exe в гит не коммитится — раздаётся через GitHub Releases):
```powershell
# Windows-бинарь
.\launcher\publish.ps1 -Rid win-x64
# Linux-бинарь (можно и с Windows кросс-собрать, но запускать на Linux)
.\launcher\publish.ps1 -Rid linux-x64
```

Тот же движок доступен из консоли:
```powershell
.\build_env.ps1 -Detect windows        # Windows-хост (add -Json для машинного вида)
.\build_env.ps1 -InstallAll windows
```
```bash
./build_env.sh detect linux            # Linux-хост
./build_env.sh install-all linux
```

---

## 🔧 Что нужно вручную (если без Studio)

### Windows (клиент + служба + инсталлятор .exe)
| Компонент | Установка |
|---|---|
| Visual Studio 2022 + «Разработка на C++» (MSVC v143 + Win SDK) | `winget install Microsoft.VisualStudio.2022.BuildTools -e --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.Windows11SDK.22621"` |
| CMake | `winget install Kitware.CMake -e` |
| .NET 8 SDK | `winget install Microsoft.DotNet.SDK.8 -e` |
| Qt 6.8.3 (msvc2022_64) → `C:\Qt` | `pip install aqtinstall` → `python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O C:\Qt` |

Сборка:
```powershell
.\build_installer.ps1 -SkipAndroidApk -NoElevate
# → AmneziaVPN_<версия>_x64_setup.exe в корне
```

### Android (APK)
Дополнительно к CMake:
| Компонент | Установка |
|---|---|
| JDK 17+ | `winget install Microsoft.OpenJDK.17 -e` |
| Qt 6.8.3 android + host + tools → `C:\QtAndroid` | `aqt install-qt windows android 6.8.3 android_arm64_v8a -O C:\QtAndroid` (+ `win64_mingw` хост, `tools_cmake`, `tools_ninja`) |
| Android SDK + NDK 27.2.12479018 → `C:\android-sdk` | commandline-tools → `sdkmanager "platform-tools" "platforms;android-35" "build-tools;34.0.0" "ndk;27.2.12479018"` |

Сборка:
```powershell
.\build_android.ps1 -AndroidApkAbi arm64-v8a -AndroidBuildType Debug
# → AmneziaVPN_<версия>_android_arm64-v8a_debug.apk
```

### Linux (.bin инсталлятор — через WSL)
```powershell
wsl --install -d Ubuntu           # первый раз → перезагрузка + настройка пользователя
wsl sudo apt-get install -y build-essential cmake ninja-build qt6-base-dev qt6-declarative-dev qt6-tools-dev libgl1-mesa-dev p7zip-full wget unzip
wsl bash -lc "cd /mnt/<диск>/<путь-к-репо> && ./deploy/build_linux.sh"
# → deploy/AmneziaVPN_Linux_Installer.bin
```

### macOS (.pkg — только на Mac)
Нужен Mac с Xcode command-line tools; Qt 6.8.3 в `~/Qt/6.8.3/macos`. Подпись/нотаризация — опционально (через env, см. шапку `deploy/build_macos.sh`).
```bash
xcode-select --install
brew install cmake ninja
pip3 install aqtinstall && aqt install-qt mac desktop 6.8.3 clang_64 -O ~/Qt
# для подписанного .pkg: export MAC_SIGNER_ID=... MAC_INSTALLER_SIGNER_ID=... (+ Developer ID сертификаты в keychain)
bash deploy/build_macos.sh
# → deploy/build/pkg/AmneziaVPN.pkg
```

---

## ⚠️ Подводные камни

- **Qt строго 6.8.3 / msvc2022_64.** Другая версия → передавай `-QtDir` в `build_installer.ps1`.
- **`cmake` и `dotnet` должны быть в PATH** (или запускай из «Developer PowerShell for VS 2022»).
- `build_installer.ps1` теперь сам находит VS **любой редакции** (Community/Pro/Enterprise/BuildTools) через `vswhere` — для `dumpbin` и VC++ Redistributable. Если C++-тулсет не стоит — падает с понятной подсказкой.
- Qt-докачка использует **aqtinstall** (нужен Python 3). Android SDK/NDK — через **sdkmanager**. Linux — только через **WSL** (нативной сборки под Windows нет).
- Все `.ps1` — **ASCII-only** (кириллица ломает парсинг Windows PowerShell 5.1). Артефакты (README, SETUP.md) — можно на русском.
