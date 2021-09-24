CC=g++
CFLAGS=/wall /werror /pedantic

all: master slave

run_master: master
	./bin/icmp_master

master: master/icmp.cpp
	@mkdir -p bin
	$(CC) -pthread -o bin/icmp_master master/icmp.cpp

slave: slave/icmp.cpp
	@mkdir -p bin
	$(CC) -o bin/icmp_slave slave/icmp.cpp
