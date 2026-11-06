#include "tracker_client.h"
#include "log.h"
#include "net.h"
#include "proto.h"
#include "util.h"

#include <unistd.h>

// reply timeout; longer than the primary's 6 s wait for its backup.
// Catches a frozen tracker, which keeps the socket open but never answers.
static constexpr int REPLY_TIMEOUT_MS = 10000;

TrackerClient::TrackerClient(std::vector<TrackerAddr> trackers, std::string my_ip, std::string my_addr)
    : trackers_(std::move(trackers)), my_ip_(std::move(my_ip)), my_addr_(std::move(my_addr)) {}

TrackerClient::~TrackerClient() { close(); }

void TrackerClient::close() {
    std::lock_guard<std::mutex> lk(mu_);
    net::close_fd(fd_);
}

void TrackerClient::set_session(const std::string& user, const std::string& token) {
    std::lock_guard<std::mutex> lk(mu_);
    user_ = user;
    token_ = token;
}

void TrackerClient::clear_session() {
    std::lock_guard<std::mutex> lk(mu_);
    user_.clear();
    token_.clear();
}

std::string TrackerClient::user() {
    std::lock_guard<std::mutex> lk(mu_);
    return user_;
}

// Try `preferred` first (from a NOT_PRIMARY redirect), then every tracker.
bool TrackerClient::connect_any(const std::string& preferred) {
    net::close_fd(fd_);
    // redirect target first, then file order, with the tracker that just
    // failed last (a frozen tracker still accepts connections)
    std::vector<std::string> order;
    if (!preferred.empty()) order.push_back(preferred);
    for (const auto& t : trackers_)
        if (t.client_addr() != last_failed_) order.push_back(t.client_addr());
    if (!last_failed_.empty()) order.push_back(last_failed_);
    for (const auto& a : order) {
        std::string ip;
        int port = 0;
        if (!net::parse_addr(a, ip, port)) continue;
        int fd = net::connect_to(ip, port, my_ip_);
        if (fd >= 0) {
            if (a == last_failed_ && order.size() > 1) last_failed_.clear();
            net::set_recv_timeout(fd, REPLY_TIMEOUT_MS);
            fd_ = fd;
            connected_to_ = a;
            return true;
        }
    }
    return false;
}

bool TrackerClient::exchange(const std::string& body, std::string& reply) {
    if (fd_ < 0) return false;
    net::Frame f;
    if (!net::send_text(fd_, body) || net::recv_frame(fd_, f) != net::RecvStatus::OK) {
        last_failed_ = connected_to_;
        net::close_fd(fd_);  // broken or silent: never reuse a desynced stream
        return false;
    }
    reply = f.body;
    return true;
}

// Re-establish the session on whichever tracker we are now connected to.
bool TrackerClient::resume_locked(std::string& redirect) {
    if (user_.empty()) return true;  // nothing to resume
    std::string r;
    if (!exchange("RESUME|" + user_ + "|" + token_ + "|" + my_addr_, r)) return false;
    if (r.rfind(proto::OK, 0) == 0) return true;
    if (r.rfind(proto::NOT_PRIMARY, 0) == 0) {
        std::vector<std::string> p = util::split(r, '|');
        redirect = p.size() >= 3 ? p[2] : "";
        return false;
    }
    // new primary doesn't know the session (e.g. made on the losing side of a split)
    logx::warn("session could not be resumed: %s", r.c_str());
    user_.clear();
    token_.clear();
    return true;
}

std::string TrackerClient::request(const std::string& body) {
    bool resumed = false;
    std::string reply;
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::string redirect;
        // connect -> resume -> send; on failure try the next tracker,
        // on NOT_PRIMARY go to the address given
        for (int attempt = 0; attempt < 6; ++attempt) {
            if (fd_ < 0) {
                if (!connect_any(redirect)) {
                    usleep(500 * 1000);  // both down or restarting: brief pause
                    continue;
                }
                // Fresh connection: carry the session over (no-op if logged out).
                if (!resume_locked(redirect)) {
                    net::close_fd(fd_);
                    continue;
                }
                resumed = !user_.empty();
            }
            if (!exchange(body, reply)) {
                logx::warn("tracker %s not responding, failing over", connected_to_.c_str());
                redirect.clear();
                continue;
            }
            if (reply.rfind(proto::NOT_PRIMARY, 0) == 0) {
                std::vector<std::string> p = util::split(reply, '|');
                redirect = p.size() >= 3 ? p[2] : "";
                net::close_fd(fd_);
                continue;
            }
            break;
        }
        if (reply.empty()) reply = proto::reply(proto::INVALID_ACTION, "no tracker reachable");
    }
    if (resumed && on_resumed_) on_resumed_();  // outside the lock: it calls request()
    return reply;
}
