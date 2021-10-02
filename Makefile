CC=g++
CFLAGS=-Wall -Werror -pedantic

all: master slave_bsd slave_linux

run_master: master
	./bin/icmp_master

run_slave: slave
	./bin/icmp_slave

bin/icmp_master: master/icmp_master.cpp
	@mkdir -p bin
	$(CC) -pthread -o bin/icmp_master master/icmp_master.cpp

master: bin/icmp_master

bin/icmp_slave_bsd: slave/icmp_slave_bsd.cpp
	@mkdir -p bin
	clang++ -target x86_64-unknown-freebsd12.2 --sysroot=/opt/cross-freebsd-12/ -o bin/icmp_slave_bsd slave/icmp_slave_bsd.cpp

slave_bsd: bin/icmp_slave_bsd

bin/icmp_slave_linux: slave/icmp_slave_linux.cpp
	@mkdir -p bin
	$(CC) -o bin/icmp_slave_linux slave/icmp_slave_linux.cpp

slave_linux: bin/icmp_slave_linux
