# Run container
# --privileged mirrors xray/ssxray/hysteria2 and lets start.sh manage
# iptables consistently on newer Docker/Ubuntu hosts.
test -n "$ANYTLS_SERVER_PORT" || { echo "ERROR: ANYTLS_SERVER_PORT is empty" >&2; exit 1; }
test -n "$CONTAINER_NAME" || { echo "ERROR: CONTAINER_NAME is empty" >&2; exit 1; }

sudo docker run -d \
--privileged \
--log-driver json-file --log-opt max-size=10m --log-opt max-file=1 \
--restart always \
--cap-add=NET_ADMIN \
-e ANYTLS_SERVER_PORT="$ANYTLS_SERVER_PORT" \
-p "$ANYTLS_SERVER_PORT:$ANYTLS_SERVER_PORT/tcp" \
--name "$CONTAINER_NAME" "$CONTAINER_NAME"

sudo docker network connect amnezia-dns-net $CONTAINER_NAME 2>/dev/null || true
