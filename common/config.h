// Fixed parameters. All sizes are 64-bit since file offsets can pass 2 GiB.
#pragma once
#include <cstdint>
#include <cstddef>

namespace cfg {

// 512 KB pieces (assignment spec); last piece may be shorter
constexpr uint64_t PIECE_SIZE = 512ULL * 1024;

// largest file accepted (4 GiB)
constexpr uint64_t MAX_FILE_SIZE = 4ULL * 1024 * 1024 * 1024;

// max frame body, checked before allocating; fits a 4 GiB file's hash list
constexpr uint32_t MAX_FRAME = 1024 * 1024;

// user/group/file names: 1..64 chars of [A-Za-z0-9._-]
constexpr size_t MAX_NAME_LEN = 64;

// per client: 3 downloads at once, 4 workers each
constexpr int MAX_ACTIVE_DOWNLOADS = 3;
constexpr int WORKERS_PER_DOWNLOAD = 4;

// tries per piece before giving up
constexpr int PIECE_MAX_ATTEMPTS = 6;

// peer server upload threads
constexpr int UPLOAD_THREADS = 8;

// Bound connection setup and stalled peer reads/writes.
constexpr int SOCKET_TIMEOUT_MS = 5000;

// tracker heartbeat: every 2 s, partner considered dead after 6 s
constexpr int HEARTBEAT_MS = 2000;
constexpr int PEER_DEAD_MS = 6000;

// how long a starting tracker looks for its partner before going primary alone
constexpr int STARTUP_WAIT_MS = 4000;
constexpr int RECONNECT_MS = 1000;

// password salt
constexpr size_t SALT_BYTES = 16;
// session token (256 bits)
constexpr size_t TOKEN_BYTES = 32;

}  // namespace cfg
