#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <netinet/ip.h>
#include <arpa/inet.h>

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

long send_ping(int sockfd, const char *dest, uint8_t *data, size_t size)
{
    uint8_t buf[1024];
    icmphdr *icmp = (icmphdr *)buf;
    icmp->type = 8;
    icmp->code = 0;
    icmp->checksum = 0;

    if (data && size > 0)
    {
        memcpy(&buf[sizeof(icmphdr)], data, size);
    }

    icmp->checksum = htons(checksum(buf, sizeof(icmp) + size));

    sockaddr_in addr_;
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(0);
    addr_.sin_addr.s_addr = inet_addr(dest);

    long ret;
    if ((ret = sendto(sockfd, buf, sizeof(icmp) + size, 0, (sockaddr *)&addr_, sizeof(addr_))) == -1)
    {
        perror("sendto");
    }
    return ret;
}

long receive_ping(int sockfd, uint8_t *buf, size_t size)
{
    long ret = 0;
    if ((ret = read(sockfd, buf, size)) == -1)
    {
        return -1;
    }
    return ret;
}

int main(int argc, char **argv)
{
    int sockfd = socket(PF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sockfd == -1)
    {
        perror("socket");
        return -1;
    }

    while (1)
    {

        const char *data = "Hello World!";

        send_ping(sockfd, "8.8.8.8", (uint8_t *)data, strlen(data) + 1);
        std::cout << "Sent ping!\n";

        uint8_t buf[1024];
        long nbytes = receive_ping(sockfd, buf, sizeof(buf));
        if (nbytes > 0)
        {
            iphdr *ip = (iphdr *)buf;
            if (nbytes > sizeof(iphdr))
            {
                in_addr addr{ip->saddr};
                std::cout << inet_ntoa(addr) << "\n";
            }
        }

        usleep(3000000);
    }

    return 0;
}