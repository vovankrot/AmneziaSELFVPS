if [ -r /etc/os-release ]; then . /etc/os-release; echo "OS: ${PRETTY_NAME:-$NAME}"; else echo "OS: $(uname -a)"; fi;\
if which apt-get > /dev/null 2>&1; then pm="apt-get"; busy_names="apt apt-get aptitude dpkg unattended-upgrade unattended-upgrades";\
elif which dnf > /dev/null 2>&1; then pm="dnf"; busy_names="dnf rpm";\
elif which yum > /dev/null 2>&1; then pm="yum"; busy_names="yum rpm";\
elif which zypper > /dev/null 2>&1; then pm="zypper"; busy_names="zypper rpm";\
elif which pacman > /dev/null 2>&1; then pm="pacman"; busy_names="pacman pacman-key makepkg";\
else pm="not-found"; busy_names=""; fi;\
echo "Package manager: $pm";\
printf "Available tools:"; for tool in sudo docker systemctl pgrep ps lsof fuser curl wget; do if command -v "$tool" > /dev/null 2>&1; then printf " %s" "$tool"; fi; done; printf "\n";\
if [ -n "$busy_names" ]; then printf "Package processes:"; for proc in $busy_names; do if command -v pgrep > /dev/null 2>&1 && pgrep -a -x "$proc" > /dev/null 2>&1; then printf " %s" "$proc"; fi; done; printf "\n"; fi;\
uname -sr
