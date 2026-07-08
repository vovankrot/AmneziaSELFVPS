#!/bin/bash
# vps_bootstrap.sh — universal hardening bootstrap for AmneziaVPN VPS host.
# Idempotent. Run once on a fresh Linux VPS BEFORE / AFTER the AmneziaVPN client
# pushes its containers; safe to re-run.
#
# What it does (all conditional, only if not already correct):
#   1. Ensures /etc/docker/daemon.json has DNS 1.1.1.1/9.9.9.9 (avoids dockerd
#      timeouts on 8.8.8.8 seen on some hosters).
#   2. Installs fail2ban with our jail.local (ssh bruteforce protection,
#      progressive bantime up to 1 week, systemd backend).
#
# Supports: debian/ubuntu (apt-get), fedora (dnf), rhel/centos (yum),
#           opensuse (zypper), arch (pacman).
#
# Usage on the VPS as a sudoer:
#   sudo bash vps_bootstrap.sh
#
# Or remotely from the workstation:
#   scp tools/vps_bootstrap.sh user@host:/tmp/
#   ssh user@host 'sudo bash /tmp/vps_bootstrap.sh && rm /tmp/vps_bootstrap.sh'

set -e

if [ "$EUID" -ne 0 ]; then
    echo "ERROR: must run as root (sudo bash $0)" >&2
    exit 1
fi

# ---- 1. detect package manager -------------------------------------------------
if   command -v apt-get >/dev/null 2>&1; then PM=apt-get;  INSTALL="-yq install";        UPDATE="-yq update";        DIST=debian
elif command -v dnf     >/dev/null 2>&1; then PM=dnf;      INSTALL="-yq install";        UPDATE="-yq check-update";  DIST=fedora
elif command -v yum     >/dev/null 2>&1; then PM=yum;      INSTALL="-y -q install";      UPDATE="-y -q check-update";DIST=centos
elif command -v zypper  >/dev/null 2>&1; then PM=zypper;   INSTALL="-nq install";        UPDATE="-nq refresh";       DIST=opensuse
elif command -v pacman  >/dev/null 2>&1; then PM=pacman;   INSTALL="-S --noconfirm --noprogressbar --quiet"; UPDATE="-Sy"; DIST=arch
else
    echo "ERROR: no supported package manager found" >&2
    exit 1
fi
[ "$DIST" = "debian" ] && export DEBIAN_FRONTEND=noninteractive
echo "[bootstrap] dist=$DIST pm=$PM"

# yum check-update returns 100 when updates are available — that is fine.
run_update() { $PM $UPDATE >/dev/null 2>&1 || true; }

# ---- 2. docker DNS -------------------------------------------------------------
DOCKER_JSON=/etc/docker/daemon.json
need_docker_restart=0
if command -v docker >/dev/null 2>&1; then
    mkdir -p /etc/docker
    if [ ! -f "$DOCKER_JSON" ]; then
        echo "[bootstrap] writing $DOCKER_JSON (DNS 1.1.1.1, 9.9.9.9)"
        cat >"$DOCKER_JSON" <<'EOF'
{
    "dns": ["1.1.1.1", "9.9.9.9"]
}
EOF
        need_docker_restart=1
    elif ! grep -q '1.1.1.1' "$DOCKER_JSON"; then
        # daemon.json exists but no 1.1.1.1 — back up and replace.
        # Refuses to merge: we don't want to risk breaking existing keys silently.
        echo "[bootstrap] WARNING: $DOCKER_JSON exists without 1.1.1.1; leaving untouched."
        echo "[bootstrap]          Review manually and add: \"dns\": [\"1.1.1.1\", \"9.9.9.9\"]"
    else
        echo "[bootstrap] $DOCKER_JSON already has 1.1.1.1, skipping"
    fi
    if [ "$need_docker_restart" = "1" ] && systemctl is-active docker >/dev/null 2>&1; then
        echo "[bootstrap] restarting docker (containers with restart policy will come back)"
        systemctl restart docker
        sleep 3
    fi
else
    # Docker not installed yet — AmneziaVPN client will install it via
    # install_docker.sh which now writes daemon.json before first start.
    echo "[bootstrap] docker not installed; daemon.json will be created by install_docker.sh"
fi

# ---- 3. fail2ban ---------------------------------------------------------------
if ! command -v fail2ban-client >/dev/null 2>&1; then
    echo "[bootstrap] installing fail2ban"
    run_update
    $PM $INSTALL fail2ban
fi

JAIL=/etc/fail2ban/jail.local
if [ ! -f "$JAIL" ] || ! grep -q "amnezia-bootstrap" "$JAIL"; then
    echo "[bootstrap] writing $JAIL"
    cat >"$JAIL" <<'EOF'
# managed-by: amnezia-bootstrap
[DEFAULT]
ignoreip = 127.0.0.1/8 ::1 10.0.0.0/8 172.16.0.0/12 192.168.0.0/16
findtime = 10m
bantime = 1h
bantime.increment = true
bantime.factor = 2
bantime.maxtime = 1w
maxretry = 5
backend = systemd

[sshd]
enabled = true
port = ssh
filter = sshd
maxretry = 5
findtime = 10m
bantime = 1h
EOF
    chmod 644 "$JAIL"
    systemctl enable --now fail2ban >/dev/null 2>&1 || true
    systemctl restart fail2ban
    sleep 2
else
    echo "[bootstrap] $JAIL already present (managed-by: amnezia-bootstrap), keeping"
    systemctl enable --now fail2ban >/dev/null 2>&1 || true
fi

# ---- 4. summary ----------------------------------------------------------------
echo "=== bootstrap summary ==="
echo "-- docker --"
if command -v docker >/dev/null 2>&1; then
    systemctl is-active docker || true
    [ -f "$DOCKER_JSON" ] && cat "$DOCKER_JSON"
else
    echo "(not installed)"
fi
echo "-- fail2ban --"
fail2ban-client --version 2>/dev/null || echo "(not installed)"
fail2ban-client status sshd 2>/dev/null || true
echo "=== done ==="
