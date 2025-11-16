#include <arpa/inet.h>
#include <cerrno>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <cstdio>

#include <openssl/ssl.h>
#include <openssl/err.h>

namespace {

FILE* ssl_keylog_file = nullptr;

void keylog_callback(const SSL* /*ssl*/, const char* line) {
    if (!ssl_keylog_file) {
        return;
    }
    std::fprintf(ssl_keylog_file, "%s\n", line);
    std::fflush(ssl_keylog_file);
}

void set_keylog_callback(SSL_CTX* ctx) {
    const char* path = std::getenv("SSLKEYLOGFILE");
    if (!path) {
        std::cerr << "No SSLKEYLOGFILE set\n";
        return;
    }

    ssl_keylog_file = std::fopen(path, "a");
    if (!ssl_keylog_file) {
        std::perror("fopen(SSLKEYLOGFILE)");
        assert(false);
        return;
    }

    SSL_CTX_set_keylog_callback(ctx, keylog_callback);
}

SSL_CTX* init_server_ssl_ctx(const std::string& cert_path, const std::string& key_path) {
    const SSL_METHOD* method = TLS_server_method();
    SSL_CTX* ctx = SSL_CTX_new(method);
    assert(ctx);

    int ok = SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    assert(ok);

    ok = SSL_CTX_use_certificate_file(ctx, cert_path.c_str(), SSL_FILETYPE_PEM);
    assert(ok);

    ok = SSL_CTX_use_PrivateKey_file(ctx, key_path.c_str(), SSL_FILETYPE_PEM);
    assert(ok);

    ok = SSL_CTX_check_private_key(ctx);
    assert(ok);

    return ctx;
}

SSL_CTX* init_client_ssl_ctx() {
    const SSL_METHOD* method = TLS_client_method();
    SSL_CTX* ctx = SSL_CTX_new(method);
    assert(ctx);

    int ok = SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    assert(ok);

    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    set_keylog_callback(ctx);
    return ctx;
}

void ssl_send_all(SSL* ssl, const char* data, size_t len) {
    size_t total_sent = 0;
    while (total_sent < len) {
        int n_sent = SSL_write(ssl, data + total_sent, static_cast<int>(len - total_sent));
        if (n_sent <= 0) {
            int err = SSL_get_error(ssl, n_sent);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            if (err == SSL_ERROR_SYSCALL && errno == EINTR) {
                continue;
            }
            ERR_print_errors_fp(stderr);
            assert(false);
        }
        total_sent += static_cast<size_t>(n_sent);
    }
}

bool ssl_read_until_enter(SSL* ssl, std::string& message) {
    message.clear();
    while (true) {
        char ch = 0;
        int n_read = SSL_read(ssl, &ch, 1);
        if (n_read <= 0) {
            int err = SSL_get_error(ssl, n_read);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            if (err == SSL_ERROR_SYSCALL && errno == EINTR) {
                continue;
            }

            return false;
        }

        if (ch == '\n') {
            return true;
        }
        message.push_back(ch);
    }
}

void fill_ipv4(sockaddr_in& addr, const std::string& ip, uint16_t port) {
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    int parsed = ::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    assert(parsed == 1);
}

void sendto_all(int fd, const sockaddr_in& addr, socklen_t addr_len, const char* data, size_t len) {
    size_t total_sent = 0;
    while (total_sent < len) {
        ssize_t sent = ::sendto(
            fd,
            data + total_sent,
            len - total_sent,
            0,
            reinterpret_cast<const sockaddr*>(&addr),
            addr_len);
        if (sent <= 0 && errno == EINTR) {
            continue;
        }
        assert(sent > 0);
        total_sent += static_cast<size_t>(sent);
    }
}

int create_tcp_server_socket(const std::string& ip, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);

    int opt = 1;
    int reuse = ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    assert(reuse == 0);

    sockaddr_in addr{};
    fill_ipv4(addr, ip, port);

    int bound = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    assert(bound == 0);

    int listen_res = ::listen(fd, 1);
    assert(listen_res == 0);
    return fd;
}

int connect_tcp_client(const std::string& ip, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);

    sockaddr_in addr{};
    fill_ipv4(addr, ip, port);

    int ret = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    assert(ret == 0);
    return fd;
}

int create_udp_server_socket(const std::string& ip, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);

    int opt = 1;
    int reuse = ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    assert(reuse == 0);

    sockaddr_in addr{};
    fill_ipv4(addr, ip, port);
    int bound = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    assert(bound == 0);

    return fd;
}

int create_udp_client_socket() {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    return fd;
}

bool read_stdin(std::string& line_out) {
    if (!std::getline(std::cin, line_out)) {
        return false;
    }
    return true;
}

void run_tcp_connection_tls(SSL* ssl) {
    const int sock_fd = SSL_get_fd(ssl);
    assert(sock_fd >= 0);

    while (true) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock_fd, &readfds);
        FD_SET(STDIN_FILENO, &readfds);

        int ready = ::select(sock_fd + 1, &readfds, nullptr, nullptr, nullptr);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        assert(ready >= 0);

        if (FD_ISSET(sock_fd, &readfds)) {
            std::cout << "Reading new msg" << std::endl;
            std::string message;
            if (!ssl_read_until_enter(ssl, message)) {
                std::cout << "Client disconnected" << std::endl;
                break;
            }
            std::cout << "New msg: " << message << std::endl;
        }

        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            std::string line;
            if (!read_stdin(line)) {
                break;
            }
            line.push_back('\n');
            ssl_send_all(ssl, line.c_str(), line.size());
        }
    }
}

void run_tcp_server(const std::string& ip, uint16_t port,
                    const std::string& cert_path, const std::string& key_path) {
    SSL_CTX* ctx = init_server_ssl_ctx(cert_path, key_path);

    int listen_fd = create_tcp_server_socket(ip, port);
    std::cout << "Waiting for client on TCP " << ip << ":" << port << std::endl;

    while (true) {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (client_fd < 0 && errno == EINTR) {
            continue;
        }
        assert(client_fd >= 0);

        std::cout << "Client connected" << std::endl;

        SSL* ssl = SSL_new(ctx);
        assert(ssl);
        SSL_set_fd(ssl, client_fd);

        int ret = SSL_accept(ssl);
        if (ret <= 0) {
            std::cerr << "Err in ssl accept" << std::endl;
            SSL_get_error(ssl, ret);
            ERR_print_errors_fp(stderr);
            SSL_free(ssl);
            ::close(client_fd);
            continue;
        }

        run_tcp_connection_tls(ssl);

        SSL_shutdown(ssl);
        SSL_free(ssl);
        ::close(client_fd);

        if (!std::cin.good()) {
            break;
        }
        std::cout << "Waiting for client on TCP " << ip << ":" << port << std::endl;
    }

    ::close(listen_fd);
    SSL_CTX_free(ctx);

    if (ssl_keylog_file) {
        std::fclose(ssl_keylog_file);
        ssl_keylog_file = nullptr;
    }
}

void run_tcp_client(const std::string& ip, uint16_t port) {
    int fd = connect_tcp_client(ip, port);
    std::cout << "Connected to server" << std::endl;

    SSL_CTX* ctx = init_client_ssl_ctx();
    SSL* ssl = SSL_new(ctx);
    assert(ssl);

    SSL_set_fd(ssl, fd);

    int ret = SSL_connect(ssl);
    if (ret <= 0) {
        SSL_get_error(ssl, ret);
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        ::close(fd);
        return;
    }

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
            std::string message;
            if (!ssl_read_until_enter(ssl, message)) {
                std::cout << "Disconnected from server" << std::endl;
                break;
            }
            std::cout << "New msg: " << message << std::endl;
        }

        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            std::string line;
            if (!read_stdin(line)) {
                break;
            }
            line.push_back('\n');
            ssl_send_all(ssl, line.c_str(), line.size());
        }
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    ::close(fd);
}

void run_udp_server(const std::string& ip, uint16_t port) {
    int fd = create_udp_server_socket(ip, port);
    std::cout << "UDP server listening on " << ip << ":" << port << std::endl;

    bool has_client = false;
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);

    while (true) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        FD_SET(STDIN_FILENO, &readfds);

        int maxfd = (fd > STDIN_FILENO ? fd : STDIN_FILENO) + 1;

        int ready = ::select(maxfd, &readfds, nullptr, nullptr, nullptr);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        assert(ready >= 0);

        if (FD_ISSET(fd, &readfds)) {
            std::cout << "Reading new msg" << std::endl;
            char buffer[4096];
            sockaddr_in from_addr{};
            socklen_t from_len = sizeof(from_addr);
            ssize_t received = ::recvfrom(
                fd,
                buffer,
                sizeof(buffer),
                0,
                reinterpret_cast<sockaddr*>(&from_addr),
                &from_len);
            if (received < 0) {
                continue;
            }

            std::string message(buffer, buffer + received);

            char addr_str[INET_ADDRSTRLEN] = {0};
            const char* ip_str = ::inet_ntop(AF_INET, &from_addr.sin_addr, addr_str, sizeof(addr_str));
            assert(ip_str != nullptr);
            uint16_t sender_port = ntohs(from_addr.sin_port);
            std::cout << "New msg from " << ip_str << ":" << sender_port << ": " << message << std::endl;

            client_addr = from_addr;
            client_len = from_len;
            has_client = true;
        }

        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            std::string line;
            if (!read_stdin(line)) {
                break;
            }
            if (!has_client) {
                std::cout << "No client to send to yet" << std::endl;
                continue;
            }
            line.push_back('\n');
            sendto_all(fd, client_addr, client_len, line.c_str(), line.size());
        }
    }

    ::close(fd);
}

void run_udp_client(const std::string& ip, uint16_t port) {
    int fd = create_udp_client_socket();
    sockaddr_in server_addr{};
    fill_ipv4(server_addr, ip, port);
    socklen_t server_len = sizeof(server_addr);
    std::cout << "Connected to server" << std::endl;

    while (true) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        FD_SET(STDIN_FILENO, &readfds);

        int maxfd = (fd > STDIN_FILENO ? fd : STDIN_FILENO) + 1;

        int ready = ::select(maxfd, &readfds, nullptr, nullptr, nullptr);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        assert(ready >= 0);

        if (FD_ISSET(fd, &readfds)) {
            std::cout << "Reading new msg" << std::endl;
            char buffer[4096];
            sockaddr_in from_addr{};
            socklen_t from_len = sizeof(from_addr);

            ssize_t received = ::recvfrom(
                fd,
                buffer,
                sizeof(buffer),
                0,
                reinterpret_cast<sockaddr*>(&from_addr),
                &from_len);
            if (received < 0) {
                continue;
            }

            if (from_addr.sin_addr.s_addr != server_addr.sin_addr.s_addr ||
                from_addr.sin_port != server_addr.sin_port) {
                std::cout << "Msg not from server, ignore" << std::endl;
                continue;
            }

            std::string message(buffer, buffer + received);
            std::cout << "New msg: " << message << std::endl;
        }

        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            std::string line;
            if (!read_stdin(line)) {
                break;
            }
            line.push_back('\n');
            sendto_all(fd, server_addr, server_len, line.c_str(), line.size());
        }
    }

    ::close(fd);
}

// ======================= Общие утилиты =======================

inline uint16_t parse_port(const std::string& str) {
    char* end = nullptr;
    long port = std::strtol(str.c_str(), &end, 10);
    assert(end != nullptr && *end == '\0');
    return static_cast<uint16_t>(port);
}

void print_usage(const std::string& prog) {
    std::cout << "Usage:\n"
              << "  " << prog << " tcp server <ip> <port> <cert.pem> <key.pem>\n"
              << "  " << prog << " tcp client <ip> <port>\n"
              << "  " << prog << " udp server <ip> <port>\n"
              << "  " << prog << " udp client <ip> <port>\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    ::signal(SIGPIPE, SIG_IGN);

    if (argc < 5) {
        print_usage(argv[0]);
        return 0;
    }

    const std::string protocol = argv[1];
    const std::string role = argv[2];
    const std::string ip = argv[3];
    const uint16_t port = parse_port(argv[4]);

    if (protocol == "tcp" && role == "server") {
        assert(argc >= 7);
        std::string cert_path = argv[5];
        std::string key_path  = argv[6];

        run_tcp_server(ip, port, cert_path, key_path);

    } else if (protocol == "tcp" && role == "client") {
        run_tcp_client(ip, port);
    } else if (protocol == "udp" && role == "server") {
        run_udp_server(ip, port);
    } else if (protocol == "udp" && role == "client") {
        run_udp_client(ip, port);
    } else {
        std::printf("Incorrect protocol or role\n");
        print_usage(argv[0]);
        return -1;
    }

    return 0;
}
