#!/bin/bash
# Apply BBR + network sysctl tuning to a live VPS (operator helper).
# Idempotent. Same content as install_docker.sh emits during fresh container install.
set -e
sudo modprobe tcp_bbr 2>/dev/null || true
echo tcp_bbr | sudo tee /etc/modules-load.d/bbr.conf > /dev/null
printf '%s\n' \
  '# managed-by: amnezia-bootstrap' \
  'net.core.default_qdisc=fq' \
  'net.ipv4.tcp_congestion_control=bbr' \
  'net.core.rmem_max=67108864' \
  'net.core.wmem_max=67108864' \
  'net.ipv4.tcp_rmem=4096 87380 67108864' \
  'net.ipv4.tcp_wmem=4096 65536 67108864' \
  'net.ipv4.tcp_mtu_probing=1' \
  'net.ipv4.tcp_fastopen=3' \
  'net.ipv4.tcp_notsent_lowat=131072' \
  'net.core.netdev_max_backlog=250000' \
  'net.core.somaxconn=4096' \
  'net.ipv4.tcp_max_syn_backlog=8192' \
  | sudo tee /etc/sysctl.d/99-amnezia-perf.conf > /dev/null
sudo sysctl --system > /dev/null
echo "---verify---"
sysctl net.ipv4.tcp_congestion_control net.core.default_qdisc net.core.rmem_max net.ipv4.tcp_fastopen
lsmod | grep bbr || echo "(bbr not in lsmod yet, will load on next tcp connection)"
