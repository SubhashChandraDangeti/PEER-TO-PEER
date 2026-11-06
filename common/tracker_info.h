// Reads tracker_info.txt: one ip:client_port:sync_port line per tracker.
#pragma once
#include <string>
#include <vector>

struct TrackerAddr {
    std::string ip;
    int client_port = 0;
    int sync_port = 0;
    std::string client_addr() const { return ip + ":" + std::to_string(client_port); }
};

// false (with err) unless there are exactly 2 valid lines
bool load_tracker_info(const std::string& path, std::vector<TrackerAddr>& out, std::string& err);
