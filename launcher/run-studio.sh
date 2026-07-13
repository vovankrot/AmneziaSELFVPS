#!/usr/bin/env bash
# SELFVPS Build Studio bootstrap (Linux): ensure the .NET 8 runtime, then launch.
# The launcher binary (framework-dependent) sits next to this file.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
bin="$here/SelfvpsBuildStudio"

if [ ! -x "$bin" ]; then
  echo "SelfvpsBuildStudio not found next to this script."
  echo "Build it:  dotnet publish launcher/SelfvpsBuildStudio -c Release -r linux-x64"
  exit 1
fi

if ! dotnet --list-runtimes 2>/dev/null | grep -q 'Microsoft.NETCore.App 8\.'; then
  echo ".NET 8 runtime not found - installing..."
  if command -v apt-get >/dev/null 2>&1; then
    sudo apt-get update && sudo apt-get install -y dotnet-runtime-8.0 \
      || echo "apt failed - install manually: https://dotnet.microsoft.com/download/dotnet/8.0"
  else
    echo "Install the .NET 8 runtime: https://dotnet.microsoft.com/download/dotnet/8.0"
  fi
fi

exec "$bin"
