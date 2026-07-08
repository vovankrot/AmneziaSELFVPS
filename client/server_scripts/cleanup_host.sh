#!/bin/bash
# cleanup_host.sh — Reverts host-level changes made by Amnezia VPN
# Idempotent: safe to run multiple times
# REMOVE_DOCKER is set to 1 by the client when Docker removal is requested

REMOVE_DOCKER=${REMOVE_DOCKER:-0}

echo "=== Amnezia host cleanup ==="

# 1. Remove iptables rules added by setup_host_firewall.sh
echo "[1/5] Removing iptables rules..."
sudo iptables -D INPUT -p icmp --icmp-type echo-request -j DROP 2>/dev/null && echo "  Removed ICMP drop rule" || echo "  ICMP drop rule not found, skipping"

# Docker-related FORWARD rules (only if Docker is being removed)
if [ "$REMOVE_DOCKER" -eq 1 ]; then
    sudo iptables -D FORWARD -j DOCKER-USER 2>/dev/null
    sudo iptables -D FORWARD -j DOCKER-ISOLATION-STAGE-1 2>/dev/null
    sudo iptables -D FORWARD -o docker0 -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT 2>/dev/null
    sudo iptables -D FORWARD -o docker0 -j DOCKER 2>/dev/null
    sudo iptables -D FORWARD -i docker0 ! -o docker0 -j ACCEPT 2>/dev/null
    sudo iptables -D FORWARD -i docker0 -o docker0 -j ACCEPT 2>/dev/null
    echo "  Removed Docker FORWARD rules"
fi

# 2. Restore sysctl defaults
echo "[2/5] Restoring sysctl defaults..."
sudo sysctl -w net.ipv4.ip_forward=0 2>/dev/null
sudo sysctl -w fs.file-max=65535 2>/dev/null
sudo sysctl -w net.core.rmem_max=212992 2>/dev/null
sudo sysctl -w net.core.wmem_max=212992 2>/dev/null
sudo sysctl -w net.core.netdev_max_backlog=1000 2>/dev/null
sudo sysctl -w net.core.somaxconn=4096 2>/dev/null
sudo sysctl -w net.ipv4.tcp_syncookies=1 2>/dev/null
sudo sysctl -w net.ipv4.tcp_tw_reuse=2 2>/dev/null
sudo sysctl -w net.ipv4.tcp_fin_timeout=60 2>/dev/null
sudo sysctl -w net.ipv4.tcp_keepalive_time=7200 2>/dev/null
sudo sysctl -w net.ipv4.ip_local_port_range="32768 60999" 2>/dev/null
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=256 2>/dev/null
sudo sysctl -w net.ipv4.tcp_max_tw_buckets=180000 2>/dev/null
sudo sysctl -w net.ipv4.tcp_fastopen=1 2>/dev/null
sudo sysctl -w net.ipv4.tcp_mtu_probing=0 2>/dev/null
sudo sysctl -w net.ipv4.tcp_congestion_control=cubic 2>/dev/null
echo "  sysctl restored to defaults"

# Remove persistent sysctl overrides if any
sudo rm -f /etc/sysctl.d/99-amnezia.conf 2>/dev/null

# 3. Remove Amnezia data directory
echo "[3/5] Removing /opt/amnezia..."
sudo rm -rf /opt/amnezia
echo "  Done"

# 4. Remove Docker (optional)
if [ "$REMOVE_DOCKER" -eq 1 ]; then
    echo "[4/5] Removing Docker..."
    sudo systemctl stop docker.socket docker.service 2>/dev/null
    sudo systemctl disable docker.socket docker.service 2>/dev/null
    # Detect package manager
    if command -v apt-get &>/dev/null; then
        sudo apt-get purge -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin 2>/dev/null
        sudo apt-get autoremove -y 2>/dev/null
    elif command -v dnf &>/dev/null; then
        sudo dnf remove -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin 2>/dev/null
    elif command -v yum &>/dev/null; then
        sudo yum remove -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin 2>/dev/null
    fi
    sudo rm -rf /var/lib/docker /var/lib/containerd
    sudo rm -f /etc/docker/daemon.json
    # Remove docker group (user stays)
    sudo groupdel docker 2>/dev/null
    echo "  Docker removed"
else
    echo "[4/5] Docker removal skipped (use --remove-docker to remove)"
fi

# 5. Clean up cron jobs
echo "[5/5] Removing amnezia cron jobs..."
sudo crontab -l 2>/dev/null | grep -v amnezia | sudo crontab - 2>/dev/null
echo "  Done"

echo "=== Cleanup complete ==="
