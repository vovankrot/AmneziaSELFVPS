# Self-heal: remove any stale/zombie container of the same name before (re)creating
# it, so a previous failed install can't block this run with a name conflict.
sudo docker rm -f $CONTAINER_NAME >/dev/null 2>&1 || true

# REALITY is a genuine TLS handshake, so it rides on TCP (unlike the sibling mKCP
# container, which is UDP).
sudo docker run -d \
--privileged \
--log-driver json-file --log-opt max-size=10m --log-opt max-file=1 \
--restart always \
--cap-add=NET_ADMIN \
-e XRAY_SERVER_PORT="$XRAY_SERVER_PORT" \
-p $XRAY_SERVER_PORT:$XRAY_SERVER_PORT/tcp \
-v /opt/amnezia-xray-reality-keys:/opt/amnezia-xray-reality-keys \
--name $CONTAINER_NAME $CONTAINER_NAME

sudo docker network connect amnezia-dns-net $CONTAINER_NAME 2>/dev/null || true

# Create tun device if not exist
sudo docker exec $CONTAINER_NAME bash -c 'mkdir -p /dev/net; if [ ! -c /dev/net/tun ]; then mknod /dev/net/tun c 10 200; fi'
