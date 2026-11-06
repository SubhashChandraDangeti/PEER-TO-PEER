// Tracker process.  Usage: ./tracker tracker_info.txt <1|2>
#include "log.h"
#include "net.h"
#include "replication.h"
#include "tracker.h"
#include "tracker_info.h"

#include <csignal>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};
int g_listen_fd = -1;
std::mutex g_conn_mu;
std::set<int> g_conn_fds;            // open client sockets, for shutdown
// one thread per client; finished ones are joined on the next accept
struct Worker {
    std::thread th;
    std::shared_ptr<std::atomic<bool>> done;
};
std::vector<Worker> g_workers;

void serve_client(Tracker& t, int fd, std::shared_ptr<std::atomic<bool>> done) {
    ConnCtx c;
    c.fd = fd;
    net::Frame f;
    while (!g_stop.load()) {
        net::RecvStatus st = net::recv_frame(fd, f);
        if (st != net::RecvStatus::OK) break;  // closed, error or malformed header
        std::string reply = f.type == net::FrameType::TEXT
                                ? t.handle(c, f.body)
                                : "INVALID_ACTION|expected a text command";
        if (!net::send_text(fd, reply)) break;
    }
    if (!g_stop.load()) t.on_disconnect(c);
    std::lock_guard<std::mutex> lk(g_conn_mu);
    g_conn_fds.erase(fd);
    ::close(fd);
    done->store(true);
}

void accept_loop(Tracker& t) {
    while (!g_stop.load()) {
        int fd = net::accept_until(g_listen_fd, g_stop);
        if (fd < 0) break;  // stopping
        std::lock_guard<std::mutex> lk(g_conn_mu);
        for (auto it = g_workers.begin(); it != g_workers.end();) {  // reap finished
            if (it->done->load()) {
                it->th.join();
                it = g_workers.erase(it);
            } else {
                ++it;
            }
        }
        g_conn_fds.insert(fd);
        auto done = std::make_shared<std::atomic<bool>>(false);
        g_workers.push_back(Worker{std::thread(serve_client, std::ref(t), fd, done), done});
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);
    if (argc != 3 || (std::string(argv[2]) != "1" && std::string(argv[2]) != "2")) {
        std::cerr << "usage: ./tracker tracker_info.txt <1|2>\n";
        return 1;
    }
    std::vector<TrackerAddr> info;
    std::string err;
    if (!load_tracker_info(argv[1], info, err)) {
        std::cerr << "tracker_info error: " << err << "\n";
        return 1;
    }
    int id = std::stoi(argv[2]);  // validated above: "1" or "2"
    logx::set_name("tracker" + std::to_string(id));

    Tracker tracker(id, info);
    const TrackerAddr& me = tracker.self();
    g_listen_fd = net::listen_on(me.ip, me.client_port);
    if (g_listen_fd < 0) {
        std::cerr << "cannot listen on " << me.client_addr() << "\n";
        return 1;
    }
    Replicator repl(tracker);
    tracker.set_replicator(&repl);
    repl.start();
    logx::info("listening for clients on %s", me.client_addr().c_str());

    std::thread acceptor(accept_loop, std::ref(tracker));

    // console; if stdin closes, keep running until killed
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "quit") break;
        if (line == "check_sync") std::cout << repl.check_sync() << std::endl;
        else if (line == "state") {
            std::lock_guard<std::mutex> lk(tracker.mu());
            std::cout << role_name(tracker.role()) << ": " << tracker.state().summary() << std::endl;
        } else if (line == "users") {
            // print stored salt + hash (passwords are never stored)
            std::lock_guard<std::mutex> lk(tracker.mu());
            for (const auto& u : tracker.state().users)
                std::cout << u.first << "  salt=" << u.second.salt << "  sha256=" << u.second.pwhash << std::endl;
        } else if (!line.empty()) std::cout << "commands: quit, check_sync, state, users" << std::endl;
    }
    if (!std::cin) pause();  // stdin closed: wait for a signal

    logx::info("shutting down");
    g_stop.store(true);
    acceptor.join();  // wakes within 200 ms
    ::close(g_listen_fd);
    repl.stop();
    {
        std::lock_guard<std::mutex> lk(g_conn_mu);
        for (int fd : g_conn_fds) ::shutdown(fd, SHUT_RDWR);  // unblocks recv()
    }
    for (auto& w : g_workers) w.th.join();  // acceptor is stopped: no lock needed
    return 0;
}
