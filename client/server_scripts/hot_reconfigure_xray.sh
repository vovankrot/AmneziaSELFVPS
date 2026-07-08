#!/bin/bash
set -euo pipefail

# Hot-reconfigure XRay: rebuild server.json using EXISTING keys with new transport settings.
# Does NOT regenerate keys — preserves all client connections.
# Optional client hints: $XRAY_SERVER_PORT, $XRAY_SITE_NAME.
# If hints are empty/stale, reuse values from the current server.json for better portability.

CLIENT_PORT_HINT='$XRAY_SERVER_PORT'
CLIENT_SITE_HINT='$XRAY_SITE_NAME'

derive_public_key_from_private() {
    local private_key="$1"

    [ -n "$private_key" ] || return 1

    xray x25519 -i "$private_key" 2>/dev/null | sed -n \
        -e 's/^Password (PublicKey):[[:space:]]*//p' \
        -e 's/^PublicKey:[[:space:]]*//p' \
        -e 's/^Public key:[[:space:]]*//p' | head -n 1 | tr -d '[:space:]'
}

cd /opt/amnezia/xray
SERVER_JSON_PATH="/opt/amnezia/xray/server.json"
SERVER_JSON_TMP_BASE=$(mktemp /opt/amnezia/xray/server.json.tmp.XXXXXX)
SERVER_JSON_TMP="${SERVER_JSON_TMP_BASE}.json"
mv "$SERVER_JSON_TMP_BASE" "$SERVER_JSON_TMP"
SERVER_JSON_BACKUP=""
XRAY_START_LOG="/tmp/amnezia_xray_hot_reconfigure.log"
XRAY_VALIDATE_LOG="/tmp/amnezia_xray_hot_reconfigure_validate.log"

cleanup() {
    if [ -n "$SERVER_JSON_TMP" ]; then
        rm -f "$SERVER_JSON_TMP"
    fi
    rm -f "$XRAY_START_LOG" "$XRAY_VALIDATE_LOG"
    if [ -n "$SERVER_JSON_BACKUP" ]; then
        rm -f "$SERVER_JSON_BACKUP"
    fi
}

trap cleanup EXIT

if [ -f "$SERVER_JSON_PATH" ]; then
    SERVER_JSON_BACKUP=$(mktemp /opt/amnezia/xray/server.json.bak.XXXXXX)
    cp "$SERVER_JSON_PATH" "$SERVER_JSON_BACKUP"
fi

# Read existing keys — abort if missing (server not properly configured)
XRAY_PRIVATE_KEY=$(cat /opt/amnezia/xray/xray_private.key 2>/dev/null | tr -d '[:space:]')
XRAY_PUBLIC_KEY=$(derive_public_key_from_private "$XRAY_PRIVATE_KEY")
if [ -z "$XRAY_PUBLIC_KEY" ]; then
    XRAY_PUBLIC_KEY=$(cat /opt/amnezia/xray/xray_public.key 2>/dev/null | tr -d '[:space:]')
fi
XRAY_SHORT_ID=$(cat /opt/amnezia/xray/xray_short_id.key 2>/dev/null | tr -d '[:space:]')

if [ -z "$XRAY_PRIVATE_KEY" ] || [ -z "$XRAY_PUBLIC_KEY" ] || [ -z "$XRAY_SHORT_ID" ]; then
    echo "ERROR: Missing XRay keys. Run full configure first." >&2
    exit 1
fi

printf '%s\n' "$XRAY_PUBLIC_KEY" > /opt/amnezia/xray/xray_public.key

extract_existing_clients_json() {
    local json_path="$SERVER_JSON_PATH"

    [ -f "$json_path" ] || return 1

    awk '
        BEGIN {
            capture = 0
            depth = 0
        }
        {
            line = $0

            if (!capture) {
                if (line !~ /"clients"[[:space:]]*:[[:space:]]*\[/) {
                    next
                }

                sub(/^.*"clients"[[:space:]]*:[[:space:]]*/, "", line)
                capture = 1
            }

            print line

            tmp = line
            open_count = gsub(/\[/, "[", tmp)
            tmp = line
            close_count = gsub(/\]/, "]", tmp)
            depth += open_count - close_count

            if (depth <= 0) {
                exit
            }
        }
    ' "$json_path" | sed '$s/,[[:space:]]*$//'
}

is_valid_port() {
    local value="$1"
    case "$value" in
        ''|*[!0-9]*)
            return 1
            ;;
    esac

    [ "$value" -ge 1 ] && [ "$value" -le 65535 ]
}

is_valid_domain() {
    local value="$1"
    [ -n "$value" ] || return 1
    [ "${#value}" -le 253 ] || return 1
    printf '%s' "$value" | grep -qE '^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?(\.[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*$'
}

extract_current_port() {
    local json_path="$SERVER_JSON_PATH"

    [ -f "$json_path" ] || return 1

    sed -n 's/.*"port"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$json_path" | head -n 1
}

extract_current_site() {
    local json_path="$SERVER_JSON_PATH"
    local site=""
    local flattened_json=""

    [ -f "$json_path" ] || return 1

    site=$(sed -n 's/.*"dest"[[:space:]]*:[[:space:]]*"\([^":][^"]*\):[0-9][0-9]*".*/\1/p' "$json_path" | head -n 1)
    if [ -n "$site" ]; then
        printf '%s' "$site"
        return 0
    fi

    flattened_json=$(tr '\n' ' ' < "$json_path")
    site=$(printf '%s' "$flattened_json" | sed -n 's/.*"serverNames"[[:space:]]*:[[:space:]]*\[[[:space:]]*"\([^"]*\)".*/\1/p' | head -n 1)
    if [ -n "$site" ]; then
        printf '%s' "$site"
        return 0
    fi

    return 1
}

resolve_server_port() {
    local resolved_port="$CLIENT_PORT_HINT"

    if is_valid_port "$resolved_port"; then
        printf '%s' "$resolved_port"
        return 0
    fi

    resolved_port=$(extract_current_port || true)
    if is_valid_port "$resolved_port"; then
        printf '%s' "$resolved_port"
        return 0
    fi

    printf '%s' '443'
}

resolve_server_site() {
    local resolved_site="$CLIENT_SITE_HINT"

    if is_valid_domain "$resolved_site"; then
        printf '%s' "$resolved_site"
        return 0
    fi

    resolved_site=$(extract_current_site || true)
    if is_valid_domain "$resolved_site"; then
        printf '%s' "$resolved_site"
        return 0
    fi

    printf '%s' 'www.microsoft.com'
}

# Read existing clients from server.json to preserve multi-user setup.
# Avoid python/jq dependencies because already deployed XRay containers may not have them.
EXISTING_CLIENTS_JSON=""
if [ -f "$SERVER_JSON_PATH" ]; then
    EXISTING_CLIENTS_JSON=$(extract_existing_clients_json || true)
fi

CLIENTS_CHECK=$(printf '%s' "$EXISTING_CLIENTS_JSON" | tr -d '[:space:]')
if [ -z "$CLIENTS_CHECK" ] || [ "$CLIENTS_CHECK" = "[]" ]; then
    # Fallback: use uuid.key
    XRAY_CLIENT_ID=$(cat /opt/amnezia/xray/xray_uuid.key 2>/dev/null | tr -d '[:space:]')
    if [ -z "$XRAY_CLIENT_ID" ]; then
        XRAY_CLIENT_ID=$(xray uuid)
        echo "$XRAY_CLIENT_ID" > /opt/amnezia/xray/xray_uuid.key
    fi
    # Validate UUID format to prevent injection
    if ! echo "$XRAY_CLIENT_ID" | grep -qE '^[0-9a-fA-F-]+$'; then
        echo "ERROR: Invalid UUID format in xray_uuid.key" >&2
        exit 1
    fi
    EXISTING_CLIENTS_JSON=$(cat <<EOF
[
                    {
                        "id": "$XRAY_CLIENT_ID",
                        "flow": ""
                    }
                ]
EOF
)
fi

# Generate XHTTP path if not present
XHTTP_PATH=$(cat /opt/amnezia/xray/xray_xhttp_path.key 2>/dev/null | tr -d '[:space:]')
if [ -z "$XHTTP_PATH" ]; then
    XHTTP_PATH="/api/v1/$(openssl rand -hex 4)"
    echo "$XHTTP_PATH" > /opt/amnezia/xray/xray_xhttp_path.key
fi

# Validate XHTTP path — must be URL-safe path
if ! echo "$XHTTP_PATH" | grep -qE '^/[a-zA-Z0-9/_-]+$'; then
    echo "ERROR: Invalid XHTTP path format" >&2
    exit 1
fi

# Validate keys — must be base64/hex only
for keyfile in xray_private.key xray_public.key xray_short_id.key; do
    val=$(cat "/opt/amnezia/xray/$keyfile" 2>/dev/null | tr -d '[:space:]')
    if ! echo "$val" | grep -qE '^[a-zA-Z0-9+/=_-]+$'; then
        echo "ERROR: Invalid key format in $keyfile" >&2
        exit 1
    fi
done

RESOLVED_SERVER_PORT=$(resolve_server_port)
RESOLVED_SITE_NAME=$(resolve_server_site)

if ! is_valid_port "$RESOLVED_SERVER_PORT"; then
    echo "ERROR: Unable to determine XRay port for hot reconfigure" >&2
    exit 1
fi

if ! is_valid_domain "$RESOLVED_SITE_NAME"; then
    echo "ERROR: Unable to determine XRay site for hot reconfigure" >&2
    exit 1
fi

# Build new server.json with XHTTP transport
cat > "$SERVER_JSON_TMP" <<EOF
{
    "log": {
        "loglevel": "error"
    },
    "inbounds": [
        {
            "port": $RESOLVED_SERVER_PORT,
            "protocol": "vless",
            "settings": {
                "clients": $EXISTING_CLIENTS_JSON,
                "decryption": "none"
            },
            "streamSettings": {
                "network": "xhttp",
                "xhttpSettings": {
                    "path": "$XHTTP_PATH",
                    "mode": "auto"
                },
                "security": "reality",
                "realitySettings": {
                    "dest": "$RESOLVED_SITE_NAME:443",
                    "serverNames": [
                        "$RESOLVED_SITE_NAME"
                    ],
                    "privateKey": "$XRAY_PRIVATE_KEY",
                    "shortIds": [
                        "$XRAY_SHORT_ID"
                    ]
                }
            }
        }
    ],
    "outbounds": [
        {
            "protocol": "freedom",
            "tag": "direct"
        },
        {
            "protocol": "blackhole",
            "tag": "block"
        }
    ],
    "routing": {
        "domainStrategy": "IPIfNonMatch",
        "rules": [
            {
                "type": "field",
                "inboundTag": ["vless"],
                "outboundTag": "direct"
            }
        ]
    }
}
EOF

if ! xray run -test -format json -config "$SERVER_JSON_TMP" >"$XRAY_VALIDATE_LOG" 2>&1; then
    echo "ERROR: Generated XRay config failed validation." >&2
    cat "$XRAY_VALIDATE_LOG" >&2
    exit 1
fi

if [ ! -f /opt/amnezia/xray/clientsTable ]; then
    printf '[]\n' > /opt/amnezia/xray/clientsTable
fi

# Restart XRay process
is_xray_running() {
    if command -v pidof >/dev/null 2>&1; then
        pidof xray >/dev/null 2>&1 && return 0
    fi

    if command -v pgrep >/dev/null 2>&1; then
        pgrep -x xray >/dev/null 2>&1 && return 0
    fi

    ps 2>/dev/null | grep -q '[x]ray'
}

rm -f "$XRAY_START_LOG"

killall -KILL xray 2>/dev/null || true
mv "$SERVER_JSON_TMP" "$SERVER_JSON_PATH"
SERVER_JSON_TMP=""
nohup xray -config "$SERVER_JSON_PATH" >"$XRAY_START_LOG" 2>&1 &

XRAY_STARTED=0
for _ in 1 2 3; do
    if is_xray_running; then
        XRAY_STARTED=1
        break
    fi
    sleep 1
done

if [ "$XRAY_STARTED" -ne 1 ]; then
    echo "ERROR: XRay failed to start with updated config." >&2
    if [ -s "$XRAY_START_LOG" ]; then
        cat "$XRAY_START_LOG" >&2
    fi

    if [ -n "$SERVER_JSON_BACKUP" ] && [ -f "$SERVER_JSON_BACKUP" ]; then
        cp "$SERVER_JSON_BACKUP" "$SERVER_JSON_PATH"
        nohup xray -config "$SERVER_JSON_PATH" >"$XRAY_START_LOG" 2>&1 &
    fi

    exit 1
fi

rm -f "$XRAY_START_LOG"
if [ -n "$SERVER_JSON_BACKUP" ]; then
    rm -f "$SERVER_JSON_BACKUP"
    SERVER_JSON_BACKUP=""
fi

trap - EXIT
cleanup

echo "HOT_RECONFIGURE_VALUES port=$RESOLVED_SERVER_PORT site=$RESOLVED_SITE_NAME path=$XHTTP_PATH"
echo "HOT_RECONFIGURE_OK"
