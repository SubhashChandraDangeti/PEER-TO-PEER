// Primary/backup replication between the two trackers.
//
// One TCP link: tracker 2 connects to tracker 1's sync port.
// Messages: HELLO, SNAP_REQ, SNAP_BEGIN/SNAP/SNAP_END, OP/ACK, HB,
// CHECK_REQ/CHECK_RESP.
//
// Roles come from both HELLOs (same rule on both sides):
//   one PRIMARY -> it stays primary; both STARTING -> tracker 1;
//   both PRIMARY (after a split) -> empty tracker loses, else tracker 1 wins.
// The loser clears its state and loads a snapshot.
// Link closed, or nothing received for PEER_DEAD_MS -> backup promotes itself.
#include "replication.h"
#include "config.h"
#include "log.h"
#include "net.h"
#include "tracker.h"
#include "util.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using Clock = std::chrono::steady_clock;

struct Replicator::Impl {
    explicit Impl(Tracker& tr) : t(tr) {}
    Tracker& t;
    std::atomic<bool> stop{false};
    int listen_fd = -1;  // tracker 1 only

    // link_mu guards link_fd and every send on it; the fd is closed under it
    // so nobody sends on a closed (possibly reused) fd.
    std::mutex link_mu;
    int link_fd = -1;

    bool backup_attached = false;  // guarded by t.mu()

    std::mutex ack_mu;  // ACK counting for synchronous replication
    std::condition_variable ack_cv;
    uint64_t acks = 0;
    uint64_t link_gen = 0;  // bumped when a link dies: wakes waiting replicate()

    std::mutex chk_mu;  // console check_sync waits here for CHECK_RESP
    std::condition_variable chk_cv;
    bool chk_have = false;
    std::string chk_resp;
    bool auto_check = false;  // after loading a snapshot, verify automatically

    std::mutex resp_mu;  // CHECK_REQ is answered on its own thread
    std::condition_variable resp_cv;
    bool resp_pending = false;

    std::thread manager, heartbeat, responder;

    void respond() {
        while (true) {
            {
                std::unique_lock<std::mutex> lk(resp_mu);
                resp_cv.wait(lk, [&] { return stop.load() || resp_pending; });
                if (stop.load()) return;
                resp_pending = false;
            }
            std::string reply;
            {
                std::lock_guard<std::mutex> lk(t.mu());
                reply = "CHECK_RESP|" + digest_text() + "|" + t.state().summary();
            }
            send(reply);
        }
    }

    // Send on the current link (false if there is none).
    bool send(const std::string& body) {
        std::lock_guard<std::mutex> lk(link_mu);
        return link_fd >= 0 && net::send_text(link_fd, body);
    }

    bool has_state() { return !t.state().users.empty(); }

    std::string digest_text() {  // "users=..,sessions=..", t.mu() held
        std::string s;
        for (const auto& d : t.state().digests()) s += (s.empty() ? "" : ",") + d.first + "=" + d.second;
        return s;
    }

    static std::map<std::string, std::string> parse_digests(const std::string& s) {
        std::map<std::string, std::string> m;
        for (const auto& kv : util::split(s, ',')) {
            size_t eq = kv.find('=');
            if (eq != std::string::npos) m[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
        return m;
    }

    // Compare our digests with the partner's: "" if equal, else the sections.
    static std::string diff(const std::string& mine, const std::string& theirs) {
        auto a = parse_digests(mine), b = parse_digests(theirs);
        std::string out;
        for (const auto& kv : a)
            if (b[kv.first] != kv.second) out += (out.empty() ? "" : ", ") + kv.first;
        return out;
    }

    // --- link setup

    void check_startup_timeout(Clock::time_point start) {
        if (t.role() == Role::STARTING &&
            Clock::now() - start > std::chrono::milliseconds(cfg::STARTUP_WAIT_MS)) {
            logx::info("partner tracker %d not reachable; starting as primary on my own", t.partner_id());
            t.set_role(Role::PRIMARY);
        }
    }

    // Tracker 1 waits for tracker 2 to connect; tracker 2 keeps dialing.
    int obtain_link(Clock::time_point start) {
        while (!stop.load()) {
            check_startup_timeout(start);
            if (t.id() == 1) {
                pollfd p{listen_fd, POLLIN, 0};
                if (::poll(&p, 1, 200) > 0) {
                    int fd = ::accept(listen_fd, nullptr, nullptr);
                    if (fd >= 0) return fd;
                }
            } else {
                const TrackerAddr& peer = t.partner();
                int fd = net::connect_to(peer.ip, peer.sync_port, t.self().ip);
                if (fd >= 0) return fd;
                for (int i = 0; i < cfg::RECONNECT_MS / 200 && !stop.load(); ++i) {
                    usleep(200 * 1000);
                    check_startup_timeout(start);
                }
            }
        }
        return -1;
    }

    // Exchange HELLO and decide who is primary. Returns false on a bad peer.
    bool handshake(int fd) {
        std::string my_role, my_has;
        {
            std::lock_guard<std::mutex> lk(t.mu());
            my_role = role_name(t.role());
            my_has = has_state() ? "1" : "0";
        }
        if (!send("HELLO|" + std::to_string(t.id()) + "|" + my_role + "|" + my_has)) return false;
        net::Frame f;
        if (net::recv_frame(fd, f) != net::RecvStatus::OK) return false;
        std::vector<std::string> h = util::split(f.body, '|');
        if (h.size() != 4 || h[0] != "HELLO" || h[1] != std::to_string(t.partner_id())) {
            logx::warn("rejected sync connection: not a valid HELLO from tracker %d", t.partner_id());
            return false;
        }
        bool me_p = my_role == "PRIMARY", peer_p = h[2] == "PRIMARY";
        bool me_has = my_has == "1", peer_has = h[3] == "1";
        int winner;
        if (me_p != peer_p) winner = me_p ? t.id() : t.partner_id();
        else if (me_p && me_has != peer_has) winner = me_has ? t.id() : t.partner_id();  // empty never wins
        else winner = 1;  // otherwise tracker 1

        logx::info("link up with tracker %d (me %s, it %s) -> tracker %d is primary", t.partner_id(),
                   my_role.c_str(), h[2].c_str(), winner);
        if (winner == t.id()) {
            t.set_role(Role::PRIMARY);  // the backup will ask for a snapshot
            return true;
        }
        if (me_p) {
            std::lock_guard<std::mutex> lk(t.mu());
            logx::warn("SPLIT-BRAIN resolved: stepping down; discarding my state (%s) "
                       "and copying tracker %d's", t.state().summary().c_str(), winner);
        }
        t.set_role(Role::STARTING);  // refuse clients (NOT_PRIMARY) until loaded
        return send("SNAP_REQ");
    }

    // --- link loop

    void reader_loop(int fd) {
        net::Frame f;
        while (!stop.load()) {
            net::RecvStatus st = net::recv_frame(fd, f);
            if (st == net::RecvStatus::TIMEOUT) {
                logx::warn("no heartbeat from tracker %d for %d ms: treating it as down", t.partner_id(),
                           cfg::PEER_DEAD_MS);
                return;
            }
            if (st != net::RecvStatus::OK) {
                logx::warn("link to tracker %d closed", t.partner_id());
                return;
            }
            const std::string& b = f.body;
            if (b == "HB") continue;
            if (b == "ACK") {
                std::lock_guard<std::mutex> lk(ack_mu);
                ++acks;
                ack_cv.notify_all();
            } else if (b.rfind("OP|", 0) == 0) {  // backup: apply one change
                {
                    std::lock_guard<std::mutex> lk(t.mu());
                    t.state().apply(b.substr(3));
                }
                if (!send("ACK")) return;
            } else if (b == "SNAP_REQ") {  // primary: send everything, then attach
                std::lock_guard<std::mutex> lk(t.mu());
                std::vector<std::string> ops = t.state().snapshot_ops();
                bool ok = send("SNAP_BEGIN");
                for (size_t i = 0; ok && i < ops.size(); ++i) ok = send("SNAP|" + ops[i]);
                ok = ok && send("SNAP_END");
                if (!ok) return;
                // sent under t.mu() so no op slips between snapshot and first OP
                backup_attached = true;
                logx::info("snapshot sent (%zu ops); tracker %d is now my backup", ops.size(), t.partner_id());
            } else if (b == "SNAP_BEGIN") {
                std::lock_guard<std::mutex> lk(t.mu());
                t.state().clear();
            } else if (b.rfind("SNAP|", 0) == 0) {
                std::lock_guard<std::mutex> lk(t.mu());
                t.state().apply(b.substr(5));
            } else if (b == "SNAP_END") {
                std::string summary;
                {
                    std::lock_guard<std::mutex> lk(t.mu());
                    summary = t.state().summary();
                }
                t.set_role(Role::BACKUP);
                logx::info("snapshot loaded (%s); verifying with check_sync", summary.c_str());
                auto_check = true;
                if (!send("CHECK_REQ")) return;
            } else if (b == "CHECK_REQ") {
                // handled by the responder thread: needs t.mu(), which may be held by a
                // request waiting for an ACK this thread hasn't read yet
                std::lock_guard<std::mutex> lk(resp_mu);
                resp_pending = true;
                resp_cv.notify_one();
            } else if (b.rfind("CHECK_RESP|", 0) == 0) {
                if (auto_check) {
                    auto_check = false;
                    std::vector<std::string> p = util::split(b, '|');
                    std::string mine;
                    {
                        std::lock_guard<std::mutex> lk(t.mu());
                        mine = digest_text();
                    }
                    std::string d = p.size() >= 2 ? diff(mine, p[1]) : "all";
                    if (d.empty()) logx::info("Recovery complete: IN SYNC with tracker %d", t.partner_id());
                    else logx::warn("Recovery check: OUT OF SYNC, differs in [%s]", d.c_str());
                } else {
                    std::lock_guard<std::mutex> lk(chk_mu);
                    chk_resp = b;
                    chk_have = true;
                    chk_cv.notify_all();
                }
            }
        }
    }

    void drop_link(int fd) {
        {
            std::lock_guard<std::mutex> lk(link_mu);
            link_fd = -1;
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
        {
            std::lock_guard<std::mutex> lk(ack_mu);  // wake a replicate() waiting for an ACK
            ++link_gen;
            ack_cv.notify_all();
        }
        std::lock_guard<std::mutex> lk(t.mu());
        backup_attached = false;
        if (!stop.load() && t.role() == Role::BACKUP) {
            logx::warn("primary tracker %d is down: PROMOTING myself to primary", t.partner_id());
            t.set_role(Role::PRIMARY);
        }
    }

    void manage() {
        Clock::time_point start = Clock::now();
        while (!stop.load()) {
            int fd = obtain_link(start);
            if (fd < 0) break;
            net::set_recv_timeout(fd, cfg::PEER_DEAD_MS);
            {
                std::lock_guard<std::mutex> lk(link_mu);
                link_fd = fd;
            }
            if (handshake(fd)) reader_loop(fd);
            drop_link(fd);
            if (!stop.load()) usleep(cfg::RECONNECT_MS * 1000);
        }
    }

    void beat() {
        while (!stop.load()) {
            for (int i = 0; i < cfg::HEARTBEAT_MS / 100 && !stop.load(); ++i) usleep(100 * 1000);
            send("HB");
        }
    }
};

Replicator::Replicator(Tracker& t) : impl_(new Impl(t)) {}

Replicator::~Replicator() {
    stop();
    delete impl_;
}

void Replicator::start() {
    Impl& m = *impl_;
    m.t.set_role(Role::STARTING);
    if (m.t.id() == 1) {
        m.listen_fd = net::listen_on(m.t.self().ip, m.t.self().sync_port, 4);
        if (m.listen_fd < 0) logx::warn("cannot listen on sync port %d", m.t.self().sync_port);
    }
    logx::info("looking for tracker %d (waiting up to %d ms before starting alone)", m.t.partner_id(),
               cfg::STARTUP_WAIT_MS);
    m.manager = std::thread(&Impl::manage, &m);
    m.heartbeat = std::thread(&Impl::beat, &m);
    m.responder = std::thread(&Impl::respond, &m);
}

void Replicator::stop() {
    Impl& m = *impl_;
    if (m.stop.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lk(m.link_mu);
        if (m.link_fd >= 0) ::shutdown(m.link_fd, SHUT_RDWR);  // unblock the reader
    }
    if (m.manager.joinable()) m.manager.join();
    if (m.heartbeat.joinable()) m.heartbeat.join();
    {
        std::lock_guard<std::mutex> lk(m.resp_mu);
        m.resp_cv.notify_all();
    }
    if (m.responder.joinable()) m.responder.join();
    net::close_fd(m.listen_fd);
}

// Synchronous replication. Called by the primary with t.mu() held.
void Replicator::replicate(const std::string& op) {
    Impl& m = *impl_;
    if (!m.backup_attached) return;  // running alone: nothing to wait for
    uint64_t target, gen;
    {
        std::lock_guard<std::mutex> lk(m.ack_mu);
        target = m.acks + 1;
        gen = m.link_gen;
    }
    bool ok = m.send("OP|" + op);
    if (ok) {
        std::unique_lock<std::mutex> lk(m.ack_mu);
        ok = m.ack_cv.wait_for(lk, std::chrono::milliseconds(cfg::PEER_DEAD_MS),
                               [&] { return m.acks >= target || m.link_gen != gen; }) &&
             m.acks >= target;
    }
    if (!ok) {
        // no ACK: drop the backup and carry on alone
        logx::warn("backup did not acknowledge; continuing without it");
        m.backup_attached = false;
        std::lock_guard<std::mutex> lk(m.link_mu);
        if (m.link_fd >= 0) ::shutdown(m.link_fd, SHUT_RDWR);
    }
}

std::string Replicator::check_sync() {
    Impl& m = *impl_;
    std::string last_diff;
    for (int attempt = 0; attempt < 3; ++attempt) {
        int fd;
        {
            std::lock_guard<std::mutex> lk(m.link_mu);
            fd = m.link_fd;
        }
        if (fd < 0 || m.t.role() == Role::STARTING)
            return "PARTNER UNREACHABLE: tracker " + std::to_string(m.t.partner_id()) + " not connected";
        std::string mine, summary;
        {
            std::lock_guard<std::mutex> lk(m.t.mu());
            mine = m.digest_text();
            summary = m.t.state().summary();
        }
        {
            std::lock_guard<std::mutex> lk(m.chk_mu);
            m.chk_have = false;
        }
        if (!m.send("CHECK_REQ")) continue;
        std::unique_lock<std::mutex> lk(m.chk_mu);
        if (!m.chk_cv.wait_for(lk, std::chrono::seconds(3), [&] { return m.chk_have; }))
            return "PARTNER UNREACHABLE: no answer from tracker " + std::to_string(m.t.partner_id());
        std::vector<std::string> p = util::split(m.chk_resp, '|');
        last_diff = p.size() >= 2 ? Impl::diff(mine, p[1]) : "all";
        if (last_diff.empty()) return "IN SYNC: " + summary;
        lk.unlock();
        usleep(300 * 1000);  // a change may have been in flight: look again
    }
    return "OUT OF SYNC: differs in [" + last_diff + "]";
}
