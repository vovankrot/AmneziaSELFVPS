#!/bin/bash
# Run SSXray Shadowsocks container

sudo docker run -d \
    --privileged \
    --log-driver json-file --log-opt max-size=10m --log-opt max-file=1 \
    --restart always \
    --cap-add=NET_ADMIN \
    -p $SSXRAY_SERVER_PORT:$SSXRAY_SERVER_PORT/tcp \
    -p $SSXRAY_SERVER_PORT:$SSXRAY_SERVER_PORT/udp \
    --name $CONTAINER_NAME $CONTAINER_NAME

# Connect to DNS network if exists
sudo docker network connect amnezia-dns-net $CONTAINER_NAME 2>/dev/null || true

# Create tun device if not exist
sudo docker exec $CONTAINER_NAME bash -c 'mkdir -p /dev/net; if [ ! -c /dev/net/tun ]; then mknod /dev/net/tun c 10 200; fi'

echo "SSXray container started"
