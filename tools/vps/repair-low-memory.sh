#!/bin/bash
# Run as root. Keeps existing VPN containers and connections running.
set -euo pipefail
umask 077
[ "$(id -u)" -eq 0 ] || { echo 'Run as root' >&2; exit 1; }
for command in docker python3 swapon mkswap fallocate flock gzip; do command -v "$command" >/dev/null; done
exec 9>/run/selfvps-resource-guards.lock
flock -n 9 || { echo 'Another resource repair is running' >&2; exit 1; }
BACKUP_DIR="/var/backups/selfvps-resource-guards/$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$BACKUP_DIR"
cp -a /etc/fstab "$BACKUP_DIR/fstab"
printf '%s\n' "$BACKUP_DIR" > /var/backups/selfvps-resource-guards/latest

SWAP_FILE=/swapfile.selfvps
if [ "$(swapon --show --noheadings | wc -l)" -eq 0 ]; then
    if [ -e "$SWAP_FILE" ] || [ -L "$SWAP_FILE" ]; then
        [ -f "$SWAP_FILE" ] && [ ! -L "$SWAP_FILE" ] && [ "$(stat -c %u "$SWAP_FILE")" -eq 0 ] || exit 1
        [ "$(stat -c %s "$SWAP_FILE")" -eq 2147483648 ] || { echo 'Unexpected existing swap file' >&2; exit 1; }
    else
        python3 -c 'import shutil; assert shutil.disk_usage("/").free > 4*1024**3, "Insufficient disk for swap reserve"'
        fallocate -l 2G "$SWAP_FILE"
        chmod 600 "$SWAP_FILE"
        mkswap "$SWAP_FILE" >/dev/null
    fi
    swapon "$SWAP_FILE"
fi
if awk -v path="$SWAP_FILE" 'NR>1 && $1==path {found=1} END {exit !found}' /proc/swaps; then
    grep -Eq '^/swapfile\.selfvps[[:space:]]' /etc/fstab || printf '\n/swapfile.selfvps none swap sw 0 0 # selfvps-resource-guard\n' >> /etc/fstab
fi

# Apply limits without a restart only to the affected mKCP XRay container.
# Save the previous values for a controlled rollback.
if docker inspect amnezia-xray >/dev/null 2>&1; then
    docker inspect --format '{{.HostConfig.Memory}} {{.HostConfig.MemorySwap}}' amnezia-xray > "$BACKUP_DIR/xray-memory-limits"
    docker update --memory 512m --memory-swap 1g amnezia-xray >/dev/null
fi

ROTATOR=/usr/local/sbin/selfvps-xray-logrotate
[ ! -e "$ROTATOR" ] || cp -a "$ROTATOR" "$BACKUP_DIR/logrotate-script"
cat > "$ROTATOR" <<'ROTATE'
#!/bin/bash
set -euo pipefail
umask 077
exec 9>/run/selfvps-xray-logrotate.lock
flock -n 9 || exit 0
ARCHIVES=/var/log/selfvps-xray
mkdir -p "$ARCHIVES"
chmod 700 "$ARCHIVES"
for container in amnezia-xray amnezia-xrayreality amnezia-ssxray; do
    [ "$(docker inspect --format '{{.State.Running}}' "$container" 2>/dev/null || true)" = true ] || continue
    size=$(docker exec "$container" sh -c 'p=/opt/amnezia/xray/access.log; [ -f "$p" ] && [ ! -L "$p" ] && stat -c %s "$p"' 2>/dev/null || true)
    [[ "$size" =~ ^[0-9]+$ ]] || continue
    [ "$size" -gt 20971520 ] || continue
    archive="$ARCHIVES/$container-$(date -u +%Y%m%dT%H%M%SZ).log"
    docker cp "$container:/opt/amnezia/xray/access.log" "$archive"
    chmod 600 "$archive"
    gzip "$archive"
    # Preserve the open inode used by XRay; rotating never restarts the process.
    docker exec "$container" sh -c 'p=/opt/amnezia/xray/access.log; [ -f "$p" ] && [ ! -L "$p" ] && truncate -s 0 "$p"'
    # Retain three compressed archives per fixed, allowlisted container.
    python3 - "$ARCHIVES" "$container" <<'PY'
import pathlib,sys
root=pathlib.Path(sys.argv[1]).resolve()
files=sorted(root.glob(sys.argv[2]+'-*.log.gz'),key=lambda p:p.name,reverse=True)
for file in files[3:]:
    if file.is_file() and not file.is_symlink() and file.resolve().parent==root:
        file.unlink()
PY
    echo "Rotated $container access log ($size bytes); backup retained"
done
ROTATE
chmod 700 "$ROTATOR"
CRON=/etc/cron.d/selfvps-xray-logrotate
[ ! -e "$CRON" ] || cp -a "$CRON" "$BACKUP_DIR/logrotate-cron"
printf '*/10 * * * * root /usr/local/sbin/selfvps-xray-logrotate\n' > "$CRON"
chmod 644 "$CRON"
bash -n "$ROTATOR"
"$ROTATOR"
printf 'Resource guards applied; backups: %s\n' "$BACKUP_DIR"
swapon --show
free -m
docker inspect --format 'XRay limits: memory={{.HostConfig.Memory}} memorySwap={{.HostConfig.MemorySwap}} started={{.State.StartedAt}} restart={{.RestartCount}}' amnezia-xray
