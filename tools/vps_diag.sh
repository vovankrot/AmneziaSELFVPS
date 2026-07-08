#!/bin/bash
PW='9091900'
SUDO() { echo "$PW" | sudo -S -p '' "$@" 2>&1; }
echo '=== docker ps ==='
SUDO docker ps
echo '=== docker logs amnezia-xray (last 120) ==='
SUDO docker logs --tail 120 amnezia-xray
echo '=== xray test config ==='
SUDO docker exec amnezia-xray xray run -test -config /opt/amnezia/xray/server.json | tail -30
echo '=== amnezia-xray inspect (Restart count) ==='
SUDO docker inspect amnezia-xray --format '{{.RestartCount}} restarts; started {{.State.StartedAt}}; status {{.State.Status}}'
echo '=== journalctl ERR 48h ==='
SUDO journalctl --since '48 hours ago' -p err --no-pager | tail -80
echo '=== journalctl docker.service 48h ==='
SUDO journalctl -u docker.service --since '48 hours ago' --no-pager | tail -40
echo '=== nginx -t ==='
SUDO nginx -t
echo '=== iptables FORWARD ==='
SUDO iptables -L FORWARD -n --line-numbers | head -25
echo '=== ss listen ==='
SUDO ss -tlnp | head -30
echo '=== /opt/amnezia/xray listing ==='
SUDO ls -la /opt/amnezia/xray 2>/dev/null
echo '=== last 5 reboots ==='
last -x reboot | head -5
echo '=== conntrack count ==='
SUDO sysctl -n net.netfilter.nf_conntrack_count 2>/dev/null
SUDO sysctl -n net.netfilter.nf_conntrack_max 2>/dev/null
echo '=== xray container env ports ==='
SUDO docker inspect amnezia-xray --format '{{json .NetworkSettings.Ports}}'
echo '=== sshd config (key bits) ==='
SUDO grep -E '^(PasswordAuthentication|PermitRootLogin|PubkeyAuthentication)' /etc/ssh/sshd_config
echo '=== fail2ban ==='
SUDO fail2ban-client status 2>&1 | head -10
echo '=== DONE ==='
