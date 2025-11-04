#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>
#include <optional>
#include <string>
#include <unordered_map>

namespace {

struct Client {
    std::string ip;
    uint16_t port;

    std::string local_ip;
    uint16_t local_port;
};

void send_connection_info(int fd, const Client &a, const Client &b) {
    sockaddr_in a_addr{}, b_addr{};
    a_addr.sin_family = AF_INET;
    a_addr.sin_port = htons(a.port);
    ::inet_pton(AF_INET, a.ip.c_str(), &a_addr.sin_addr);
    b_addr.sin_family = AF_INET;
    b_addr.sin_port = htons(b.port);
    ::inet_pton(AF_INET, b.ip.c_str(), &b_addr.sin_addr);

    std::string msg_a, msg_b;
    if (a.ip != b.ip) {
        msg_a = b.ip + " " + std::to_string(b.port) + "\n";
        msg_b = a.ip + " " + std::to_string(a.port) + "\n";
    } else {
        msg_a = b.local_ip + " " + std::to_string(b.local_port) + "\n";
        msg_b = a.local_ip + " " + std::to_string(a.local_port) + "\n";
    }

    ::sendto(fd, msg_a.data(), msg_a.size(), 0,
        reinterpret_cast<const sockaddr*>(&a_addr), sizeof(a_addr));

    ::sendto(fd, msg_b.data(), msg_b.size(), 0,
        reinterpret_cast<const sockaddr*>(&b_addr), sizeof(b_addr));
}

inline uint16_t parse_port(const std::string& str) {
    char* end = nullptr;
    long port = std::strtol(str.c_str(), &end, 10);
    assert(end != nullptr && *end == '\0');
    return static_cast<uint16_t>(port);
}

} // namespace anonymous

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <ip> <port>\n";
        return 1;
    }

    const char *ip = argv[1];
    uint16_t port = parse_port(argv[2]);

    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);

    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    ::inet_pton(AF_INET, ip, &bind_addr.sin_addr);

    int bind_res = ::bind(fd, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr));
    assert(bind_res == 0);

    std::unordered_map<std::string, 
                       std::pair<std::optional<Client>, 
                                 std::optional<Client>>> connection_ids;

    while (true) {
        char buf[4096];
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        ssize_t received = ::recvfrom(fd, buf, sizeof(buf) - 1, 0,
                                      reinterpret_cast<sockaddr*>(&from), &from_len);
        if (received <= 0) continue;
        buf[received] = '\0';

        std::istringstream iss{buf};

        std::string id, local_ip;
        uint16_t local_port = 0;
        if (!(iss >> id >> local_ip >> local_port)) continue;

        char ip[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        uint16_t port = ntohs(from.sin_port);

        Client client{ip, port, local_ip, local_port};
        auto &connection = connection_ids[id];

        if (connection.first && connection.second) {
            send_connection_info(fd, *connection.first, *connection.second);
            continue;
        }

        if (connection.first && !connection.second) {
            const Client &a = *connection.first;
            if (a.ip == client.ip && a.port == client.port) {
                continue;
            }

            connection.second = client;

            send_connection_info(fd, *connection.first, *connection.second);
            continue;
        }

        if (!connection.first && !connection.second) {
            connection.first = client;
            continue;
        }
    }

    ::close(fd);
    return 0;
}
