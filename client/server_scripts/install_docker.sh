if which apt-get > /dev/null 2>&1; then pm=$(which apt-get); silent_inst="-yq install"; check_pkgs="-yq update"; docker_pkg="docker.io"; dist="debian";\
elif which dnf > /dev/null 2>&1; then pm=$(which dnf); silent_inst="-yq install"; check_pkgs="-yq check-update"; docker_pkg="docker"; dist="fedora";\
elif which yum > /dev/null 2>&1; then pm=$(which yum); silent_inst="-y -q install"; check_pkgs="-y -q check-update"; docker_pkg="docker"; dist="centos";\
elif which zypper > /dev/null 2>&1; then pm=$(which zypper); silent_inst="-nq install"; check_pkgs="-nq refresh"; docker_pkg="docker"; dist="opensuse";\
elif which pacman > /dev/null 2>&1; then pm=$(which pacman); silent_inst="-S --noconfirm --noprogressbar --quiet"; check_pkgs="-Sup"; docker_pkg="docker"; dist="archlinux";\
else echo "Packet manager not found"; exit 1; fi;\
echo "Dist: $dist, Packet manager: $pm, Install command: $silent_inst, Check pkgs command: $check_pkgs, Docker pkg: $docker_pkg";\
if [ "$dist" = "debian" ]; then export DEBIAN_FRONTEND=noninteractive; fi;\
if ! command -v sudo > /dev/null 2>&1; then $pm $check_pkgs; $pm $silent_inst sudo; fi;\
if ! command -v fuser > /dev/null 2>&1; then sudo $pm $check_pkgs; sudo $pm $silent_inst psmisc; fi;\
if ! command -v lsof > /dev/null 2>&1; then sudo $pm $check_pkgs; sudo $pm $silent_inst lsof; fi;\
if ! command -v docker > /dev/null 2>&1; then \
  sudo $pm $check_pkgs; sudo $pm $silent_inst $docker_pkg;\
  sudo mkdir -p /etc/docker;\
  if [ ! -f /etc/docker/daemon.json ]; then \
    echo '{ "dns": ["1.1.1.1", "9.9.9.9"] }' | sudo tee /etc/docker/daemon.json > /dev/null;\
  fi;\
  sleep 5; sudo systemctl enable --now docker; sleep 5;\
fi;\
if [ "$(cat /sys/module/apparmor/parameters/enabled 2>/dev/null)" = "Y" ]; then \
  if ! command -v apparmor_parser > /dev/null 2>&1; then sudo $pm $check_pkgs; sudo $pm $silent_inst apparmor; fi;\
fi;\
if ! command -v fail2ban-server > /dev/null 2>&1; then sudo $pm $check_pkgs; sudo $pm $silent_inst fail2ban || true; fi;\
if command -v fail2ban-server > /dev/null 2>&1; then \
  if [ ! -f /etc/fail2ban/jail.local ] || ! grep -q "managed-by: amnezia-bootstrap" /etc/fail2ban/jail.local 2>/dev/null; then \
    sudo mkdir -p /etc/fail2ban;\
    printf '%s\n' '# managed-by: amnezia-bootstrap' '[DEFAULT]' 'ignoreip = 127.0.0.1/8 ::1 10.0.0.0/8 172.16.0.0/12 192.168.0.0/16' 'findtime = 10m' 'bantime  = 1h' 'bantime.increment = true' 'bantime.factor = 2' 'bantime.maxtime = 1w' 'maxretry = 5' 'backend = systemd' '' '[sshd]' 'enabled  = true' 'port     = ssh' 'filter   = sshd' 'maxretry = 5' 'findtime = 10m' 'bantime  = 1h' | sudo tee /etc/fail2ban/jail.local > /dev/null;\
  fi;\
  sudo systemctl enable --now fail2ban > /dev/null 2>&1 || true;\
  sudo systemctl restart fail2ban > /dev/null 2>&1 || true;\
fi;\
if [ ! -f /etc/sysctl.d/99-amnezia-perf.conf ] || ! grep -q "managed-by: amnezia-bootstrap" /etc/sysctl.d/99-amnezia-perf.conf 2>/dev/null; then \
  sudo modprobe tcp_bbr 2>/dev/null || true;\
  echo tcp_bbr | sudo tee /etc/modules-load.d/bbr.conf > /dev/null 2>&1 || true;\
  printf '%s\n' '# managed-by: amnezia-bootstrap' 'net.core.default_qdisc=fq' 'net.ipv4.tcp_congestion_control=bbr' 'net.core.rmem_max=67108864' 'net.core.wmem_max=67108864' 'net.ipv4.tcp_rmem=4096 87380 67108864' 'net.ipv4.tcp_wmem=4096 65536 67108864' 'net.ipv4.tcp_mtu_probing=1' 'net.ipv4.tcp_fastopen=3' 'net.ipv4.tcp_notsent_lowat=131072' 'net.core.netdev_max_backlog=250000' 'net.core.somaxconn=4096' 'net.ipv4.tcp_max_syn_backlog=8192' | sudo tee /etc/sysctl.d/99-amnezia-perf.conf > /dev/null;\
  sudo sysctl --system > /dev/null 2>&1 || true;\
fi;\
if [ "$(systemctl is-active docker)" != "active" ]; then \
  sudo $pm $check_pkgs; sudo $pm $silent_inst $docker_pkg;\
  sleep 5; sudo systemctl start docker; sleep 5;\
fi;\
if ! command -v sudo > /dev/null 2>&1; then echo "Failed to install sudo, command not found"; exit 1; fi;\
docker --version;\
uname -sr
