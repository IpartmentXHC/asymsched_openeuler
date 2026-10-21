#include "asymsched/api.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <map>
#include <netdb.h>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace asymsched {
namespace {

constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
constexpr std::size_t kMaxBodyBytes = 32 * 1024 * 1024;

volatile std::sig_atomic_t stop_requested = 0;

void stop_handler(int) {
    stop_requested = 1;
}

std::string lower(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool send_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t result =
            ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

const char* status_text(int status) {
    switch (status) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 413:
            return "Payload Too Large";
        default:
            return "Internal Server Error";
    }
}

HttpResponse simple_error(int status, const std::string& message) {
    return HttpResponse{status, "application/json; charset=utf-8",
                        "{\"error\":\"" + message + "\"}"};
}

bool read_request(int fd, HttpRequest& request, HttpResponse& error) {
    std::string buffer;
    char chunk[8192];
    std::size_t header_end = std::string::npos;

    while (header_end == std::string::npos) {
        const ssize_t received = ::recv(fd, chunk, sizeof(chunk), 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (received == 0) {
            return false;
        }
        buffer.append(chunk, static_cast<std::size_t>(received));
        header_end = buffer.find("\r\n\r\n");
        if (header_end == std::string::npos &&
            buffer.size() > kMaxHeaderBytes) {
            error = simple_error(413, "request headers too large");
            return false;
        }
    }

    const std::string header = buffer.substr(0, header_end);
    std::istringstream stream(header);
    std::string request_line;
    if (!std::getline(stream, request_line) || request_line.empty()) {
        error = simple_error(400, "malformed request line");
        return false;
    }
    if (!request_line.empty() && request_line.back() == '\r') {
        request_line.pop_back();
    }
    std::istringstream line(request_line);
    std::string version;
    if (!(line >> request.method >> request.path >> version) ||
        version.rfind("HTTP/", 0) != 0) {
        error = simple_error(400, "malformed request line");
        return false;
    }

    std::map<std::string, std::string> headers;
    std::string header_line;
    while (std::getline(stream, header_line)) {
        if (!header_line.empty() && header_line.back() == '\r') {
            header_line.pop_back();
        }
        if (header_line.empty()) {
            continue;
        }
        const std::size_t colon = header_line.find(':');
        if (colon == std::string::npos) {
            error = simple_error(400, "malformed header");
            return false;
        }
        headers[lower(trim(header_line.substr(0, colon)))] =
            trim(header_line.substr(colon + 1));
    }

    if (headers.find("transfer-encoding") != headers.end()) {
        error = simple_error(400, "transfer-encoding is not supported");
        return false;
    }

    std::size_t content_length = 0;
    const auto length_it = headers.find("content-length");
    if (length_it != headers.end()) {
        try {
            std::size_t consumed = 0;
            content_length = std::stoull(length_it->second, &consumed);
            if (consumed != length_it->second.size()) {
                throw std::invalid_argument("invalid content-length");
            }
        } catch (const std::exception&) {
            error = simple_error(400, "invalid content-length");
            return false;
        }
    }
    if (content_length > kMaxBodyBytes) {
        error = simple_error(413, "request body too large");
        return false;
    }

    const std::size_t body_start = header_end + 4;
    request.body = buffer.substr(body_start);
    while (request.body.size() < content_length) {
        const ssize_t received = ::recv(fd, chunk, sizeof(chunk), 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (received == 0) {
            error = simple_error(400, "truncated request body");
            return false;
        }
        request.body.append(chunk, static_cast<std::size_t>(received));
        if (request.body.size() > kMaxBodyBytes) {
            error = simple_error(413, "request body too large");
            return false;
        }
    }
    request.body.resize(content_length);
    return true;
}

void handle_connection(int fd) {
    HttpRequest request;
    HttpResponse error;
    if (!read_request(fd, request, error)) {
        if (!error.body.empty()) {
            const std::string response =
                "HTTP/1.1 " + std::to_string(error.status) + " " +
                status_text(error.status) +
                "\r\nContent-Type: " + error.content_type +
                "\r\nContent-Length: " + std::to_string(error.body.size()) +
                "\r\nConnection: close\r\n\r\n" + error.body;
            send_all(fd, response);
        }
        return;
    }

    const HttpResponse response = handle_http(request);
    const std::string output =
        "HTTP/1.1 " + std::to_string(response.status) + " " +
        status_text(response.status) + "\r\nContent-Type: " +
        response.content_type + "\r\nContent-Length: " +
        std::to_string(response.body.size()) +
        "\r\nConnection: close\r\n\r\n" + response.body;
    send_all(fd, output);
}

std::uint16_t bound_port(int fd) {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return 0;
    }
    if (address.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port);
    }
    if (address.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
    }
    return 0;
}

}  // namespace

int run_server(const std::string& host, std::uint16_t port) {
    struct sigaction ignore_action {};
    ignore_action.sa_handler = SIG_IGN;
    sigemptyset(&ignore_action.sa_mask);
    ignore_action.sa_flags = 0;
    sigaction(SIGPIPE, &ignore_action, nullptr);

    struct sigaction stop_action {};
    stop_action.sa_handler = stop_handler;
    sigemptyset(&stop_action.sa_mask);
    // Do not set SA_RESTART: accept() must return EINTR so the loop can exit.
    stop_action.sa_flags = 0;
    sigaction(SIGINT, &stop_action, nullptr);
    sigaction(SIGTERM, &stop_action, nullptr);
    stop_requested = 0;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* addresses = nullptr;
    const std::string service = std::to_string(port);
    const char* node = host.empty() ? nullptr : host.c_str();
    const int lookup = getaddrinfo(node, service.c_str(), &hints, &addresses);
    if (lookup != 0) {
        throw std::runtime_error(std::string("getaddrinfo failed: ") +
                                 gai_strerror(lookup));
    }

    int listener = -1;
    for (addrinfo* address = addresses; address != nullptr;
         address = address->ai_next) {
        listener = socket(address->ai_family, address->ai_socktype,
                          address->ai_protocol);
        if (listener < 0) {
            continue;
        }
        int enabled = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled,
                   sizeof(enabled));
        if (bind(listener, address->ai_addr, address->ai_addrlen) == 0 &&
            listen(listener, 64) == 0) {
            break;
        }
        close(listener);
        listener = -1;
    }
    freeaddrinfo(addresses);
    if (listener < 0) {
        throw std::runtime_error(std::string("cannot bind server socket: ") +
                                 std::strerror(errno));
    }

    const std::uint16_t actual_port = bound_port(listener);
    std::cerr << "asymsched-server listening on " << host << ":"
              << actual_port << std::endl;

    while (stop_requested == 0) {
        sockaddr_storage peer{};
        socklen_t peer_length = sizeof(peer);
        const int connection =
            accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_length);
        if (connection < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "accept failed: " << std::strerror(errno)
                      << std::endl;
            continue;
        }
        handle_connection(connection);
        close(connection);
    }

    close(listener);
    return 0;
}

}  // namespace asymsched
