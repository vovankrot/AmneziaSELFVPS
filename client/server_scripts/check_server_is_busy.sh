if which apt-get > /dev/null 2>&1; then procs="apt apt-get aptitude dpkg unattended-upgrade unattended-upgrades";\
elif which dnf > /dev/null 2>&1; then procs="dnf rpm";\
elif which yum > /dev/null 2>&1; then procs="yum rpm";\
elif which zypper > /dev/null 2>&1; then procs="zypper rpm";\
elif which pacman > /dev/null 2>&1; then procs="pacman pacman-key makepkg";\
else echo "Package manager not found"; echo "Internal error"; exit 1; fi;\
if command -v pgrep > /dev/null 2>&1; then \
	for proc in $procs; do match=$(pgrep -a -x "$proc" 2>/dev/null | head -n 1); if [ -n "$match" ]; then echo "$match"; exit 0; fi; done;\
fi;\
if command -v ps > /dev/null 2>&1; then \
	ps -eo pid=,comm= 2>/dev/null | awk '$2 ~ /^(apt|apt-get|aptitude|dpkg|unattended-upgrade|unattended-upgrades|dnf|yum|zypper|rpm|pacman|pacman-key|makepkg)$/ { print $0; exit }';\
else echo "ps not installed"; fi
