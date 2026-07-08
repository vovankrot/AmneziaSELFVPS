FROM alpine:3.20
LABEL maintainer="AmneziaVPN"

# AnyTLS — TLS-in-TLS censorship-bypass proxy with multiplexing.
# Upstream: https://github.com/anytls/anytls-go
ARG ANYTLS_VERSION="0.0.12"

RUN apk add --no-cache curl unzip bash openssl ca-certificates dumb-init iptables ip6tables tzdata
RUN apk --update upgrade --no-cache

RUN mkdir -p /opt/amnezia /opt/amnezia/anytls
RUN echo -e "#!/bin/bash\ntail -f /dev/null" > /opt/amnezia/start.sh
RUN chmod a+x /opt/amnezia/start.sh

RUN set -eux; \
  arch="$(uname -m)"; \
  case "$arch" in \
    x86_64)  asset_arch="amd64" ;; \
    aarch64) asset_arch="arm64" ;; \
    armv7l)  asset_arch="armv7" ;; \
    *) echo "Unsupported arch: $arch" >&2; exit 1 ;; \
  esac; \
  curl -fL --retry 3 --retry-delay 2 \
    "https://github.com/anytls/anytls-go/releases/download/v${ANYTLS_VERSION}/anytls_${ANYTLS_VERSION}_linux_${asset_arch}.zip" \
    -o /root/anytls.zip; \
  unzip /root/anytls.zip -d /usr/bin/; \
  chmod +x /usr/bin/anytls-server /usr/bin/anytls-client 2>/dev/null || true; \
  rm /root/anytls.zip; \
  /usr/bin/anytls-server -h 2>&1 | head -n 1 || true

RUN echo -e " \n\
  fs.file-max = 51200 \n\
  net.core.rmem_max = 67108864 \n\
  net.core.wmem_max = 67108864 \n\
  net.core.netdev_max_backlog = 250000 \n\
  net.core.somaxconn = 4096 \n\
  net.core.default_qdisc = fq \n\
  net.ipv4.tcp_congestion_control = bbr \n\
  net.ipv4.tcp_fastopen = 3 \n\
  net.ipv4.ip_local_port_range = 10000 65000 \n\
  " | sed -e 's/^\s\+//g' | tee -a /etc/sysctl.conf && \
  mkdir -p /etc/security && \
  echo -e " \n\
  * soft nofile 51200 \n\
  * hard nofile 51200 \n\
  " | sed -e 's/^\s\+//g' | tee -a /etc/security/limits.conf

ENV TZ=UTC

ENTRYPOINT [ "dumb-init", "/opt/amnezia/start.sh" ]
