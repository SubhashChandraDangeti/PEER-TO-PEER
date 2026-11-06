// Background downloads. 3 slot threads take jobs from a queue; each job
// runs 4 workers. Workers pop piece numbers from a shared queue, pick the
// seeder with the fewest requests in flight, check the SHA-1 and pwrite the
// piece into <dest>.part. Failed pieces go back on the queue (max 6 tries).
// At the end the whole file is checked and renamed.
#pragma once
#include "shared_files.h"
#include "tracker_client.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct DownloadStatus {
    enum State { QUEUED, ACTIVE, DONE, FAILED } state = QUEUED;
    std::string group, name, dest, error;
    std::atomic<uint64_t> total{0};  // read by show_downloads
    std::atomic<uint64_t> done{0};
};

class Downloader {
public:
    Downloader(TrackerClient& tracker, SharedFiles& files, std::string my_ip);
    ~Downloader();
    void start();
    void stop();
    // Queue a download. Returns "" or an error message.
    std::string submit(const std::string& group, const std::string& name, const std::string& dest);
    std::vector<std::string> status_lines();

private:
    void slot_loop();
    void run(DownloadStatus& d);
    bool fetch_piece(const std::string& seeder_addr, const std::string& group, const std::string& name,
                     uint64_t index, std::string& out);

    TrackerClient& tracker_;
    SharedFiles& files_;
    std::string my_ip_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<DownloadStatus>> queue_;
    std::vector<std::shared_ptr<DownloadStatus>> all_;  // for show_downloads, in order
    std::vector<std::thread> slots_;
};
