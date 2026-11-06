#include "tracker_info.h"
#include "net.h"
#include "util.h"

#include <fcntl.h>
#include <unistd.h>

bool load_tracker_info(const std::string& path, std::vector<TrackerAddr>& out, std::string& err) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        err = "cannot open " + path;
        return false;
    }
    std::string content;
    char buf[1024];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
        content.append(buf, static_cast<size_t>(n));
        if (content.size() > 64 * 1024) break;  // a real file is a few bytes
    }
    ::close(fd);

    out.clear();
    for (const std::string& raw : util::split(content, '\n')) {
        std::string line = util::trim(raw);
        if (line.empty() || line[0] == '#') continue;
        // ip:client_port:sync_port -> split off the last field as sync port
        size_t last = line.rfind(':');
        if (last == std::string::npos) {
            err = "bad line: " + line;
            return false;
        }
        TrackerAddr t;
        std::string ip;
        int cport = 0, sport = 0;
        if (!net::parse_addr(line.substr(0, last), ip, cport) ||
            !net::parse_addr(ip + ":" + line.substr(last + 1), ip, sport)) {
            err = "bad line (want ip:client_port:sync_port): " + line;
            return false;
        }
        t.ip = ip;
        t.client_port = cport;
        t.sync_port = sport;
        out.push_back(t);
    }
    if (out.size() != 2) {
        err = "tracker_info must list exactly 2 trackers";
        return false;
    }
    return true;
}
