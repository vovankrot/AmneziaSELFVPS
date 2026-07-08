# Run container
# NOTE: --sysctl net.core.rmem_max/wmem_max removed — dockerd on Ubuntu 24.04
# rejects them as non-namespaced sysctls and aborts the run, which bash
# surfaces as exit code 127. The same buffers are tuned host-side by
# tools/apply_perf_tuning.sh, which is what actually matters for UDP throughput.
# --privileged mirrors the xray container and lets start.sh manage iptables.
sudo docker run -d \
--privileged \
--log-driver json-file --log-opt max-size=10m --log-opt max-file=1 \
--restart always \
--cap-add=NET_ADMIN \
-p $HYSTERIA_SERVER_PORT:$HYSTERIA_SERVER_PORT/udp \
--name $CONTAINER_NAME $CONTAINER_NAME

sudo docker network connect amnezia-dns-net $CONTAINER_NAME 2>/dev/null || true
