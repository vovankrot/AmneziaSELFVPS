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
if [ -s "$PASSWORD_PATH" ]; then
    ANYTLS_PASSWORD="$(cat "$PASSWORD_PATH")"
else
    ANYTLS_PASSWORD="$(openssl rand -hex 16)"
fi
echo "$ANYTLS_PASSWORD" > "$PASSWORD_PATH"
chmod 600 "$PASSWORD_PATH"

# Persist a server identity across restarts. The client obtains its pin over verified SSH.
if [ ! -s "$CONFIG_DIR/server.crt" ] || [ ! -s "$CONFIG_DIR/server.key" ]; then
    openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 \
        -subj '/CN=AnyTLS' -keyout "$CONFIG_DIR/server.key" -out "$CONFIG_DIR/server.crt"
fi
chmod 600 "$CONFIG_DIR/server.key" "$CONFIG_DIR/server.crt"

# Sanity check: ensure the binary is actually present.
if ! command -v anytls-server >/dev/null 2>&1; then
    echo "ERROR: anytls-server not found in container" >&2
    exit 1
fi
