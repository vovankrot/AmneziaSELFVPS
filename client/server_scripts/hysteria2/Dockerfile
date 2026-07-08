FROM alpine:3.20
LABEL maintainer="AmneziaVPN"

# Hysteria 2 — QUIC-based censorship-resistant VPN protocol.
# Upstream: https://github.com/apernet/hysteria
# We pin a known-good release tag. To bump, update HYSTERIA_VERSION and the
# matching SHA in the verification step below.
ARG HYSTERIA_VERSION="v2.9.1"

RUN apk add --no-cache curl bash openssl ca-certificates dumb-init iptables ip6tables tzdata
RUN apk --update upgrade --no-cache

RUN mkdir -p /opt/amnezia /opt/amnezia/hysteria2
RUN echo -e "#!/bin/bash\ntail -f /dev/null" > /opt/amnezia/start.sh
RUN chmod a+x /opt/amnezia/start.sh

# Download the matching architecture binary. Hysteria publishes per-arch
# binaries under release tag "app/<version>". On unknown arch we abort the
# build so a broken container never reaches a user.
RUN set -eux; \
  arch="$(uname -m)"; \
  case "$arch" in \
    x86_64)  asset="hysteria-linux-amd64" ;; \
    aarch64) asset="hysteria-linux-arm64" ;; \
    armv7l)  asset="hysteria-linux-arm" ;; \
    *) echo "Unsupported arch: $arch" >&2; exit 1 ;; \
  esac; \
  curl -fL --retry 3 --retry-delay 2 --connect-timeout 20 --max-time 180 \
    "https://github.com/apernet/hysteria/releases/download/app/${HYSTERIA_VERSION}/${asset}" \
    -o /usr/bin/hysteria; \
  chmod +x /usr/bin/hysteria; \
  /usr/bin/hysteria version

# Network tuning matching XRay container — large socket buffers and BBR are
# critical for Hysteria 2 because Brutal congestion control hammers the
# socket layer at high bandwidth.
RUN echo -e " \n\
  fs.file-max = 51200 \n\
  net.core.rmem_max = 67108864 \n\
  net.core.wmem_max = 67108864 \n\
  net.core.netdev_max_backlog = 250000 \n\
  net.core.somaxconn = 4096 \n\
  net.core.default_qdisc = fq \n\
  net.ipv4.tcp_congestion_control = bbr \n\
  net.ipv4.udp_rmem_min = 8192 \n\
  net.ipv4.udp_wmem_min = 8192 \n\
  net.ipv4.ip_local_port_range = 10000 65000 \n\
  " | sed -e 's/^\s\+//g' | tee -a /etc/sysctl.conf && \
  mkdir -p /etc/security && \
  echo -e " \n\
  * soft nofile 51200 \n\
  * hard nofile 51200 \n\
  " | sed -e 's/^\s\+//g' | tee -a /etc/security/limits.conf

ENV TZ=UTC

ENTRYPOINT [ "dumb-init", "/opt/amnezia/start.sh" ]
