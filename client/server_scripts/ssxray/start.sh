#!/bin/bash

# SSXray Shadowsocks container startup script
echo "Container startup - SSXray Shadowsocks"

# Apply sysctl settings if possible
sysctl -p /etc/sysctl.conf 2>/dev/null || true

# Setup iptables
SSXRAY_LISTEN_PORT="$SSXRAY_SERVER_PORT"

if command -v iptables >/dev/null 2>&1; then
    iptables -A INPUT -i lo -j ACCEPT || true
    iptables -A INPUT -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT || true
    iptables -A INPUT -p icmp -j ACCEPT || true
    if [ -n "$SSXRAY_LISTEN_PORT" ]; then
        iptables -A INPUT -p tcp --dport "$SSXRAY_LISTEN_PORT" -j ACCEPT || true
        iptables -A INPUT -p udp --dport "$SSXRAY_LISTEN_PORT" -j ACCEPT || true
    fi
    iptables -P INPUT DROP || true
fi

if command -v ip6tables >/dev/null 2>&1; then
    ip6tables -A INPUT -i lo -j ACCEPT || true
    ip6tables -A INPUT -m state --state RELATED,ESTABLISHED -j ACCEPT || true
    ip6tables -A INPUT -p ipv6-icmp -j ACCEPT || true
    if [ -n "$SSXRAY_LISTEN_PORT" ]; then
        ip6tables -A INPUT -p tcp --dport "$SSXRAY_LISTEN_PORT" -j ACCEPT || true
        ip6tables -A INPUT -p udp --dport "$SSXRAY_LISTEN_PORT" -j ACCEPT || true
    fi
    ip6tables -P INPUT DROP || true
fi

# Kill any existing xray process
pkill -9 xray 2>/dev/null || true
sleep 1

# Start xray with Shadowsocks config
if [ -f /opt/amnezia/ssxray/server.json ]; then
    echo "Starting XRay Shadowsocks server..."
    exec /usr/bin/xray run -config /opt/amnezia/ssxray/server.json
else
    echo "ERROR: Server config not found at /opt/amnezia/ssxray/server.json"
    tail -f /dev/null
fi
