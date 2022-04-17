BUILD_DIR=build
SRC_DIR=src
CLIENT_DIR=$(SRC_DIR)/client
INTERFACE_DIR=$(SRC_DIR)/interface
SERVER_DIR=$(SRC_DIR)/server

CXX=clang++
CXX_FLAGS=-std=c++17 -Wall -Wextra -Wshadow -Wnon-virtual-dtor -pedantic

all: $(BUILD_DIR)/pingd_client_linux\
	 $(BUILD_DIR)/pingd_client_bsd\
  	 $(BUILD_DIR)/pingd_cli\
	 $(BUILD_DIR)/pingd_server

clean:
	rm -rf $(BUILD_DIR)

$(BUILD_DIR)/pingd_client_linux: $(CLIENT_DIR)/pingd_client.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXX_FLAGS) -pthread -o $(BUILD_DIR)/pingd_client_linux $(CLIENT_DIR)/pingd_client.cpp

$(BUILD_DIR)/pingd_client_bsd: $(CLIENT_DIR)/pingd_client.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXX_FLAGS) -target x86_64-unknown-freebsd12.2 --sysroot=/opt/cross-freebsd-12/ -pthread -o $(BUILD_DIR)/pingd_client_bsd $(CLIENT_DIR)/pingd_client.cpp

$(BUILD_DIR)/pingd_cli: $(INTERFACE_DIR)/pingd_cli.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXX_FLAGS) -o $(BUILD_DIR)/pingd_cli $(INTERFACE_DIR)/pingd_cli.cpp -lreadline

$(BUILD_DIR)/pingd_server: $(SERVER_DIR)/pingd_server.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXX_FLAGS) -pthread -o $(BUILD_DIR)/pingd_server $(SERVER_DIR)/pingd_server.cpp 

# clang++ -lIphlpapi -lws2_32 -o .\build\pingd_client_win.exe .\src\client\pingd_client_win.cpp
