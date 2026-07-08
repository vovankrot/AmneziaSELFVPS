#!/bin/bash
set -euo pipefail

# Hysteria 2 configurator. Generates:
#   * an HTTPS-style self-signed cert with the SNI we will impersonate
#   * a strong random password (used by the client for auth)
#   * the server YAML config
# Notes:
#   * Hysteria 2 listens on UDP only. The container exposes -p $HYSTERIA_SERVER_PORT/udp.
#   * "masquerade" makes the server serve a real-looking HTTPS site to anyone
#     who connects without valid auth — this is the entire point vs raw WireGuard.

CONFIG_DIR="/opt/amnezia/hysteria2"
mkdir -p "$CONFIG_DIR"
cd "$CONFIG_DIR"

CONFIG_PATH="$CONFIG_DIR/config.yaml"
# IMPORTANT: keep the `.yaml` extension on the staging file. Hysteria's
# config loader (viper) picks the parser by file extension, so a name like
# `config.yaml.tmp.aMeKga` makes it try the literal "aMeKga" parser and
# fail with `Unsupported Config Type "aMeKga"`.
CONFIG_TMP="$CONFIG_DIR/.config.staging.$$.yaml"
PASSWORD_PATH="$CONFIG_DIR/hysteria2_password.key"
CERT_PATH="$CONFIG_DIR/hysteria2_server.crt"
KEY_PATH="$CONFIG_DIR/hysteria2_server.key"
MASQ_HOST_PATH="$CONFIG_DIR/hysteria2_masquerade_host.key"

VALIDATE_LOG="/tmp/amnezia_hysteria2_validate.log"

cleanup() {
    rm -f "$CONFIG_TMP" "$VALIDATE_LOG"
}
trap cleanup EXIT

# Masquerade target. We default to www.bing.com — globally reachable, high-volume.
# Override with HYSTERIA_MASQUERADE_HOST from outside if user picks another site.
MASQ_HOST="${HYSTERIA_MASQUERADE_HOST:-www.bing.com}"
echo "$MASQ_HOST" > "$MASQ_HOST_PATH"

# Password — 32 bytes hex. Hysteria 2 supports a single shared password by
# default; for now we keep one. Multi-user (per-client) auth needs the
# "userpass" auth mode and a userdb file — left for a future revision.
HYSTERIA_PASSWORD="$(openssl rand -hex 16)"
echo "$HYSTERIA_PASSWORD" > "$PASSWORD_PATH"
chmod 600 "$PASSWORD_PATH"

# Salamander obfuscation password. Salamander XORs every packet into pseudo-random
# noise so DPI cannot fingerprint the QUIC/Hysteria handshake. This is what lets
# Hysteria pass RKN/TSPU: plain QUIC/UDP to a flagged server IP is dropped, but
# salamander-obfuscated UDP passes (verified end-to-end on this network). by vovankrot
HYSTERIA_OBFS_PASSWORD="$(openssl rand -hex 16)"
echo "$HYSTERIA_OBFS_PASSWORD" > "$CONFIG_DIR/hysteria2_obfs_password.key"
chmod 600 "$CONFIG_DIR/hysteria2_obfs_password.key"

# Self-signed cert. ECDSA P-256, valid 100 years. CN matches MASQ_HOST so that
# clients with `tls.sni: $MASQ_HOST` and `tls.insecure: true` see a name match
# in logs (still insecure, the point is just to look like real HTTPS to DPI).
timeout 30 openssl req -x509 -nodes \
    -newkey ec:<(openssl ecparam -name prime256v1) \
    -keyout "$KEY_PATH" \
    -out "$CERT_PATH" \
    -subj "/CN=$MASQ_HOST" \
    -days 36500 \
    >/dev/null 2>&1
chmod 600 "$KEY_PATH"

cat > "$CONFIG_TMP" <<EOF
listen: :$HYSTERIA_SERVER_PORT

tls:
  cert: $CERT_PATH
  key: $KEY_PATH

obfs:
  type: salamander
  salamander:
    password: $HYSTERIA_OBFS_PASSWORD

auth:
  type: password
  password: $HYSTERIA_PASSWORD

masquerade:
  type: proxy
  proxy:
    url: https://$MASQ_HOST/
    rewriteHost: true

# Brutal CC is enabled per-client via the client's bandwidth hints —
# server just needs to not throttle.
bandwidth:
  up: 1 gbps
  down: 1 gbps

# Quiet logs in production; flip to "info" if debugging.
log:
  level: warn
EOF

# Validate by running the server briefly. timeout 124 means the daemon kept
# running long enough to bind the socket, so the YAML parsed successfully.
set +e
timeout 12 hysteria server --disable-update-check -c "$CONFIG_TMP" >"$VALIDATE_LOG" 2>&1
rc=$?
set -e
if [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ]; then
  echo "ERROR: Hysteria 2 config failed validation (exit $rc)" >&2
  cat "$VALIDATE_LOG" >&2
  exit 1
fi

mv "$CONFIG_TMP" "$CONFIG_PATH"

trap - EXIT
cleanup
