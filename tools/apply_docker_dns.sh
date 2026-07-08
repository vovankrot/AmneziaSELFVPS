#!/bin/bash
set -e
if [ -f /etc/docker/daemon.json ]; then
    cp /etc/docker/daemon.json /etc/docker/daemon.json.bak
fi
install -m 644 /tmp/daemon.json /etc/docker/daemon.json
rm /tmp/daemon.json
echo "=== new config ==="
cat /etc/docker/daemon.json
echo "=== restart docker ==="
systemctl restart docker
sleep 3
systemctl is-active docker
echo "=== containers ==="
docker ps --format 'table {{.Names}}\t{{.Status}}'
