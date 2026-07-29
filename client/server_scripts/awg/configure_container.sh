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
    AWG_HEADER_PROTECTION_KEY=$(awg genkey 2>/dev/null || true)
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

# Append header protection and prove the daemon accepts the result. If awg refuses
# the config we take the line back out and leave no key file, so the client builds
# a plain AWG2 config instead of one the server cannot honour.
if [ -n "$AWG_HEADER_PROTECTION_KEY" ]; then
    echo "HeaderProtectionKey = $AWG_HEADER_PROTECTION_KEY" >> /opt/amnezia/awg/awg0.conf
    if awg-quick strip /opt/amnezia/awg/awg0.conf >/dev/null 2>&1; then
        echo "$AWG_HEADER_PROTECTION_KEY" > /opt/amnezia/awg/awg_header_protection.key
    else
        sed -i '/^HeaderProtectionKey = /d' /opt/amnezia/awg/awg0.conf
    fi
fi
