CC=clang++
CFLAGS=-Wall -Werror -pedantic

all: server client_bsd client_linux

clean:
	rm -rf bin/

bin/icmp_server: server/icmp_server.cpp
	@mkdir -p bin
	$(CC) -pthread -o bin/icmp_server server/icmp_server.cpp

server: bin/icmp_server

bin/icmp_client_bsd: client/icmp_client_bsd.cpp
	@mkdir -p bin
	$(CC) -target x86_64-unknown-freebsd12.2 --sysroot=/opt/cross-freebsd-12/ -pthread -o bin/icmp_client_bsd client/icmp_client_bsd.cpp

client_bsd: bin/icmp_client_bsd

bin/icmp_client_linux: client/icmp_client_linux.cpp
	@mkdir -p bin
	$(CC) -pthread -o bin/icmp_client_linux client/icmp_client_linux.cpp

client_linux: bin/icmp_client_linux
