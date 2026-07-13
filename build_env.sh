#!/usr/bin/env bash
# SELFVPS build-environment engine (Linux). Mirrors build_env.ps1's JSON contract
# so the Avalonia Build Studio can use one code path on both OSes.
#
#   ./build_env.sh detect <linux|android> [--json]
#   ./build_env.sh install-all <linux|android>
#
# Linux artifacts build natively (deploy/build_linux.sh / deploy/build_android.sh).
set -uo pipefail

QT_VER="${QT_VERSION:-6.8.3}"

json_escape() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }
has() { command -v "$1" >/dev/null 2>&1; }

COMPS=()      # JSON objects
HUMAN=()      # "present|name|detail|hint"

add() { # id name group present detail hint auto
  COMPS+=("{\"id\":\"$(json_escape "$1")\",\"name\":\"$(json_escape "$2")\",\"group\":\"$(json_escape "$3")\",\"present\":$4,\"detail\":\"$(json_escape "$5")\",\"installHint\":\"$(json_escape "$6")\",\"canAutoInstall\":$7}")
  HUMAN+=("$4|$2|$5|$6")
}

pkg_present() { dpkg -s "$1" >/dev/null 2>&1; }

jdk17_home() {
  local j
  for j in "${JAVA_HOME:-}" /usr/lib/jvm/java-*-openjdk* /usr/lib/jvm/temurin-*; do
    [ -n "$j" ] && [ -x "$j/bin/java" ] || continue
    local v; v=$("$j/bin/java" -version 2>&1 | head -n1 | grep -oE '[0-9]+' | head -n1)
    if [ -n "$v" ] && [ "$v" -ge 17 ] 2>/dev/null; then echo "$j"; return 0; fi
  done
  if has java; then
    local v; v=$(java -version 2>&1 | head -n1 | grep -oE '[0-9]+' | head -n1)
    if [ -n "$v" ] && [ "$v" -ge 17 ] 2>/dev/null; then echo "$(dirname "$(dirname "$(readlink -f "$(command -v java)")")")"; return 0; fi
  fi
  return 1
}

detect_linux() {
  if has gcc && has g++; then add gcc "GCC / G++ (build-essential)" linux true "$(gcc -dumpversion 2>/dev/null)" "" false
  else add gcc "GCC / G++ (build-essential)" linux false "" "sudo apt-get install -y build-essential" true; fi

  if has cmake; then add cmake "CMake" linux true "$(cmake --version 2>/dev/null | head -n1)" "" false
  else add cmake "CMake" linux false "" "sudo apt-get install -y cmake" true; fi

  if has ninja; then add ninja "Ninja" linux true "$(ninja --version 2>/dev/null)" "" false
  else add ninja "Ninja" linux false "" "sudo apt-get install -y ninja-build" true; fi

  if has qmake6 || pkg_present qt6-base-dev; then
    add qt6_base "Qt 6 base (qt6-base-dev)" linux true "$(qmake6 -query QT_VERSION 2>/dev/null)" "" false
  else add qt6_base "Qt 6 base (qt6-base-dev)" linux false "" "sudo apt-get install -y qt6-base-dev qt6-tools-dev" true; fi

  if pkg_present qt6-declarative-dev; then add qt6_qml "Qt 6 Declarative/QML (qt6-declarative-dev)" linux true "installed" "" false
  else add qt6_qml "Qt 6 Declarative/QML (qt6-declarative-dev)" linux false "" "sudo apt-get install -y qt6-declarative-dev" true; fi

  if has 7z || has 7za; then add p7zip "7-Zip (p7zip-full)" linux true "" "" false
  else add p7zip "7-Zip (p7zip-full)" linux false "" "sudo apt-get install -y p7zip-full" true; fi

  if has wget && has unzip; then add wgetunzip "wget + unzip" linux true "" "" false
  else add wgetunzip "wget + unzip" linux false "" "sudo apt-get install -y wget unzip" true; fi
}

detect_android() {
  # Android from Linux is best-effort; the heavy Qt/SDK come from aqt/sdkmanager.
  local jh; if jh=$(jdk17_home); then add jdk17 "JDK 17+" android true "$jh" "" true
  else add jdk17 "JDK 17+" android false "" "sudo apt-get install -y openjdk-17-jdk" true; fi

  if has cmake; then add cmake "CMake" android true "$(cmake --version 2>/dev/null | head -n1)" "" false
  else add cmake "CMake" android false "" "sudo apt-get install -y cmake ninja-build" true; fi

  local qroot="${QT_ANDROID_ROOT:-$HOME/Qt}/$QT_VER/android_arm64_v8a"
  if [ -d "$qroot" ]; then add qt_android "Qt $QT_VER (android)" android true "$qroot" "" false
  else add qt_android "Qt $QT_VER (android)" android false "" "pip install aqtinstall && aqt install-qt linux android $QT_VER android_arm64_v8a -O ~/Qt" false; fi

  local sdk="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
  if [ -d "$sdk/platform-tools" ] || [ -d "$sdk/cmdline-tools" ]; then add android_sdk "Android SDK" android true "$sdk" "" false
  else add android_sdk "Android SDK + NDK" android false "" "install cmdline-tools, then sdkmanager 'platform-tools' 'platforms;android-35' 'ndk;27.2.12479018'" false; fi
}

detect_macos() {
  # Xcode command-line tools give clang + the codesign/pkgbuild toolchain.
  if xcode-select -p >/dev/null 2>&1 && has clang; then add xcode "Xcode command-line tools (clang)" macos true "$(xcode-select -p 2>/dev/null)" "" true
  else add xcode "Xcode command-line tools (clang)" macos false "" "xcode-select --install" true; fi

  if has cmake; then add cmake "CMake" macos true "$(cmake --version 2>/dev/null | head -n1)" "" true
  else add cmake "CMake" macos false "" "brew install cmake ninja" true; fi

  local qbin="${QT_MACOS_ROOT:-$HOME/Qt}/$QT_VER/macos/bin"
  if [ -x "$qbin/qt-cmake" ] || [ -x "$qbin/macdeployqt" ]; then add qt_macos "Qt $QT_VER (macos)" macos true "$qbin" "" false
  else add qt_macos "Qt $QT_VER (macos)" macos false "" "pip3 install aqtinstall && aqt install-qt mac desktop $QT_VER clang_64 -O ~/Qt" false; fi

  # Signing identity is OPTIONAL: build_macos.sh only produces a signed .pkg when
  # MAC_SIGNER_ID / MAC_INSTALLER_SIGNER_ID and the Developer ID certs are present.
  if security find-identity -p codesigning 2>/dev/null | grep -q "Developer ID Application"; then
    add codesign "Developer ID signing identity (optional)" macos true "present" "" false
  else
    add codesign "Developer ID signing identity (optional, for a signed .pkg)" macos false "" "import an Apple Developer ID cert into the keychain + set MAC_SIGNER_ID / MAC_INSTALLER_SIGNER_ID (see deploy/build_macos.sh header)" false
  fi
}

render() {
  local json="$1"
  if [ "$json" = "1" ]; then
    local IFS=,; printf '[%s]\n' "${COMPS[*]}"
    return
  fi
  local missing=0
  echo "Prerequisites:"
  for row in "${HUMAN[@]}"; do
    IFS='|' read -r present name detail hint <<<"$row"
    if [ "$present" = "true" ]; then printf '  [ OK ] %s\n' "$name"
    else printf '  [ -- ] %s\n' "$name"; [ -n "$hint" ] && printf '         install: %s\n' "$hint"; missing=$((missing+1)); fi
    [ -n "$detail" ] && [ "$present" = "true" ] && printf '         %s\n' "$detail"
  done
  echo
  if [ "$missing" -eq 0 ]; then echo "All set."; else echo "Missing $missing. Run: ./build_env.sh install-all <platform>"; fi
}

install_all_linux() {
  echo ">> sudo apt-get update && install linux toolchain"
  sudo apt-get update
  sudo apt-get install -y build-essential cmake ninja-build qt6-base-dev qt6-tools-dev qt6-declarative-dev libgl1-mesa-dev p7zip-full wget unzip
}

install_all_android() {
  echo ">> sudo apt-get install openjdk-17 + build tools"
  sudo apt-get update
  sudo apt-get install -y openjdk-17-jdk cmake ninja-build wget unzip
  echo "NOTE: Qt-for-Android + Android SDK/NDK are not apt packages."
  echo "      Qt:  pip install aqtinstall && aqt install-qt linux android $QT_VER android_arm64_v8a -O ~/Qt"
  echo "      SDK: download commandlinetools, then sdkmanager 'platform-tools' 'platforms;android-35' 'ndk;27.2.12479018'"
}

install_all_macos() {
  if ! xcode-select -p >/dev/null 2>&1; then echo ">> xcode-select --install"; xcode-select --install || true; fi
  if has brew; then echo ">> brew install cmake ninja"; brew install cmake ninja || true
  else echo "Homebrew not found - install it from https://brew.sh, then: brew install cmake ninja"; fi
  echo "NOTE: Qt $QT_VER for macOS via aqt:  pip3 install aqtinstall && aqt install-qt mac desktop $QT_VER clang_64 -O ~/Qt"
  echo "NOTE: a signed .pkg needs an Apple Developer ID cert in the keychain + MAC_SIGNER_ID / MAC_INSTALLER_SIGNER_ID (see deploy/build_macos.sh)."
}

# ------------------------------------------------------------------ dispatch --
cmd="${1:-}"; plat="${2:-linux}"; flag="${3:-}"
case "$cmd" in
  detect)
    case "$plat" in
      linux) detect_linux ;;
      android) detect_android ;;
      macos) detect_macos ;;
      *) echo "unknown platform: $plat" >&2; exit 1 ;;
    esac
    if [ "$flag" = "--json" ]; then render 1; else render 0; fi
    ;;
  install-all)
    case "$plat" in
      linux) install_all_linux ;;
      android) install_all_android ;;
      macos) install_all_macos ;;
      *) echo "unknown platform: $plat" >&2; exit 1 ;;
    esac
    ;;
  *)
    echo "usage: $0 detect <linux|android|macos> [--json] | install-all <linux|android|macos>" >&2
    exit 1 ;;
esac
