CC=g++
CFLAGS=/wall /werror /pedantic

all: master slave

run_master: master
	./bin/icmp_master

run_slave: slave
	./bin/icmp_slave

master: master/icmp_master.cpp
	@mkdir -p bin
	$(CC) -pthread -o bin/icmp_master master/icmp_master.cpp

slave: slave/icmp_slave.cpp
	@mkdir -p bin
	$(CC) -o bin/icmp_slave slave/icmp_slave.cpp
