#!/bin/bash
set -euo pipefail

cd /opt/amnezia/xray
SERVER_JSON_PATH="/opt/amnezia/xray/server.json"
SERVER_JSON_TMP=$(mktemp /opt/amnezia/xray/server.json.tmp.XXXXXX)
XRAY_VALIDATE_LOG="/tmp/amnezia_xray_configure_validate.log"

cleanup() {
    rm -f "$SERVER_JSON_TMP" "$XRAY_VALIDATE_LOG"
}

trap cleanup EXIT

# Own persist directory, separate from the sibling mKCP container's
# /opt/amnezia-xray-keys -- these are two independently installable containers and
# must not share (or clobber) each other's identity on reinstall. by vovankrot
XRAY_PERSIST_DIR="/opt/amnezia-xray-reality-keys"
mkdir -p "$XRAY_PERSIST_DIR"

if [ -s "$XRAY_PERSIST_DIR/xray_uuid.key" ]; then
    XRAY_CLIENT_ID=$(tr -d '[:space:]' < "$XRAY_PERSIST_DIR/xray_uuid.key")
else
    XRAY_CLIENT_ID=$(xray uuid)
    echo "$XRAY_CLIENT_ID" > "$XRAY_PERSIST_DIR/xray_uuid.key"
fi
echo "$XRAY_CLIENT_ID" > /opt/amnezia/xray/xray_uuid.key

# REALITY's identity: an x25519 keypair (private key stays server-side, public key
# ships to clients) plus a short id. Persisted the same way as the UUID above, so a
# container reinstall does not silently invalidate every existing client's config.
if [ -s "$XRAY_PERSIST_DIR/xray_private.key" ] && [ -s "$XRAY_PERSIST_DIR/xray_public.key" ]; then
    XRAY_PRIVATE_KEY=$(tr -d '[:space:]' < "$XRAY_PERSIST_DIR/xray_private.key")
    XRAY_PUBLIC_KEY=$(tr -d '[:space:]' < "$XRAY_PERSIST_DIR/xray_public.key")
else
    KEYPAIR=$(xray x25519)
    XRAY_PRIVATE_KEY=$(echo "$KEYPAIR" | sed -n '1p' | cut -d: -f2 | tr -d '[:space:]')
    XRAY_PUBLIC_KEY=$(echo "$KEYPAIR" | sed -n '2p' | cut -d: -f2 | tr -d '[:space:]')
    echo "$XRAY_PRIVATE_KEY" > "$XRAY_PERSIST_DIR/xray_private.key"
    echo "$XRAY_PUBLIC_KEY" > "$XRAY_PERSIST_DIR/xray_public.key"
fi
echo "$XRAY_PRIVATE_KEY" > /opt/amnezia/xray/xray_private.key
echo "$XRAY_PUBLIC_KEY" > /opt/amnezia/xray/xray_public.key

if [ -s "$XRAY_PERSIST_DIR/xray_short_id.key" ]; then
    XRAY_SHORT_ID=$(tr -d '[:space:]' < "$XRAY_PERSIST_DIR/xray_short_id.key")
else
    XRAY_SHORT_ID=$(openssl rand -hex 8)
    echo "$XRAY_SHORT_ID" > "$XRAY_PERSIST_DIR/xray_short_id.key"
fi
echo "$XRAY_SHORT_ID" > /opt/amnezia/xray/xray_short_id.key

cat > "$SERVER_JSON_TMP" <<EOF
{
    "log": {
        "access": "/opt/amnezia/xray/access.log",
        "loglevel": "warning"
    },
    "inbounds": [
        {
            "tag": "vless",
            "port": $XRAY_SERVER_PORT,
            "protocol": "vless",
            "settings": {
                "clients": [
                    {
                        "id": "$XRAY_CLIENT_ID",
                        "email": "$XRAY_CLIENT_ID",
                        "flow": "xtls-rprx-vision"
                    }
                ],
                "decryption": "none"
            },
            "streamSettings": {
                "network": "tcp",
                "security": "reality",
                "realitySettings": {
                    "dest": "$XRAY_SITE_NAME:443",
                    "serverNames": ["$XRAY_SITE_NAME"],
                    "privateKey": "$XRAY_PRIVATE_KEY",
                    "shortIds": ["$XRAY_SHORT_ID"]
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
    echo "ERROR: Generated XRay REALITY config failed validation" >&2
    cat "$XRAY_VALIDATE_LOG" >&2
    exit 1
fi

mv "$SERVER_JSON_TMP" "$SERVER_JSON_PATH"

if [ ! -f /opt/amnezia/xray/clientsTable ]; then
    printf '[]\n' > /opt/amnezia/xray/clientsTable
fi

trap - EXIT
cleanup
