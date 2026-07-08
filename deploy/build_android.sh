#!/bin/bash
# shellcheck disable=SC2086

set -o errexit -o nounset

usage() {
  cat <<EOT

Usage:
  build_android [options] <artifact_types>

Build AmneziaVPN android client.

Artifact types:
 -u, --aab                        Build Android App Bundle (AAB)
 -a, --apk (<abi_list> | all)     Build APKs for the specified ABIs or for all available ABIs
                                  Available ABIs: 'x86', 'x86_64', 'armeabi-v7a', 'arm64-v8a'
                                  <abi_list> - list of ABIs delimited by ';'

Options:
 -d, --debug                      Build debug version
 -b, --build-platform <platform>  The SDK platform used for building the Java code of the application
                                  By default, the latest available platform is used
 -m, --move                       Move the build result to the root of the build directory
 -f, --fdroid                     Build for F-Droid
 -h, --help                       Display this help

EOT
}

BUILD_TYPE="release"

get_java_major() {
  local java_cmd=$1
  "$java_cmd" -version 2>&1 | sed -nE 's/.* version "1\.([0-9]+).*/\1/p; s/.* version "([0-9]+).*/\1/p' | head -n 1
}

ensure_tool_on_path() {
  local tool_name=$1
  shift

  if command -v "$tool_name" >/dev/null 2>&1 || command -v "$tool_name.exe" >/dev/null 2>&1; then
    return 0
  fi

  local candidate_dir
  for candidate_dir in "$@"; do
    if [[ -x "$candidate_dir/$tool_name" || -x "$candidate_dir/$tool_name.exe" ]]; then
      export PATH="$candidate_dir:$PATH"
      return 0
    fi
  done

  return 1
}

resolve_java_cmd() {
  if [[ -n "${JAVA_HOME:-}" && -x "$JAVA_HOME/bin/java" ]]; then
    echo "$JAVA_HOME/bin/java"
    return 0
  fi

  if command -v java >/dev/null 2>&1; then
    command -v java
    return 0
  fi

  return 1
}

opts=$(getopt -l debug,aab,apk:,build-platform:,move,fdroid,help -o "dua:b:mfh" -- "$@")
eval set -- "$opts"
while true; do
  case "$1" in
    -d | --debug) BUILD_TYPE="debug"; shift;;
    -u | --aab) AAB=1; shift;;
    -a | --apk) ABIS=$2; shift 2;;
    -b | --build-platform) ANDROID_BUILD_PLATFORM=$2; shift 2;;
    -m | --move) MOVE_RESULT=1; shift;;
    -f | --fdroid) FDROID=1; shift;;
    -h | --help) usage; exit 0;;
    --) shift; break;;
  esac
done

# Validate ABIS parameter
if [[ -v ABIS && \
    ! "$ABIS" = "all" && \
    ! "$ABIS" =~ ^((x86|x86_64|armeabi-v7a|arm64-v8a);)*(x86|x86_64|armeabi-v7a|arm64-v8a)$ ]]; then
  echo "The 'apk' option must be a list of ['x86', 'x86_64', 'armeabi-v7a', 'arm64-v8a']" \
       "delimited by ';' or 'all', but is '$ABIS'"
  exit 1
fi

# At least one artifact type must be specified
if [[ ! (-v AAB || -v ABIS) ]]; then
  usage; exit 0
fi

echo "Build script started..."

PROJECT_DIR=$(pwd)
DEPLOY_DIR=$PROJECT_DIR/deploy

mkdir -p $DEPLOY_DIR/build
BUILD_DIR=$DEPLOY_DIR/build
OUT_APP_DIR=$BUILD_DIR/client

echo "Project dir: $PROJECT_DIR"
echo "Build dir: $BUILD_DIR"

# Determine path to qt bin folder with qt-cmake
if [[ -v AAB || "$ABIS" = "all" ]]; then
  qt_bin_dir_suffix="x86_64"
else
  if [[ $ABIS = *";"* ]]; then
    oneOf=$(echo $ABIS | cut -d';' -f 1)
  else
    oneOf=$ABIS
  fi
  case $oneOf in
    "armeabi-v7a") qt_bin_dir_suffix="armv7";;
    "arm64-v8a") qt_bin_dir_suffix="arm64_v8a";;
    *) qt_bin_dir_suffix=$oneOf;;
  esac
fi
# get real path
# calls on paths containing '..' may result in a 'Permission denied'
QT_BIN_DIR=$(cd $QT_HOST_PATH/../android_$qt_bin_dir_suffix/bin && pwd)

echo "Building App..."

echo "Qt host: $QT_HOST_PATH"
echo "Using Qt in $QT_BIN_DIR"
echo "Using Android SDK in $ANDROID_SDK_ROOT"
echo "Using Android NDK in $ANDROID_NDK_ROOT"

case "$(uname -s 2>/dev/null || echo unknown)" in
  MINGW*|MSYS*|CYGWIN*)
    QT_TOOLS_ROOT=$(cd "$QT_HOST_PATH/../.." && pwd)

    if ! ensure_tool_on_path cmake "$QT_TOOLS_ROOT/Tools/CMake_64/bin"; then
      echo "cmake was not found in PATH or Qt tools. Checked: $QT_TOOLS_ROOT/Tools/CMake_64/bin"
      exit 1
    fi

    if ! ensure_tool_on_path ninja "$QT_TOOLS_ROOT/Tools/Ninja"; then
      echo "ninja was not found in PATH or Qt tools. Checked: $QT_TOOLS_ROOT/Tools/Ninja"
      exit 1
    fi
    ;;
esac

JAVA_CMD=$(resolve_java_cmd || true)
if [[ -z "$JAVA_CMD" ]]; then
  echo "Android build requires Java 17+ but no java executable was found via JAVA_HOME or PATH"
  exit 1
fi

JAVA_MAJOR=$(get_java_major "$JAVA_CMD")
if [[ -z "$JAVA_MAJOR" ]]; then
  echo "Failed to determine Java version from $JAVA_CMD"
  exit 1
fi

if (( JAVA_MAJOR < 17 )); then
  echo "Android build requires Java 17+, but resolved Java is version $JAVA_MAJOR ($JAVA_CMD)"
  exit 1
fi

if [[ -z "${JAVA_HOME:-}" || ! -x "$JAVA_HOME/bin/java" ]]; then
  JAVA_HOME=$(cd "$(dirname "$JAVA_CMD")/.." && pwd)
fi

export JAVA_HOME
export AMNEZIA_JAVA_TOOLCHAIN_VERSION=$JAVA_MAJOR
echo "Using Java toolchain version $AMNEZIA_JAVA_TOOLCHAIN_VERSION via $JAVA_CMD (JAVA_HOME=$JAVA_HOME)"

# Run qt-cmake to configure build
qt_cmake_opts=()
qt_cmake_generator_opts=()

case "$(uname -s 2>/dev/null || echo unknown)" in
  MINGW*|MSYS*|CYGWIN*) qt_cmake_generator_opts+=(-G Ninja);;
esac

if [[ -v AAB || "$ABIS" = "all" ]]; then
  qt_cmake_opts+=(-DQT_ANDROID_BUILD_ALL_ABIS=ON)
else
  qt_cmake_opts+=(-DQT_ANDROID_ABIS="$ABIS")
fi

# QT_NO_GLOBAL_APK_TARGET_PART_OF_ALL=ON - Skip building apks as part of the default 'ALL' target
# We'll build apks during androiddeployqt
$QT_BIN_DIR/qt-cmake "${qt_cmake_generator_opts[@]}" -S $PROJECT_DIR -B $BUILD_DIR \
  -DQT_NO_GLOBAL_APK_TARGET_PART_OF_ALL=ON \
  -DCMAKE_BUILD_TYPE=$BUILD_TYPE \
  "${qt_cmake_opts[@]}"

# Build app
cmake --build $BUILD_DIR --config $BUILD_TYPE

# Build and package APK or AAB
echo "Building APK/AAB..."

deployqt_opts=()

if [ -v AAB ]; then
  deployqt_opts+=(--aab)
fi

if [ -v ANDROID_BUILD_PLATFORM ]; then
  deployqt_opts+=(--android-platform "$ANDROID_BUILD_PLATFORM")
fi

if [ "$BUILD_TYPE" = "release" ]; then
  deployqt_opts+=(--release)
fi

# for gradle to skip all tasks when it is executed by androiddeployqt
# gradle is started later explicitly
export ANDROIDDEPLOYQT_RUN=1

$QT_HOST_PATH/bin/androiddeployqt \
  --input $OUT_APP_DIR/android-AmneziaVPN-deployment-settings.json \
  --output $OUT_APP_DIR/android-build \
  "${deployqt_opts[@]}"

generated_gradle_properties=$OUT_APP_DIR/android-build/gradle.properties
if [[ ! -f "$generated_gradle_properties" ]]; then
  echo "Generated gradle.properties not found: $generated_gradle_properties"
  exit 1
fi

java_home_for_gradle=${JAVA_HOME//\\//}
cat >> "$generated_gradle_properties" <<EOT

# Pinned by build_android.sh to avoid stale JDK discovery on Windows hosts.
org.gradle.java.home=$java_home_for_gradle
org.gradle.java.installations.auto-detect=false
org.gradle.java.installations.fromEnv=JAVA_HOME
org.gradle.java.installations.paths=$java_home_for_gradle
EOT

# run gradle
gradle_opts=()

if [ -v FDROID ]; then
  BUILD_TYPE="fdroid"
fi

if [ -v AAB ]; then
  gradle_opts+=(bundle"${BUILD_TYPE^}")
fi
if [ -v ABIS ]; then
  gradle_opts+=(assemble"${BUILD_TYPE^}")
fi

$OUT_APP_DIR/android-build/gradlew \
  --no-daemon \
  --project-dir $OUT_APP_DIR/android-build \
  -DexplicitRun=1 \
  -PamneziaJavaToolchainVersion=$JAVA_MAJOR \
  "${gradle_opts[@]}"

if [[ -v CI || -v MOVE_RESULT ]]; then
  echo "Moving APK/AAB..."
  if [ -v AAB ]; then
    mv -u $OUT_APP_DIR/android-build/build/outputs/bundle/$BUILD_TYPE/AmneziaVPN-$BUILD_TYPE.aab \
       $PROJECT_DIR/deploy/build/
  fi

  if [ -v ABIS ]; then
    if [ "$ABIS" = "all" ]; then
      ABIS="x86;x86_64;armeabi-v7a;arm64-v8a"
    fi

    suffix=$BUILD_TYPE
    if [ -v FDROID ]; then
      suffix+="-unsigned"
    fi

    IFS=';' read -r -a abi_array <<< "$ABIS"
    for ABI in "${abi_array[@]}"
    do
      mv -u $OUT_APP_DIR/android-build/build/outputs/apk/$BUILD_TYPE/AmneziaVPN-$ABI-$suffix.apk \
       $PROJECT_DIR/deploy/build/
    done
  fi
fi
