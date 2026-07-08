SUDO='echo 9091900 | sudo -S -p ""'
echo '=== CONTAINERS ==='
eval $SUDO docker ps --format "'{{.Names}} {{.Status}}'" 2>/dev/null
echo '=== XRAY LOG TAIL ==='
eval $SUDO docker logs --tail 50 amnezia-xray 2>&1 | tail -50
echo '=== SSXRAY LOG TAIL ==='
eval $SUDO docker logs --tail 30 amnezia-ssxray 2>&1 | tail -30
echo '=== XRAY SERVER.JSON ==='
eval $SUDO docker exec amnezia-xray cat /opt/amnezia/xray/server.json 2>/dev/null
echo '=== XRAY PROCESS ==='
eval $SUDO docker exec amnezia-xray pgrep -af xray 2>/dev/null
echo '=== ESTABLISHED 443 ==='
eval $SUDO ss -tnp state established 2>/dev/null | grep ':443' | head -10
echo '=== CONTAINER DNS TEST ==='
eval $SUDO docker exec amnezia-xray getent hosts www.microsoft.com 2>&1 | head -3
echo '=== DOCKER DAEMON.JSON ==='
eval $SUDO cat /etc/docker/daemon.json 2>/dev/null
echo '=== HOST SYSCTL ==='
sysctl -n net.ipv4.tcp_congestion_control net.core.default_qdisc 2>&1
echo '=== JOURNAL DOCKER ERRS (1h) ==='
eval $SUDO journalctl -u docker --since '1 hour ago' 2>/dev/null | grep -iE 'error|fail|timeout' | tail -20
echo '=== AMNEZIA-SYSCTL CONF ==='
eval $SUDO cat /etc/sysctl.d/99-amnezia-perf.conf 2>/dev/null | head -20
echo '=== LSMOD bbr ==='
lsmod | grep -E 'bbr|hybla' 2>/dev/null
