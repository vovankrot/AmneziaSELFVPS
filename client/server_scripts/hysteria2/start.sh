#!/bin/bash

# Hysteria 2 startup. Run inside the container as root.

echo "Container startup"

if command -v iptables >/dev/null 2>&1; then
    iptables -A INPUT -i lo -j ACCEPT
    iptables -A INPUT -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
    iptables -A INPUT -p icmp -j ACCEPT
    # Allow inbound UDP only on the Hysteria 2 port. The script is normally
    # rendered with $HYSTERIA_SERVER_PORT substituted before upload; if an old
    # static script is reused, fall back to parsing config.yaml.
    HYSTERIA_LISTEN_PORT="$HYSTERIA_SERVER_PORT"
    if [ -z "$HYSTERIA_LISTEN_PORT" ] && [ -f /opt/amnezia/hysteria2/config.yaml ]; then
        HYSTERIA_LISTEN_PORT="$(sed -n 's/^[[:space:]]*listen:[[:space:]]*:\([0-9][0-9]*\).*/\1/p' /opt/amnezia/hysteria2/config.yaml | head -n 1)"
    fi
    if [ -n "$HYSTERIA_LISTEN_PORT" ]; then
        iptables -A INPUT -p udp --dport "$HYSTERIA_LISTEN_PORT" -j ACCEPT
    fi
    iptables -P INPUT DROP
fi

if command -v ip6tables >/dev/null 2>&1; then
    ip6tables -A INPUT -i lo -j ACCEPT
    ip6tables -A INPUT -m state --state RELATED,ESTABLISHED -j ACCEPT
    ip6tables -A INPUT -p ipv6-icmp -j ACCEPT
    ip6tables -P INPUT DROP
fi

# kill any lingering daemon in case of restart
killall -KILL hysteria 2>/dev/null || true

if [ -f /opt/amnezia/hysteria2/config.yaml ]; then
    exec hysteria server --disable-update-check -c /opt/amnezia/hysteria2/config.yaml
fi

# No config yet (image just built, configurator not run) — idle so the
# install pipeline can scp files into us.
tail -f /dev/null
