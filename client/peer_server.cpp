#include "peer_server.h"
#include "config.h"
#include "log.h"
#include "net.h"
#include "proto.h"
#include "util.h"

#include <algorithm>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

PeerServer::PeerServer(SharedFiles& files, bool corrupt_mode) : files_(files), corrupt_(corrupt_mode) {}

bool PeerServer::start(const std::string& ip, int port) {
    listen_fd_ = net::listen_on(ip, port);
    if (listen_fd_ < 0) return false;
    acceptor_ = std::thread(&PeerServer::accept_loop, this);
    for (int i = 0; i < cfg::UPLOAD_THREADS; ++i) workers_.emplace_back(&PeerServer::worker, this);
    return true;
}

void PeerServer::stop() {
    stop_.store(true);
    if (acceptor_.joinable()) acceptor_.join();  // wakes within 200 ms
    net::close_fd(listen_fd_);
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (int fd : active_) ::shutdown(fd, SHUT_RDWR);
        for (int fd : queue_) ::close(fd);
        queue_.clear();
    }
    cv_.notify_all();
    for (auto& w : workers_) w.join();
    workers_.clear();
}

void PeerServer::accept_loop() {
    while (!stop_.load()) {
        int fd = net::accept_until(listen_fd_, stop_);
        if (fd < 0) break;  // stopping
        net::set_recv_timeout(fd, cfg::SOCKET_TIMEOUT_MS);
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(fd);
        cv_.notify_one();
    }
}

void PeerServer::worker() {
    while (true) {
        int fd;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stop_.load() || !queue_.empty(); });
            if (stop_.load()) return;
            fd = queue_.front();
            queue_.pop_front();
            active_.push_back(fd);
        }
        serve(fd);
        {
            std::lock_guard<std::mutex> lk(mu_);
            active_.erase(std::find(active_.begin(), active_.end(), fd));
        }
        ::close(fd);
    }
}

void PeerServer::serve(int fd) {
    net::Frame req;
    std::string buf;
    while (!stop_.load() && net::recv_frame(fd, req) == net::RecvStatus::OK) {
        std::vector<std::string> f = util::split(req.body, '|');
        uint64_t index = 0;
        if (req.type != net::FrameType::TEXT || f.size() != 4 || f[0] != "GET_PIECE" ||
            !util::valid_name(f[1]) || !util::valid_name(f[2]) || !util::parse_u64(f[3], index)) {
            net::send_text(fd, proto::reply(proto::INVALID_ACTION, "bad piece request"));
            return;
        }
        std::string path;
        if (!files_.lookup(f[1], f[2], path)) {
            if (!net::send_text(fd, proto::reply(proto::NOT_FOUND, "not sharing that file"))) return;
            continue;
        }
        int file = ::open(path.c_str(), O_RDONLY);
        struct stat st {};
        if (file < 0 || ::fstat(file, &st) != 0 || !S_ISREG(st.st_mode)) {
            if (file >= 0) ::close(file);
            if (!net::send_text(fd, proto::reply(proto::NOT_FOUND, "file no longer readable"))) return;
            continue;
        }
        uint64_t size = static_cast<uint64_t>(st.st_size);
        uint64_t len = util::piece_length(size, index);
        if (len == 0) {
            ::close(file);
            if (!net::send_text(fd, proto::reply(proto::INVALID_ACTION, "piece index out of range"))) return;
            continue;
        }
        // 64-bit offset (files can be > 2 GiB)
        buf.resize(len);
        uint64_t got = 0;
        while (got < len) {
            ssize_t n = ::pread(file, &buf[got], len - got, static_cast<off_t>(index * cfg::PIECE_SIZE + got));
            if (n <= 0) break;
            got += static_cast<uint64_t>(n);
        }
        ::close(file);
        if (got != len) {
            if (!net::send_text(fd, proto::reply(proto::NOT_FOUND, "read error"))) return;
            continue;
        }
        if (corrupt_) buf[len / 2] ^= 0x5A;  // --corrupt
        if (!net::send_frame(fd, net::FrameType::PIECE, buf)) return;
    }
}
