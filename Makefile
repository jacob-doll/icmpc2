CC=clang++

all: pingd_server pingd_client_linux

clean:
	rm -rf bin/

bin/icmp_server: server/icmp_server.cpp
	@mkdir -p bin
	$(CC) -pthread -o bin/icmp_server server/icmp_server.cpp -lreadline

server: bin/icmp_server

bin/pingd_cli: server/pingd_cli.cpp
	@mkdir -p bin
	$(CC) -pthread -std=c++17 -o bin/pingd_cli server/pingd_cli.cpp -lreadline

bin/pingd_server: server/pingd_server.cpp
	@mkdir -p bin
	$(CC) -pthread -std=c++17 -o bin/pingd_server server/pingd_server.cpp 

pingd_server: bin/pingd_server bin/pingd_cli

bin/icmp_client_bsd: client/icmp_client_bsd.cpp
	@mkdir -p bin
	$(CC) -target x86_64-unknown-freebsd12.2 --sysroot=/opt/cross-freebsd-12/ -pthread -o bin/icmp_client_bsd client/icmp_client_bsd.cpp

client_bsd: bin/icmp_client_bsd

bin/icmp_client_linux: client/icmp_client_linux.cpp
	@mkdir -p bin
	$(CC) -pthread -o bin/icmp_client_linux client/icmp_client_linux.cpp

client_linux: bin/icmp_client_linux

bin/pingd_client_linux: client/pingd_client_linux.cpp
	@mkdir -p bin
	$(CC) -pthread -o bin/pingd_client_linux client/pingd_client_linux.cpp

pingd_client_linux: bin/pingd_client_linux
