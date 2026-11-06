#include "downloader.h"
#include "config.h"
#include "crypto.h"
#include "log.h"
#include "net.h"
#include "proto.h"
#include "util.h"

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

Downloader::Downloader(TrackerClient& tracker, SharedFiles& files, std::string my_ip)
    : tracker_(tracker), files_(files), my_ip_(std::move(my_ip)) {}

Downloader::~Downloader() { stop(); }

void Downloader::start() {
    stop_.store(false);
    for (int i = 0; i < cfg::MAX_ACTIVE_DOWNLOADS; ++i) slots_.emplace_back(&Downloader::slot_loop, this);
}

void Downloader::stop() {
    stop_.store(true);
    cv_.notify_all();
    for (auto& t : slots_) t.join();
    slots_.clear();
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& d : queue_) {
        d->error = "stopped";
        d->state = DownloadStatus::FAILED;
    }
    queue_.clear();
}

std::string Downloader::submit(const std::string& group, const std::string& name, const std::string& dest_in) {
    if (!util::valid_name(group) || !util::valid_name(name)) return "invalid group or file name";
    // Destination: an existing directory gets <dir>/<name>; otherwise it is
    // the full path of the new file.
    std::string dest = dest_in;
    const std::string normalized_group = util::to_lower(group);
    struct stat st {};
    if (::stat(dest.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) dest = dest + "/" + name;

    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& d : all_)
        if ((d->state == DownloadStatus::QUEUED || d->state == DownloadStatus::ACTIVE) &&
            (d->dest == dest || (d->group == normalized_group && d->name == name)))
            return "that download is already in progress";
    auto d = std::make_shared<DownloadStatus>();
    d->group = normalized_group;
    d->name = name;
    d->dest = dest;
    queue_.push_back(d);
    all_.push_back(d);
    cv_.notify_one();
    return "";
}

std::vector<std::string> Downloader::status_lines() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    for (const auto& d : all_) {
        char line[512];
        switch (d->state) {
            case DownloadStatus::QUEUED:
                std::snprintf(line, sizeof(line), "[Q] [%s] %s", d->group.c_str(), d->name.c_str());
                break;
            case DownloadStatus::ACTIVE: {
                uint64_t done = d->done.load(), total = d->total.load();
                std::snprintf(line, sizeof(line), "[D] [%s] %s  %llu/%llu pieces (%.1f%%)", d->group.c_str(),
                              d->name.c_str(), (unsigned long long)done, (unsigned long long)total,
                              total ? 100.0 * done / total : 0.0);
                break;
            }
            case DownloadStatus::DONE:
                std::snprintf(line, sizeof(line), "[C] [%s] %s", d->group.c_str(), d->name.c_str());
                break;
            case DownloadStatus::FAILED:
                std::snprintf(line, sizeof(line), "[F] [%s] %s  (%s)", d->group.c_str(), d->name.c_str(),
                              d->error.c_str());
                break;
        }
        out.push_back(line);
    }
    return out;
}

void Downloader::slot_loop() {
    while (true) {
        std::shared_ptr<DownloadStatus> d;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stop_.load() || !queue_.empty(); });
            if (stop_.load()) return;
            d = queue_.front();
            queue_.pop_front();
            d->state = DownloadStatus::ACTIVE;
        }
        run(*d);
    }
}

bool Downloader::fetch_piece(const std::string& seeder_addr, const std::string& group, const std::string& name,
                             uint64_t index, std::string& out) {
    std::string ip;
    int port = 0;
    if (!net::parse_addr(seeder_addr, ip, port)) return false;
    int fd = net::connect_to(ip, port, my_ip_);
    if (fd < 0) return false;
    net::set_recv_timeout(fd, cfg::SOCKET_TIMEOUT_MS);
    net::Frame f;
    bool ok = net::send_text(fd, "GET_PIECE|" + group + "|" + name + "|" + std::to_string(index)) &&
              net::recv_frame(fd, f) == net::RecvStatus::OK && f.type == net::FrameType::PIECE;
    ::close(fd);
    if (ok) out.swap(f.body);
    return ok;
}

namespace {
// Shared state of one download, used by its worker threads.
struct Sched {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<uint64_t> queue;               // pieces waiting for a worker
    std::vector<int> attempts;                // per piece
    std::vector<std::set<size_t>> failed_on;  // per piece: seeders that failed it
    std::vector<int> inflight;                // per seeder: requests in progress
    std::vector<uint64_t> served;             // per seeder: pieces delivered (for the log)
    uint64_t done = 0, total = 0;
    bool failed = false;
    std::string error;
};
}  // namespace

void Downloader::run(DownloadStatus& d) {
    auto fail = [&](const std::string& why) {
        std::lock_guard<std::mutex> lk(mu_);
        d.error = why;
        d.state = DownloadStatus::FAILED;
        logx::warn("download %s/%s failed: %s", d.group.c_str(), d.name.c_str(), why.c_str());
    };

    // 1. Ask the tracker for the file's hashes and its seeders.
    std::string r = tracker_.request("DOWNLOAD_FILE|" + d.group + "|" + d.name);
    std::vector<std::string> f = util::split(r, '|');
    if (f[0] != proto::OK || f.size() != 6) return fail(f.size() > 1 ? f[1] : r);
    uint64_t size = 0;
    if (!util::parse_u64(f[2], size) || size > cfg::MAX_FILE_SIZE) return fail("bad file size from tracker");
    const std::string file_sha1 = f[3];
    std::vector<std::string> hashes = f[4].empty() ? std::vector<std::string>{} : util::split(f[4], ',');
    std::vector<std::string> seeder_names, seeder_addrs;
    for (const auto& s : util::split(f[5], ',')) {
        size_t at = s.find('@');
        if (at == std::string::npos) continue;
        seeder_names.push_back(s.substr(0, at));
        seeder_addrs.push_back(s.substr(at + 1));
    }
    if (hashes.size() != util::piece_count(size) || seeder_addrs.empty()) return fail("bad metadata from tracker");
    d.total = hashes.size();

    // 2. create <dest>.part at full size (O_NOFOLLOW: don't follow a symlink)
    const std::string part = d.dest + ".part";
    int out = ::open(part.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_NOFOLLOW, 0644);
    if (out < 0) return fail("cannot create " + part + ": " + std::strerror(errno));
    if (::ftruncate(out, static_cast<off_t>(size)) != 0) {
        ::close(out);
        ::unlink(part.c_str());
        return fail("cannot size " + part + " (disk full?)");
    }

    // 3. Workers pull pieces from the shared queue.
    Sched s;
    s.total = hashes.size();
    s.attempts.assign(s.total, 0);
    s.failed_on.assign(s.total, {});
    s.inflight.assign(seeder_addrs.size(), 0);
    s.served.assign(seeder_addrs.size(), 0);
    for (uint64_t i = 0; i < s.total; ++i) s.queue.push_back(i);

    auto worker = [&] {
        std::string data;
        while (true) {
            uint64_t idx;
            size_t pick = 0;
            {
                std::unique_lock<std::mutex> lk(s.mu);
                s.cv.wait_for(lk, std::chrono::milliseconds(200), [&] {
                    return s.failed || stop_.load() || !s.queue.empty() || s.done == s.total;
                });
                if (s.failed || stop_.load() || s.done == s.total) return;
                if (s.queue.empty()) continue;
                idx = s.queue.front();
                s.queue.pop_front();
                // if every seeder has failed this piece, give them all another try
                if (s.failed_on[idx].size() >= seeder_addrs.size()) s.failed_on[idx].clear();
                // least busy seeder that hasn't failed it; ties go to the one that has served fewest
                bool found = false;
                for (size_t k = 0; k < seeder_addrs.size(); ++k) {
                    if (s.failed_on[idx].count(k)) continue;
                    if (!found || s.inflight[k] < s.inflight[pick] ||
                        (s.inflight[k] == s.inflight[pick] && s.served[k] < s.served[pick]))
                        pick = k, found = true;
                }
                s.inflight[pick]++;
            }
            bool ok = fetch_piece(seeder_addrs[pick], d.group, d.name, idx, data) &&
                      data.size() == util::piece_length(size, idx) &&
                      crypto::sha1_hex(data.data(), data.size()) == hashes[idx];
            if (ok) {
                uint64_t off = idx * cfg::PIECE_SIZE, w = 0;  // 64-bit offset
                while (w < data.size()) {
                    ssize_t n = ::pwrite(out, data.data() + w, data.size() - w, static_cast<off_t>(off + w));
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) break;
                    w += static_cast<uint64_t>(n);
                }
                ok = (w == data.size());
            }
            std::lock_guard<std::mutex> lk(s.mu);
            s.inflight[pick]--;
            if (ok) {
                s.done++;
                s.served[pick]++;
                d.done.store(s.done);
            } else if (++s.attempts[idx] >= cfg::PIECE_MAX_ATTEMPTS) {
                s.failed = true;
                s.error = "piece " + std::to_string(idx) + " failed " + std::to_string(s.attempts[idx]) +
                          " times (no seeder delivered a valid copy)";
            } else {
                s.failed_on[idx].insert(pick);
                s.queue.push_back(idx);  // retry, preferably on another seeder
            }
            s.cv.notify_all();
        }
    };
    std::vector<std::thread> workers;
    uint64_t n_workers = std::min<uint64_t>(cfg::WORKERS_PER_DOWNLOAD, std::max<uint64_t>(1, s.total));
    for (uint64_t i = 0; i < n_workers; ++i) workers.emplace_back(worker);
    for (auto& w : workers) w.join();

    if (s.failed || stop_.load() || s.done != s.total) {
        ::close(out);
        ::unlink(part.c_str());  // remove the partial file
        return fail(s.failed ? s.error : "stopped");
    }

    // 4. Whole-file check, then the atomic rename.
    if (::fsync(out) != 0) {
        ::close(out);
        ::unlink(part.c_str());
        return fail("cannot flush downloaded file");
    }
    ::close(out);
    int in = ::open(part.c_str(), O_RDONLY);
    crypto::Sha1Stream whole;
    std::string buf(cfg::PIECE_SIZE, '\0');
    bool read_ok = in >= 0;
    while (read_ok) {
        ssize_t n = ::read(in, &buf[0], buf.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) read_ok = false;
        if (n <= 0) break;
        whole.update(buf.data(), static_cast<size_t>(n));
    }
    if (in >= 0) ::close(in);
    if (!read_ok || whole.final_hex() != file_sha1) {
        ::unlink(part.c_str());
        return fail("whole-file SHA-1 mismatch");
    }
    if (::rename(part.c_str(), d.dest.c_str()) != 0) {
        ::unlink(part.c_str());
        return fail(std::string("rename failed: ") + std::strerror(errno));
    }

    // 5. register as a seeder now that the file is verified
    char abs[PATH_MAX];
    files_.add(d.group, d.name, ::realpath(d.dest.c_str(), abs) ? abs : d.dest);
    tracker_.request("ADD_SEEDER|" + d.group + "|" + d.name);

    std::string spread;
    for (size_t k = 0; k < seeder_names.size(); ++k)
        spread += (k ? ", " : "") + seeder_names[k] + "=" + std::to_string(s.served[k]);
    logx::info("download %s/%s complete: %llu pieces, per seeder: %s", d.group.c_str(), d.name.c_str(),
               (unsigned long long)s.total, spread.c_str());
    std::lock_guard<std::mutex> lk(mu_);
    d.state = DownloadStatus::DONE;
}
