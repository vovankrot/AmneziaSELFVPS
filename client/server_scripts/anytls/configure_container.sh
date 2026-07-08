#!/bin/bash
set -euo pipefail

# AnyTLS server configurator. anytls-server takes its password and listen
# address purely as CLI arguments — there is no config file. So this script
# just generates and persists a password key. The start.sh reads it back.

CONFIG_DIR="/opt/amnezia/anytls"
mkdir -p "$CONFIG_DIR"
cd "$CONFIG_DIR"

PASSWORD_PATH="$CONFIG_DIR/anytls_password.key"

# 32-byte hex password. AnyTLS uses this verbatim as the shared secret.
ANYTLS_PASSWORD="$(openssl rand -hex 16)"
echo "$ANYTLS_PASSWORD" > "$PASSWORD_PATH"
chmod 600 "$PASSWORD_PATH"

# Sanity check: ensure the binary is actually present.
if ! command -v anytls-server >/dev/null 2>&1; then
    echo "ERROR: anytls-server not found in container" >&2
    exit 1
fi
