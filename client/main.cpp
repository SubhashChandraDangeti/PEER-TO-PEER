// Client.  Usage: ./client <IP>:<PORT> tracker_info.txt [--corrupt]
// <IP>:<PORT> is this client's own listen address for serving pieces.
// --corrupt makes it send corrupted pieces (for testing).
#include "config.h"
#include "crypto.h"
#include "downloader.h"
#include "log.h"
#include "net.h"
#include "peer_server.h"
#include "proto.h"
#include "shared_files.h"
#include "tracker_client.h"
#include "util.h"

#include <climits>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace {

TrackerClient* g_tracker = nullptr;
SharedFiles g_files;
std::string g_files_owner;  // user whose files g_files holds

void print_reply(const std::string& r) {
    std::vector<std::string> f = util::split(r, '|');
    std::cout << "[" << f[0] << "] " << (f.size() > 1 ? f[1] : "") << std::endl;
}

void print_list(const std::string& r) {
    std::vector<std::string> f = util::split(r, '|');
    print_reply(r);
    for (size_t i = 2; i < f.size(); ++i) std::cout << "  " << f[i] << std::endl;
}

// Re-register every file we still share (after login or a failover).
void reannounce() {
    for (const auto& gf : g_files.list()) {
        std::string r = g_tracker->request("ADD_SEEDER|" + gf.first + "|" + gf.second);
        if (r.rfind(proto::OK, 0) != 0) g_files.remove(gf.first, gf.second);
    }
}

// one pass over the file: SHA-1 per piece + whole file
bool hash_file(const std::string& path, uint64_t& size, std::string& whole, std::string& pieces, std::string& err) {
    int fd = ::open(path.c_str(), O_RDONLY);
    struct stat st {};
    if (fd < 0 || ::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0) ::close(fd);
        err = "cannot open regular file " + path;
        return false;
    }
    size = static_cast<uint64_t>(st.st_size);
    if (size > cfg::MAX_FILE_SIZE) {
        ::close(fd);
        err = "file is larger than 4 GiB";
        return false;
    }
    crypto::Sha1Stream all;
    std::string buf(cfg::PIECE_SIZE, '\0');
    std::vector<std::string> hs;
    uint64_t total = 0;
    while (total < size) {
        uint64_t want = util::piece_length(size, hs.size()), got = 0;
        while (got < want) {
            ssize_t n = ::read(fd, &buf[got], want - got);
            if (n <= 0) break;
            got += static_cast<uint64_t>(n);
        }
        if (got != want) {
            ::close(fd);
            err = "read error (file changed while hashing?)";
            return false;
        }
        hs.push_back(crypto::sha1_hex(buf.data(), want));
        all.update(buf.data(), want);
        total += want;
    }
    ::close(fd);
    whole = all.final_hex();
    pieces = util::join(hs, ',');
    return true;
}

const char* HELP =
    "commands:\n"
    "  create_user <user> <password>      login <user> <password>      logout\n"
    "  create_group <group>   join_group <group>   leave_group <group>   list_groups\n"
    "  list_requests <group>  accept_request <group> <user>\n"
    "  upload_file <group> <path>   list_files <group>   stop_share <group> <file>\n"
    "  download_file <group> <file> <destination>   show_downloads   quit\n";

}  // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);
    if (argc < 3 || argc > 4 || (argc == 4 && std::string(argv[3]) != "--corrupt")) {
        std::cerr << "usage: ./client <IP>:<PORT> tracker_info.txt [--corrupt]\n";
        return 1;
    }
    std::string my_ip;
    int my_port = 0;
    if (!net::parse_addr(argv[1], my_ip, my_port)) {
        std::cerr << "invalid listen address " << argv[1] << " (want ip:port)\n";
        return 1;
    }
    std::vector<TrackerAddr> trackers;
    std::string err;
    if (!load_tracker_info(argv[2], trackers, err)) {
        std::cerr << "tracker_info error: " << err << "\n";
        return 1;
    }
    bool corrupt = argc == 4;
    logx::set_name(std::string("client ") + argv[1]);

    PeerServer peers(g_files, corrupt);
    if (!peers.start(my_ip, my_port)) {
        std::cerr << "cannot listen on " << argv[1] << " (port in use?)\n";
        return 1;
    }
    if (corrupt) logx::warn("--corrupt: every piece this client serves will be corrupted (test mode)");
    TrackerClient tracker(trackers, my_ip, argv[1]);
    g_tracker = &tracker;
    tracker.set_on_resumed(reannounce);
    Downloader downloads(tracker, g_files, my_ip);
    downloads.start();
    std::cout << "client listening on " << argv[1] << ". Type 'help' for commands." << std::endl;

    std::string line;
    while (std::getline(std::cin, line)) {
        std::vector<std::string> a = util::split_ws(line);
        if (a.empty()) continue;
        const std::string& c = a[0];
        auto need = [&](size_t n) {
            if (a.size() == n + 1) return true;
            std::cout << "[INVALID_ACTION] wrong number of arguments. Type 'help'." << std::endl;
            return false;
        };

        if (c == "quit" || c == "exit") break;
        else if (c == "help") std::cout << HELP;
        else if (c == "create_user") { if (need(2)) print_reply(tracker.request("CREATE_USER|" + a[1] + "|" + a[2])); }
        else if (c == "login") {
            if (!need(2)) continue;
            std::string r = tracker.request("LOGIN|" + a[1] + "|" + a[2] + "|" + argv[1]);
            print_reply(r);
            std::vector<std::string> f = util::split(r, '|');
            if (f[0] == proto::OK && f.size() == 3) {
                tracker.set_session(a[1], f[2]);
                if (g_files_owner != a[1]) g_files.clear();  // different user: forget the old file list
                g_files_owner = a[1];
                reannounce();  // resume sharing what we still have
            }
        } else if (c == "logout") {
            if (!need(0)) continue;
            downloads.stop();
            print_reply(tracker.request("LOGOUT"));
            tracker.clear_session();
            g_files.clear();
            downloads.start();
        } else if (c == "create_group") { if (need(1)) print_reply(tracker.request("CREATE_GROUP|" + a[1])); }
        else if (c == "join_group") { if (need(1)) print_reply(tracker.request("JOIN_GROUP|" + a[1])); }
        else if (c == "leave_group") {
            if (!need(1)) continue;
            std::string r = tracker.request("LEAVE_GROUP|" + a[1]);
            print_reply(r);
            if (r.rfind(proto::OK, 0) == 0)
                for (const auto& gf : g_files.list())
                    if (gf.first == util::to_lower(a[1])) g_files.remove(gf.first, gf.second);
        } else if (c == "list_groups") { if (need(0)) print_list(tracker.request("LIST_GROUPS")); }
        else if (c == "list_requests") { if (need(1)) print_list(tracker.request("LIST_REQUESTS|" + a[1])); }
        else if (c == "accept_request") { if (need(2)) print_reply(tracker.request("ACCEPT_REQUEST|" + a[1] + "|" + a[2])); }
        else if (c == "list_files") {
            if (!need(1)) continue;
            std::string r = tracker.request("LIST_FILES|" + a[1]);
            std::vector<std::string> f = util::split(r, '|');
            print_reply(r);
            for (size_t i = 2; i < f.size(); ++i) {
                std::vector<std::string> p = util::split(f[i], ':');  // name:size:seeders
                if (p.size() == 3)
                    std::cout << "  " << p[0] << "  (" << p[1] << " bytes, " << p[2] << " seeder(s))" << std::endl;
            }
        } else if (c == "upload_file") {
            if (!need(2)) continue;
            std::string name = util::base_name(a[2]);
            if (!util::valid_name(name)) {
                std::cout << "[INVALID_ACTION] file name must use only A-Z a-z 0-9 . _ -" << std::endl;
                continue;
            }
            uint64_t size = 0;
            std::string whole, pieces;
            if (!hash_file(a[2], size, whole, pieces, err)) {
                std::cout << "[INVALID_ACTION] " << err << std::endl;
                continue;
            }
            std::string r = tracker.request("UPLOAD_FILE|" + a[1] + "|" + name + "|" + std::to_string(size) + "|" +
                                            whole + "|" + pieces);
            print_reply(r);
            if (r.rfind(proto::OK, 0) == 0) {
                char abs[PATH_MAX];
                g_files.add(util::to_lower(a[1]), name, ::realpath(a[2].c_str(), abs) ? abs : a[2]);
            }
        } else if (c == "download_file") {
            if (!need(3)) continue;
            if (tracker.user().empty()) {
                std::cout << "[NOT_LOGGED_IN] please log in first" << std::endl;
                continue;
            }
            std::string e = downloads.submit(a[1], a[2], a[3]);
            std::cout << (e.empty() ? "[OK] download queued; see show_downloads" : "[INVALID_ACTION] " + e)
                      << std::endl;
        } else if (c == "show_downloads") {
            auto lines = downloads.status_lines();
            if (lines.empty()) std::cout << "no downloads" << std::endl;
            for (const auto& l : lines) std::cout << l << std::endl;
        } else if (c == "stop_share") {
            if (!need(2)) continue;
            std::string r = tracker.request("STOP_SHARE|" + a[1] + "|" + a[2]);
            print_reply(r);
            if (r.rfind(proto::OK, 0) == 0) g_files.remove(util::to_lower(a[1]), a[2]);
        } else {
            std::cout << "[INVALID_ACTION] unknown command. Type 'help'." << std::endl;
        }
    }

    // shutdown; the tracker drops our seeder entries when we disconnect
    downloads.stop();
    peers.stop();
    tracker.close();
    return 0;
}
