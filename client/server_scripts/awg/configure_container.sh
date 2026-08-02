mkdir -p /opt/amnezia/awg
cd /opt/amnezia/awg
WIREGUARD_SERVER_PRIVATE_KEY=$(awg genkey)
echo $WIREGUARD_SERVER_PRIVATE_KEY > /opt/amnezia/awg/wireguard_server_private_key.key

WIREGUARD_SERVER_PUBLIC_KEY=$(echo $WIREGUARD_SERVER_PRIVATE_KEY | awg pubkey)
echo $WIREGUARD_SERVER_PUBLIC_KEY > /opt/amnezia/awg/wireguard_server_public_key.key

WIREGUARD_PSK=$(awg genpsk)
echo $WIREGUARD_PSK > /opt/amnezia/awg/wireguard_psk.key

# AmneziaWG 3 header protection (opt-in, see Settings::isAwgHeaderProtectionEnabled).
# Encrypts the low-entropy header fields that make a WireGuard packet recognisable
# to DPI in the first place. The key must be byte-identical on both peers, so it is
# minted here once and read back by the client when it builds its config.
#
# $AWG_ENABLE_HEADER_PROTECTION is substituted client-side to "true"/"false".
# Even when asked for it, only keep the key if this image actually understands the
# option -- an older amneziawg-go rejects the whole config and would leave a dead
# tunnel behind. The key file is what the client feature-detects on, so it must
# exist only when header protection really is active. by vovankrot
AWG_HEADER_PROTECTION_KEY=""
rm -f /opt/amnezia/awg/awg_header_protection.key
if [ "$AWG_ENABLE_HEADER_PROTECTION" = "true" ]; then
    # Both halves have to understand header protection before we commit to it:
    # the Go daemon reads it over UAPI as header_protection_key, the userspace
    # tools parse HeaderProtectionKey out of the .conf. Probe the binaries for
    # those literals -- they exist only in AWG3 builds. Writing a key that either
    # half cannot use produces a config that loads but carries no traffic, which
    # is far worse than staying on generation 2.
    AWG_GO_BIN=$(command -v amneziawg-go 2>/dev/null || command -v awg-go 2>/dev/null || echo /usr/bin/amneziawg-go)
    AWG_TOOL_BIN=$(command -v awg 2>/dev/null || echo /usr/bin/awg)
    if grep -aq 'header_protection_key' "$AWG_GO_BIN" 2>/dev/null \
       && grep -aq 'HeaderProtectionKey' "$AWG_TOOL_BIN" 2>/dev/null; then
        AWG_HEADER_PROTECTION_KEY=$(awg genkey 2>/dev/null || true)
    else
        echo "AmneziaWG 3 not supported by this image, installing generation 2" >&2
    fi
fi

cat > /opt/amnezia/awg/awg0.conf <<EOF
[Interface]
PrivateKey = $WIREGUARD_SERVER_PRIVATE_KEY
Address = $AWG_SUBNET_IP/$WIREGUARD_SUBNET_CIDR
ListenPort = $AWG_SERVER_PORT
Jc = $JUNK_PACKET_COUNT
Jmin = $JUNK_PACKET_MIN_SIZE
Jmax = $JUNK_PACKET_MAX_SIZE
S1 = $INIT_PACKET_JUNK_SIZE
S2 = $RESPONSE_PACKET_JUNK_SIZE
S3 = $COOKIE_REPLY_PACKET_JUNK_SIZE
S4 = $TRANSPORT_PACKET_JUNK_SIZE
H1 = $INIT_PACKET_MAGIC_HEADER
H2 = $RESPONSE_PACKET_MAGIC_HEADER
H3 = $UNDERLOAD_PACKET_MAGIC_HEADER
H4 = $TRANSPORT_PACKET_MAGIC_HEADER
# I1 = $SPECIAL_JUNK_1
# I2 = $SPECIAL_JUNK_2
# I3 = $SPECIAL_JUNK_3
# I4 = $SPECIAL_JUNK_4
# I5 = $SPECIAL_JUNK_5
EOF

# Capability was already established above, so commit the key. The file is what the
# client feature-detects on, so it exists only when header protection is really in
# the config.
#
# Note: do NOT try to validate with `awg-quick strip` here. strip performs no
# validation at all -- it filters out wg-quick-only keys and echoes the rest,
# passing unknown keys straight through -- so it succeeds regardless and proves
# nothing. The real parse happens in start.sh via `awg-quick up`.
if [ -n "$AWG_HEADER_PROTECTION_KEY" ]; then
    echo "HeaderProtectionKey = $AWG_HEADER_PROTECTION_KEY" >> /opt/amnezia/awg/awg0.conf
    echo "$AWG_HEADER_PROTECTION_KEY" > /opt/amnezia/awg/awg_header_protection.key
fi
