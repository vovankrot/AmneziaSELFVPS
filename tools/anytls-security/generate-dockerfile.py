"""Generate the server Dockerfile from the exact same patch tested for Windows."""
from pathlib import Path
import base64
root=Path(__file__).resolve().parents[2]
patch=base64.b64encode((Path(__file__).parent/'patch.py').read_bytes()).decode()
dockerfile=f'''FROM golang:1.24.4-alpine3.21@sha256:56a23791af0f77c87b049230ead03bd8c3ad41683415ea4595e84ce7eada121a AS builder
RUN apk add --no-cache git python3
WORKDIR /src
RUN git clone https://github.com/anytls/anytls-go.git . && \\
    git checkout --detach 2012ef89768409f45437f1c06a7af5f6eea402ad && \\
    test "$(git rev-parse HEAD)" = 2012ef89768409f45437f1c06a7af5f6eea402ad
RUN printf '%s' '{patch}' | base64 -d > /tmp/patch.py && python3 /tmp/patch.py /src
RUN go test ./util && CGO_ENABLED=0 go build -trimpath -o /out/anytls-server ./cmd/server

FROM alpine:3.21@sha256:ce64758a109eb420d874a118f87920e625e12d3634e03b4a5573fd9f6e5d3507
LABEL maintainer="AmneziaSELFVPS"
RUN apk add --no-cache bash openssl ca-certificates dumb-init iptables ip6tables tzdata
COPY --from=builder /out/anytls-server /usr/bin/anytls-server
RUN mkdir -p /opt/amnezia/anytls && printf '#!/bin/bash\\ntail -f /dev/null\\n' > /opt/amnezia/start.sh && chmod 755 /opt/amnezia/start.sh
ENV TZ=UTC
ENTRYPOINT ["dumb-init", "/opt/amnezia/start.sh"]
'''
(root/'client/server_scripts/anytls/Dockerfile').write_text(dockerfile)
