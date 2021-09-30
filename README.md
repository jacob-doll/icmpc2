# Gameplan

change hostname and disable monitoring on pfsense boxes

sysctl -w net.ipv4.icmp_echo_ignore_all=1
make sure to change ip in ping.sh

1. run pfctl -d
2. add a new ssh user (pw user add -n marlena -c 'Marlean Root' -d /home/marlena -G wheel -m -s /usr/local/bin/bash)
3. enable ssh
