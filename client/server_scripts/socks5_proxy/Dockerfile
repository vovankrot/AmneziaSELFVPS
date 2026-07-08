# Build 3proxy from source on Alpine (musl). Upstream 3proxy/3proxy:latest
# became distroless and broke RUN steps. Pinning to v0.9.5 avoids floating-tag drift.
FROM alpine:3.20 AS build
RUN apk add --no-cache build-base linux-headers git
RUN git clone --depth 1 --branch 0.9.5 https://github.com/3proxy/3proxy.git /src
WORKDIR /src
RUN ln -s Makefile.Linux Makefile && make -j$(nproc)

FROM alpine:3.20

LABEL maintainer="AmneziaVPN"

COPY --from=build /src/bin/3proxy /bin/3proxy
RUN mkdir -p /opt/amnezia /usr/local/3proxy/conf /usr/local/3proxy/logs
RUN printf '#!/bin/sh\ntail -f /dev/null\n' > /opt/amnezia/start.sh \
 && chmod a+x /opt/amnezia/start.sh

ENTRYPOINT [ "/bin/sh", "/opt/amnezia/start.sh" ]
CMD [ "" ]
