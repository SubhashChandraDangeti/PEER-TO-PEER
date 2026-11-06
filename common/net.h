// Socket helpers and message framing.
// Frame: [4-byte big-endian length][1-byte type][body], length = 1 + body size.
// TCP doesn't keep message boundaries, so every message carries its length.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

namespace net {

enum class FrameType : uint8_t {
    TEXT  = 1,  // a command or reply, fields separated by '|'
    PIECE = 2,  // raw bytes of one file piece (binary-safe)
};

struct Frame {
    FrameType type = FrameType::TEXT;
    std::string body;  // may contain '\0' (binary pieces)
};

enum class RecvStatus {
    OK,       // a complete, valid frame was read
    CLOSED,   // peer closed the connection cleanly
    TIMEOUT,  // no data within the socket's receive timeout
    ERROR,    // socket error, or a malformed header (bad type / too big)
};

// Loop until all `len` bytes are written / read. Return false on failure.
bool send_all(int fd, const void* buf, size_t len);
// Returns 1 = ok, 0 = peer closed before any byte, -1 = error, -2 = timeout.
int recv_all(int fd, void* buf, size_t len);

bool send_frame(int fd, FrameType type, const std::string& body);
inline bool send_text(int fd, const std::string& body) {
    return send_frame(fd, FrameType::TEXT, body);
}
RecvStatus recv_frame(int fd, Frame& out);

// Address "ip:port". Returns false if the IP or port is invalid.
bool parse_addr(const std::string& s, std::string& ip, int& port);

// Create a listening socket bound to ip:port. Returns fd or -1.
int listen_on(const std::string& ip, int port, int backlog = 64);
// Connect to ip:port. If bind_ip is non-empty, the outgoing socket is bound
// to that local address first. Returns fd or -1.
int connect_to(const std::string& ip, int port, const std::string& bind_ip = "");

// accept() that polls every 200 ms so it can notice `stop`
// (closing the socket doesn't wake accept() on macOS)
int accept_until(int listen_fd, const std::atomic<bool>& stop);

// Receive timeout in milliseconds (0 = wait forever).
void set_recv_timeout(int fd, int ms);

void close_fd(int& fd);  // close and set to -1

}  // namespace net
