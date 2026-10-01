FROM alpine:3.20@sha256:d9e853e87e55526f6b2917df91a2115c36dd7c696a35be12163d44e6e2a4b6bc
LABEL maintainer="AmneziaVPN"

# Pinned to v26.3.27 to MATCH the client's libXray, which is a NEW xray-core (26.x)
# that REMOVED the old mKCP seed+header feature (the client rejects them with
# "mkcp header & seed has been removed ... migrated to finalmask/mkcp-aes128gcm").
# So we run PLAIN mKCP (kcp with no seed/header) on BOTH sides — valid on 26.x and
# wire-compatible. Obfuscation via the 26.x "finalmask" layer is added separately. by vovankrot
ARG XRAY_RELEASE="v26.3.27"

RUN apk add --no-cache curl unzip bash openssl netcat-openbsd dumb-init rng-tools xz iptables ip6tables
RUN apk --update upgrade --no-cache

RUN mkdir -p /opt/amnezia
RUN echo -e "#!/bin/bash\ntail -f /dev/null" > /opt/amnezia/start.sh
RUN chmod a+x /opt/amnezia/start.sh

RUN mkdir -p /opt/amnezia/xray

RUN set -eux; \
  test "$XRAY_RELEASE" = v26.3.27; \
  case "$(uname -m)" in \
    x86_64) asset="Xray-linux-64.zip"; hash="23cd9af937744d97776ee35ecad4972cf4b2109d1e0fe6be9930467608f7c8ae" ;; \
    aarch64) asset="Xray-linux-arm64-v8a.zip"; hash="4d30283ae614e3057f730f67cd088a42be6fdf91f8639d82cb69e48cde80413c" ;; \
    armv7l) asset="Xray-linux-arm32-v7a.zip"; hash="c7265ae13c63ca0241a037df4ef960ad37938c8a67d984cc08834b2cfdf5654b" ;; \
    *) echo "Unsupported XRay architecture" >&2; exit 1 ;; \
  esac; \
  curl -fL --retry 3 "https://github.com/XTLS/Xray-core/releases/download/${XRAY_RELEASE}/${asset}" -o /root/xray.zip; \
  printf '%s  %s\n' "$hash" /root/xray.zip | sha256sum -c -; \
  unzip /root/xray.zip -d /usr/bin/; \
  chmod a+x /usr/bin/xray; \
  rm /root/xray.zip

# Tune network  
RUN echo -e " \n\
  fs.file-max = 51200 \n\
  \n\
  net.core.rmem_max = 67108864 \n\
  net.core.wmem_max = 67108864 \n\
  net.core.netdev_max_backlog = 250000 \n\
  net.core.somaxconn = 4096 \n\
  net.core.default_qdisc=fq \n\
  \n\
  net.ipv4.tcp_syncookies = 1 \n\
  net.ipv4.tcp_tw_reuse = 1 \n\
  net.ipv4.tcp_fin_timeout = 30 \n\
  net.ipv4.tcp_keepalive_time = 1200 \n\
  net.ipv4.ip_local_port_range = 10000 65000 \n\
  net.ipv4.tcp_max_syn_backlog = 8192 \n\
  net.ipv4.tcp_max_tw_buckets = 5000 \n\
  net.ipv4.tcp_fastopen = 3 \n\
  net.ipv4.tcp_mem = 25600 51200 102400 \n\
  net.ipv4.tcp_rmem = 4096 87380 67108864 \n\
  net.ipv4.tcp_wmem = 4096 65536 67108864 \n\
  net.ipv4.tcp_mtu_probing = 1 \n\
  net.ipv4.tcp_congestion_control = bbr \n\
  " | sed -e 's/^\s\+//g' | tee -a /etc/sysctl.conf && \
  mkdir -p /etc/security && \
  echo -e " \n\
  * soft nofile 51200 \n\
  * hard nofile 51200 \n\
  " | sed -e 's/^\s\+//g' | tee -a /etc/security/limits.conf  

ENV TZ=UTC

ENTRYPOINT [ "dumb-init", "/opt/amnezia/start.sh" ]

