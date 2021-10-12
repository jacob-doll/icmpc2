# ICMP C2

C2 implementation using raw sockets over the ICMP protocol. The master server runs on Linux with agents available for Linux and FreeBSD. Support for Windows agents will be provided soon.

## Server

To run the server use this command:
> ./icmp_server [database_file]
>
> - **database_file** is a file of hostname to ip mappings. This can be useful if you know what hosts will be on the network. A data base file can also be exported using the export command on the server.

Before running the server make sure to run:
> sysctl -w net.ipv4.icmp_echo_ignore_all=1
>
> - This will disable the default reply from the kernel allowing the server to handle all ICMP requests.

### Supported commands

- help: displays usable commands
- list: lists all active connections to the server
- hosts: lists expected hosts that are supplied by the database file
- group: by default this command lists all groups
  - add/rm [group] [host]: adds/removes a host to a group to run commands on
  - list [group]: lists all hosts within a group
- set [host/group]: sets the current host to run commands on
- run [command]: runs a command on the currently set host
- file [src] [dst]: sends a file to the currently set host. src is the file on the server box, and dst is the location on the host machine.
- exfil [src] [dst]: exfiltrates a file from the currently set host. src is the location on the server to save the file, and dst is the location of the file on the host to exfiltrate.
- runall [command]: runs a command on all active connections
- export [filename]: export all active connections to a database file
- load [filename]: load a database file of host to ip mappings
- clear: clear the active connections. Useful for testing if connections still exist.
- exit: stops the server and exits

## Client

To run the client on the host machine use this command:
> ./icmp_client_TARGETOS interface ip
>
> - **interface** is the interface that you want to send and receive ICMP packets on.
> - **ip** is the IPv4 address of the server machine to send and receive ICMP packets from.

## TODO

- [ ] check for beacon uptime (add ttl for each host)
- [ ] have each beacon constantly ping (thread)
- [ ] each command will have its own salt
- [ ] groups
- [ ] libpcap
- [ ] have commands use int types instead of strings
- [ ] xor encoding of packets
- [ ] set sub-command
- [ ] group command and sub-commands
