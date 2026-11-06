// Serves pieces to other clients: 1 acceptor + a fixed pool of workers.
// GET_PIECE|group|file|index -> PIECE frame, or a TEXT error.
#pragma once
#include "shared_files.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class PeerServer {
public:
    PeerServer(SharedFiles& files, bool corrupt_mode);
    bool start(const std::string& ip, int port);
    void stop();

private:
    void accept_loop();
    void worker();
    void serve(int fd);

    SharedFiles& files_;
    bool corrupt_;  // --corrupt: flip a byte in every piece (testing)
    int listen_fd_ = -1;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<int> queue_;
    std::vector<int> active_;  // fds being served, shut down on stop()
    std::thread acceptor_;
    std::vector<std::thread> workers_;
};
