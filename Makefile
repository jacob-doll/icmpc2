BUILD_DIR=build
SRC_DIR=src
CLIENT_DIR=$(SRC_DIR)/client
INTERFACE_DIR=$(SRC_DIR)/interface
SERVER_DIR=$(SRC_DIR)/server

CXX=clang++
CXX_FLAGS=-std=c++17 -Wall -Wextra -Wshadow -Wnon-virtual-dtor -pedantic

all: pingd_client_linux pingd_client_bsd pingd_cli pingd_server

clean:
	rm -rf $(BUILD_DIR)

pingd_client_linux: $(CLIENT_DIR)/pingd_client.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXX_FLAGS) -pthread -o $(BUILD_DIR)/pingd_client_linux $(CLIENT_DIR)/pingd_client.cpp

pingd_client_bsd: $(CLIENT_DIR)/pingd_client.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXX_FLAGS) -target x86_64-unknown-freebsd12.2 --sysroot=/opt/cross-freebsd-12/ -pthread -o $(BUILD_DIR)/pingd_client_bsd $(CLIENT_DIR)/pingd_client.cpp

pingd_cli: $(INTERFACE_DIR)/pingd_cli.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXX_FLAGS) -o $(BUILD_DIR)/pingd_cli $(INTERFACE_DIR)/pingd_cli.cpp -lreadline

pingd_server: $(SERVER_DIR)/pingd_server.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXX_FLAGS) -pthread -o $(BUILD_DIR)/pingd_server $(SERVER_DIR)/pingd_server.cpp 
