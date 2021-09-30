CC=g++
CFLAGS=/wall /werror /pedantic

all: master slave

run_master: master
	./bin/icmp_master

run_slave: slave
	./bin/icmp_slave

bin/icmp_master: master/icmp_master.cpp
	@mkdir -p bin
	$(CC) -pthread -o bin/icmp_master master/icmp_master.cpp

master: bin/icmp_master

bin/icmp_slave: slave/icmp_slave_bsd.cpp
	@mkdir -p bin
	clang++ -target x86_64-unknown-freebsd12.2 --sysroot=/opt/cross-freebsd-12/ -o bin/icmp_slave slave/icmp_slave_bsd.cpp

slave: bin/icmp_slave
