#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <sys/select.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <cassert>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <chrono>

namespace {

void fill_ipv4(sockaddr_in& addr, const std::string& ip, uint16_t port) {
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    int parsed = ::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    assert(parsed == 1);
}

void sendto_all(int fd, const sockaddr_in& addr, socklen_t addr_len,
                const char* data, size_t len) {
    size_t total_sent = 0;
    while (total_sent < len) {
        ssize_t sent = ::sendto(fd, data + total_sent, len - total_sent, 0,
                                reinterpret_cast<const sockaddr*>(&addr),
                                addr_len);
        if (sent <= 0 && errno == EINTR) continue;
        assert(sent > 0);
        total_sent += static_cast<size_t>(sent);
    }
}

bool read_stdin(std::string& line_out) {
    if (!std::getline(std::cin, line_out)) return false;
    return true;
}

int create_udp_client_socket(const std::string& my_ip) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;

    int parsed = ::inet_pton(AF_INET, my_ip.c_str(), &addr.sin_addr);
    assert(parsed == 1);

    int bind_res = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    assert(bind_res == 0);

    return fd;
}


inline uint16_t parse_port(const std::string& str) {
    char* end = nullptr;
    long port = std::strtol(str.c_str(), &end, 10);
    assert(end != nullptr && *end == '\0');
    return static_cast<uint16_t>(port);
}

std::pair<std::string, uint16_t>
receive_udp_addresses(int fd,
                      const std::string& rendezvous_ip,
                      uint16_t rendezvous_port,
                      const std::string& connection_id) {
    sockaddr_in rendezvous_addr{};
    fill_ipv4(rendezvous_addr, rendezvous_ip, rendezvous_port);

    sockaddr_in local_addr{};
    socklen_t local_len = sizeof(local_addr);
    int res = ::getsockname(fd, reinterpret_cast<sockaddr*>(&local_addr), &local_len);
    assert(res == 0);

    char local_ip_str[INET_ADDRSTRLEN] = "";
    ::inet_ntop(AF_INET, &local_addr.sin_addr, local_ip_str, sizeof(local_ip_str));
    uint16_t local_port = ntohs(local_addr.sin_port);
    
    assert(local_port != 0);

    std::ostringstream oss;
    oss << connection_id << " " << local_ip_str << " " << local_port << "\n";
    std::string reg_msg = oss.str();

    char buf[4096];
    while (true) {
        sendto_all(fd, rendezvous_addr, sizeof(rendezvous_addr),
                   reg_msg.c_str(), reg_msg.size());

        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        ssize_t received =
            ::recvfrom(fd, buf, sizeof(buf), 0,
                       reinterpret_cast<sockaddr*>(&from), &from_len);
        if (received <= 0) {
            continue;
        }

        std::istringstream iss{std::string{buf, buf + received}};
        std::string peer_ip;
        uint16_t peer_port = 0;
        if (!(iss >> peer_ip >> peer_port)) {
            continue;
        }

        return {peer_ip, peer_port};
    }

    assert(false); // unreachable
}

void run_udp_client(int fd, const std::string& ip, uint16_t port) {
    sockaddr_in receiver_addr{};
    fill_ipv4(receiver_addr, ip, port);
    std::cout << "Connected to server" << std::endl;

    while (true) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        FD_SET(STDIN_FILENO, &readfds);

        int ready = ::select(fd + 1, &readfds, nullptr, nullptr, nullptr);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        assert(ready >= 0);

        if (FD_ISSET(fd, &readfds)) {
            std::cout << "Reading new msg" << std::endl;
            char buffer[4096];
            sockaddr_in from_addr{};
            socklen_t from_len = sizeof(from_addr);

            ssize_t received =
                ::recvfrom(fd, buffer, sizeof(buffer), 0,
                           reinterpret_cast<sockaddr*>(&from_addr), &from_len);
            if (received < 0) continue;

            if (from_addr.sin_addr.s_addr != receiver_addr.sin_addr.s_addr ||
                from_addr.sin_port != receiver_addr.sin_port) {
                std::cout << "Msg not from another client, ignore" << std::endl;
                continue;
            }

            std::string message(buffer, buffer + received);
            std::cout << "New msg: " << message << std::endl;
        }

        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            std::string line;
            if (!read_stdin(line)) break;
            line.push_back('\n');
            sendto_all(fd, receiver_addr, sizeof(receiver_addr),
                       line.c_str(), line.size());
        }
    }
}


void init_udp_punching(int fd, const std::string& ip, uint16_t port) {
    using namespace std::chrono_literals;

    sockaddr_in receiver_addr{};
    fill_ipv4(receiver_addr, ip, port);

    for (size_t i = 0; i < 3; ++i) {
        const char* msg = "init\n";
        sendto_all(fd, receiver_addr, sizeof(receiver_addr), msg, strlen(msg));
        std::this_thread::sleep_for(10ms);
    }
}

void run_udp(const std::string& rendezvous_ip, uint16_t rendezvous_port,
             const std::string& connection_id, const std::string& my_ip) {
    int fd = create_udp_client_socket(my_ip);

    auto [ip, port] =
        receive_udp_addresses(fd, rendezvous_ip, rendezvous_port, connection_id);
    
    std::cout << ip << " " << port << "\n";
    init_udp_punching(fd, ip, port);

    run_udp_client(fd, ip, port);

    ::close(fd);
}

void print_usage(const std::string& prog) {
    std::cout << "Usage:\n"
              << "  " << prog << " <rendezvous_ip> <rendezvous_port> <connection_id> <my_ip>\n";
}

} // namespace anonymous

int main(int argc, char* argv[]) {
    ::signal(SIGPIPE, SIG_IGN);
    if (argc < 5) {
        print_usage(argv[0]);
        return 0;
    }

    const std::string rendezvous_ip = argv[1];
    const uint16_t rendezvous_port = parse_port(argv[2]);
    const std::string connection_id = argv[3];
    const std::string my_ip = argv[4];

    run_udp(rendezvous_ip, rendezvous_port, connection_id, my_ip);
}
