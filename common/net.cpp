#include "net.h"
#include "config.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0  // macOS: SIGPIPE is ignored process-wide instead
#endif

namespace net {

bool send_all(int fd, const void* buf, size_t len) {
    const char* p = static_cast<const char*>(buf);
    while (len > 0) {
        ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

int recv_all(int fd, void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, p + got, len - got, 0);
        if (n == 0) return got == 0 ? 0 : -1;  // closed (mid-message = error)
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
            return -1;
        }
        got += static_cast<size_t>(n);
    }
    return 1;
}

bool send_frame(int fd, FrameType type, const std::string& body) {
    if (body.size() > cfg::MAX_FRAME) return false;
    uint32_t len = htonl(static_cast<uint32_t>(body.size() + 1));
    char header[5];
    std::memcpy(header, &len, 4);
    header[4] = static_cast<char>(type);
    // header + body in one send
    std::string out;
    out.reserve(5 + body.size());
    out.append(header, 5);
    out.append(body);
    return send_all(fd, out.data(), out.size());
}

RecvStatus recv_frame(int fd, Frame& out) {
    uint32_t len_be = 0;
    int r = recv_all(fd, &len_be, 4);
    if (r == 0) return RecvStatus::CLOSED;
    if (r == -2) return RecvStatus::TIMEOUT;
    if (r < 0) return RecvStatus::ERROR;

    uint32_t len = ntohl(len_be);
    if (len < 1 || len > cfg::MAX_FRAME + 1) return RecvStatus::ERROR;  // before allocating

    uint8_t type = 0;
    if (recv_all(fd, &type, 1) != 1) return RecvStatus::ERROR;
    if (type != static_cast<uint8_t>(FrameType::TEXT) &&
        type != static_cast<uint8_t>(FrameType::PIECE))
        return RecvStatus::ERROR;

    out.type = static_cast<FrameType>(type);
    out.body.assign(len - 1, '\0');
    if (len > 1) {
        r = recv_all(fd, &out.body[0], len - 1);
        if (r == -2) return RecvStatus::TIMEOUT;
        if (r != 1) return RecvStatus::ERROR;
    }
    return RecvStatus::OK;
}

bool parse_addr(const std::string& s, std::string& ip, int& port) {
    size_t colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0) return false;
    std::string ip_part = s.substr(0, colon);
    std::string port_part = s.substr(colon + 1);
    if (port_part.empty() || port_part.size() > 5) return false;
    for (char c : port_part)
        if (c < '0' || c > '9') return false;
    int p = std::stoi(port_part);  // safe: at most 5 digits, all checked
    if (p < 1 || p > 65535) return false;
    in_addr tmp{};
    if (inet_pton(AF_INET, ip_part.c_str(), &tmp) != 1) return false;
    ip = ip_part;
    port = p;
    return true;
}

static bool make_sockaddr(const std::string& ip, int port, sockaddr_in& a) {
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    return inet_pton(AF_INET, ip.c_str(), &a.sin_addr) == 1;
}

int listen_on(const std::string& ip, int port, int backlog) {
    sockaddr_in a;
    if (!make_sockaddr(ip, port, a)) return -1;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    timeval send_timeout{};
    send_timeout.tv_sec = cfg::SOCKET_TIMEOUT_MS / 1000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    int one = 1;
    // lets a restarted process rebind the port right away
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0 ||
        ::listen(fd, backlog) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

int connect_to(const std::string& ip, int port, const std::string& bind_ip) {
    sockaddr_in a;
    if (!make_sockaddr(ip, port, a)) return -1;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (!bind_ip.empty()) {
        sockaddr_in local;
        if (make_sockaddr(bind_ip, 0, local))
            ::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local));
    }
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(fd);
        return -1;
    }
    int result = ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    if (result < 0 && errno == EINPROGRESS) {
        pollfd p{fd, POLLOUT, 0};
        result = ::poll(&p, 1, cfg::SOCKET_TIMEOUT_MS);
        int error = 0;
        socklen_t len = sizeof(error);
        if (result <= 0 || ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0)
            result = -1;
        else result = 0;
    }
    if (result < 0 || ::fcntl(fd, F_SETFL, flags) < 0) {
        ::close(fd);
        return -1;
    }
    timeval timeout{};
    timeout.tv_sec = cfg::SOCKET_TIMEOUT_MS / 1000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    return fd;
}

int accept_until(int listen_fd, const std::atomic<bool>& stop) {
    while (!stop.load()) {
        pollfd p{listen_fd, POLLIN, 0};
        int r = ::poll(&p, 1, 200);
        if (r <= 0) continue;  // timeout or EINTR: re-check `stop`
        int fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd >= 0) return fd;
    }
    return -1;
}

void set_recv_timeout(int fd, int ms) {
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

}  // namespace net
