#!/bin/bash

# This scripts copied from Amnezia client to Docker container to /opt/amnezia and launched every time container starts

echo "Container startup"
#ifconfig eth0:0 $SERVER_IP_ADDRESS netmask 255.255.255.255 up

if command -v iptables >/dev/null 2>&1; then
    iptables -A INPUT -i lo -j ACCEPT
    iptables -A INPUT -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
    iptables -A INPUT -p icmp -j ACCEPT
    iptables -A INPUT -p tcp --dport 80 -j ACCEPT
    iptables -A INPUT -p udp --dport "${XRAY_SERVER_PORT:-443}" -j ACCEPT
    iptables -P INPUT DROP
fi

if command -v ip6tables >/dev/null 2>&1; then
    ip6tables -A INPUT -i lo -j ACCEPT
    ip6tables -A INPUT -m state --state RELATED,ESTABLISHED -j ACCEPT
    ip6tables -A INPUT -p ipv6-icmp -j ACCEPT
    ip6tables -P INPUT DROP
fi

# kill daemons in case of restart
killall -KILL xray

# start daemons if configured
if [ -f /opt/amnezia/xray/server.json ]; then
    exec xray -config /opt/amnezia/xray/server.json
fi

tail -f /dev/null
