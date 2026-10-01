#!/bin/bash
# Upgrade in place; never regenerate credentials, certificates or client records.
set -euo pipefail
container=amnezia-hysteria2
version=v2.12.2
docker inspect "$container" >/dev/null
test "$(docker inspect -f '{{.State.Running}}' "$container")" = true
work=$(mktemp -d)
changed=0
locked=0
cleanup() {
    status=$1
    trap - EXIT
    if [ "$status" -ne 0 ] && [ "$changed" = 1 ]; then
        echo 'Update failed; restoring the previous Hysteria binary' >&2
        docker stop -t 10 "$container" >/dev/null || true
        if ! { docker cp "$work/previous" "$container:/usr/bin/hysteria" && docker start "$container"; }; then
            echo "ROLLBACK FAILED: previous binary retained at $work/previous" >&2
            if [ "$locked" = 1 ]; then rmdir /tmp/selfvps-hysteria-update.lock; fi
            exit "$status"
        fi
    fi
    if [ "$locked" = 1 ]; then rmdir /tmp/selfvps-hysteria-update.lock; fi
    rm -rf -- "$work"
    exit "$status"
}
trap 'cleanup $?' EXIT
mkdir /tmp/selfvps-hysteria-update.lock 2>/dev/null || { echo 'Another Hysteria update is active' >&2; exit 1; }
locked=1
arch=$(docker exec "$container" uname -m)
case "$arch" in
    x86_64) asset=hysteria-linux-amd64; sha=6493dfffd55b5883f64c76c63880ecc32988f0c568c9ca9014907877b4d55f94 ;;
    aarch64) asset=hysteria-linux-arm64; sha=ebfacc1ec3a0edfd742cd68ce17f292a6092e606b9d11f99b035c1d888f3d709 ;;
    armv7l) asset=hysteria-linux-arm; sha=274a0de7e2d145aa03fac017a7c7e9a995620f4eaf8db41656d37eddf2764f03 ;;
    *) echo "Unsupported architecture: $arch" >&2; exit 1 ;;
esac
echo "Downloading Hysteria $version ($arch)"
curl -fL --retry 3 --connect-timeout 20 --max-time 180 \
    "https://github.com/HyNetworks/hysteria/releases/download/app/$version/$asset" -o "$work/hysteria"
echo "$sha  $work/hysteria" | sha256sum -c -
chmod 755 "$work/hysteria"
docker cp "$container:/usr/bin/hysteria" "$work/previous"
before=$(docker exec "$container" sh -c 'find /opt/amnezia/hysteria2 -type f -exec sha256sum {} \; | sort' | sha256sum)
docker cp "$work/hysteria" "$container:/usr/bin/hysteria.selfvps-new"
docker exec "$container" /usr/bin/hysteria.selfvps-new version | grep -F "$version"
changed=1
docker exec "$container" mv /usr/bin/hysteria.selfvps-new /usr/bin/hysteria
docker restart -t 10 "$container" >/dev/null
sleep 3
test "$(docker inspect -f '{{.State.Running}}' "$container")" = true
docker exec "$container" pidof hysteria >/dev/null
docker exec "$container" /usr/bin/hysteria version | grep -F "$version"
after=$(docker exec "$container" sh -c 'find /opt/amnezia/hysteria2 -type f -exec sha256sum {} \; | sort' | sha256sum)
test "$before" = "$after"
echo 'SELFVPS_HYSTERIA_UPDATE_OK v2.12.2; configuration and credentials preserved'
