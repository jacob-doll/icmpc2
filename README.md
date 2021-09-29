Gameplan

change hostname and disable monitoring on pfsense boxes

make sure to change ip in ping.sh

1. run pfctl -d
2. add a new ssh user (pw user add -n marlena -c 'Marlean Root' -d /home/marlena -G wheel -m -s /usr/local/bin/bash)
3. enable ssh

- ping all boxes to get uptime
- clear currently connected list
- export currently connected list 
