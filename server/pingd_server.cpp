#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>
#include <thread>
#include <mutex>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <unistd.h>

struct buffer
{
  int pos;
  std::vector<uint8_t> data;
};

struct connection
{
  connection(const std::string &hostname)
    : hostname{ hostname }
  {
    out_buf.pos = -1;
    in_buf.pos = -1;
  }
  std::string hostname;
  buffer out_buf;
  buffer in_buf;
};

using connections_t = std::map<uint32_t, connection>;

static connections_t active_connections;
static std::mutex active_connections_mutex;

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

void listen_task()
{
  int sockfd;
  uint8_t in[1024];
  sockaddr_in addr;

  if ((sockfd = socket(PF_INET, SOCK_RAW, IPPROTO_ICMP)) == -1) {
    perror("socket");
    std::exit(-1);
  }

  while (1) {
    std::memset(in, 0, sizeof(in));
    int nbytes = read(sockfd, in, sizeof(in));

    if (nbytes <= 0) continue;

    iphdr *ip = (iphdr *)in;

    if (nbytes > sizeof(iphdr)) {
      nbytes -= sizeof(iphdr);
      icmphdr *icmp = (icmphdr *)(in + sizeof(iphdr));

      if (active_connections.find(ip->saddr) == active_connections.end()) {
        std::lock_guard<std::mutex> guard(active_connections_mutex);
        active_connections.insert({ ip->saddr, connection{ inet_ntoa(in_addr{ ip->saddr }) } });
      }

      auto &connection = active_connections.at(ip->saddr);
      auto &in_buf = connection.in_buf;
      auto &out_buf = connection.out_buf;

      if (nbytes > sizeof(icmphdr)) {
        std::lock_guard<std::mutex> guard(active_connections_mutex);
        size_t in_size = nbytes - sizeof(icmphdr);
        size_t index = sizeof(iphdr) + sizeof(icmphdr);
        in_buf.data.insert(in_buf.data.end(), &in[index], &in[index + nbytes]);
      }

      size_t packet_size = sizeof(icmphdr);

      if (out_buf.pos >= 0 && out_buf.pos < out_buf.data.size()) {
        std::lock_guard<std::mutex> guard(active_connections_mutex);
        size_t to_write = out_buf.data.size() > 64 ? 64 : out_buf.data.size();
        std::memcpy(icmp + 1, out_buf.data.data(), to_write);
        out_buf.pos += to_write;
        packet_size += to_write;
        if (out_buf.pos >= out_buf.data.size()) {
          out_buf.data.clear();
          out_buf.pos = -1;
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
}

int main(int argc, char **argv)
{
  std::thread listen_thread{ listen_task };
  std::string hello = "Hello World!";

  while (1) {
    for (auto it = active_connections.begin();
         it != active_connections.end();
         it++) {
      if (it->second.out_buf.data.empty()) {
        std::lock_guard<std::mutex> guard(active_connections_mutex);
        it->second.out_buf.data.insert(it->second.out_buf.data.begin(), hello.begin(), hello.end());
        it->second.out_buf.pos = 0;
      }
      std::cout << it->second.out_buf.pos << " " << it->second.out_buf.data.size() << "\n";
    }
    sleep(1);
  }

  return 0;
}