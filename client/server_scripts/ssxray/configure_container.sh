#!/bin/bash
set -e

cd /opt/amnezia/ssxray

# Generate Shadowsocks 2022 password (32 bytes for aes-256-gcm, base64 encoded)
SS_PASSWORD=$(openssl rand -base64 32)
echo "$SS_PASSWORD" > /opt/amnezia/ssxray/ss_password.key

# Create server configuration
cat > /opt/amnezia/ssxray/server.json <<EOF
{
    "log": {
        "loglevel": "warning"
    },
    "inbounds": [
        {
            "port": $SSXRAY_SERVER_PORT,
            "protocol": "shadowsocks",
            "settings": {
                "method": "$SSXRAY_CIPHER",
                "password": "$SS_PASSWORD",
                "network": "tcp,udp"
            },
            "sniffing": {
                "enabled": true,
                "destOverride": ["http", "tls"]
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
        "rules": [
            {
                "type": "field",
                "outboundTag": "block",
                "ip": ["geoip:private"]
            }
        ]
    }
}
EOF

echo "SSXray Shadowsocks server configured on port $SSXRAY_SERVER_PORT"
