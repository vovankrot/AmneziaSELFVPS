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
# Persist the client UUID + salamander password across container reinstalls via a host
# volume (mounted by run_container.sh at /opt/amnezia-xray-keys). A reinstall recreates
# the container from scratch and would otherwise regenerate BOTH -- wiping the shared
# client + obfs key and breaking every other device. Reuse if present; generate +
# persist on first install. All devices share this ONE UUID (the configurator reads it
# from the server instead of minting its own), so reinstalls never break other devices.
# by vovankrot
XRAY_PERSIST_DIR="/opt/amnezia-xray-keys"
mkdir -p "$XRAY_PERSIST_DIR"

if [ -s "$XRAY_PERSIST_DIR/xray_uuid.key" ]; then
    XRAY_CLIENT_ID=$(tr -d '[:space:]' < "$XRAY_PERSIST_DIR/xray_uuid.key")
else
    XRAY_CLIENT_ID=$(xray uuid)
    echo "$XRAY_CLIENT_ID" > "$XRAY_PERSIST_DIR/xray_uuid.key"
fi
echo "$XRAY_CLIENT_ID" > /opt/amnezia/xray/xray_uuid.key

# salamander = the SAME packet masking that keeps Hysteria alive on this RKN-flagged IP:
# it XORs every packet into pseudo-random noise so RKN/TSPU cannot fingerprint it (unlike
# reality, whose TLS handshake RKN clamps on a flagged IP on ANY port). Client and server
# must share this password (client reads xray_salamander.key).
if [ -s "$XRAY_PERSIST_DIR/xray_salamander.key" ]; then
    XRAY_SALAMANDER_PASSWORD=$(tr -d '[:space:]' < "$XRAY_PERSIST_DIR/xray_salamander.key")
else
    XRAY_SALAMANDER_PASSWORD=$(openssl rand -hex 16)
    echo "$XRAY_SALAMANDER_PASSWORD" > "$XRAY_PERSIST_DIR/xray_salamander.key"
fi
echo "$XRAY_SALAMANDER_PASSWORD" > /opt/amnezia/xray/xray_salamander.key

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
                        "flow": ""
                    }
                ],
                "decryption": "none"
            },
            "streamSettings": {
                "network": "kcp",
                "kcpSettings": {
                    "mtu": 1350,
                    "tti": 50,
                    "uplinkCapacity": 100,
                    "downlinkCapacity": 100,
                    "congestion": true,
                    "readBufferSize": 2,
                    "writeBufferSize": 2
                },
                "finalmask": {
                    "udp": [
                        {
                            "type": "salamander",
                            "settings": {
                                "password": "$XRAY_SALAMANDER_PASSWORD",
                                "packetSize": "512-1200"
                            }
                        }
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
    echo "ERROR: Generated XRay config failed validation" >&2
    cat "$XRAY_VALIDATE_LOG" >&2
    exit 1
fi

mv "$SERVER_JSON_TMP" "$SERVER_JSON_PATH"

if [ ! -f /opt/amnezia/xray/clientsTable ]; then
    printf '[]\n' > /opt/amnezia/xray/clientsTable
fi

trap - EXIT
cleanup
