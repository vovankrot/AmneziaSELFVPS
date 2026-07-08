#!/bin/bash
echo "=== docker status ==="
systemctl is-active docker
echo "=== daemon.json ==="
cat /etc/docker/daemon.json
echo "=== containers ==="
docker ps --format 'table {{.Names}}\t{{.Status}}\t{{.Ports}}'
echo "=== docker info DNS ==="
docker info 2>/dev/null | grep -i -E 'dns|name servers' || true
