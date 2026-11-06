// Connection to the trackers. request() handles NOT_PRIMARY redirects and
// failover (reconnect, RESUME with the token, re-announce, retry once).
// One mutex: the CLI and download threads share the connection.
#pragma once
#include "tracker_info.h"

#include <functional>
#include <mutex>
#include <string>
#include <vector>

class TrackerClient {
public:
    TrackerClient(std::vector<TrackerAddr> trackers, std::string my_ip, std::string my_addr);
    ~TrackerClient();

    // Send one request and return the reply body. On total failure returns
    // "INVALID_ACTION|no tracker reachable".
    std::string request(const std::string& body);

    // Remembers the session after LOGIN, forgets it after LOGOUT.
    void set_session(const std::string& user, const std::string& token);
    void clear_session();
    std::string user();

    // Called after a successful failover to re-announce shared files.
    void set_on_resumed(std::function<void()> cb) { on_resumed_ = std::move(cb); }
    void close();

private:
    bool connect_any(const std::string& preferred);  // mu_ held
    bool resume_locked(std::string& redirect);         // mu_ held
    bool exchange(const std::string& body, std::string& reply);  // mu_ held

    std::vector<TrackerAddr> trackers_;
    std::string my_ip_, my_addr_;
    std::mutex mu_;
    int fd_ = -1;
    std::string connected_to_;
    std::string last_failed_;  // tried last on the next reconnect
    std::string user_, token_;
    std::function<void()> on_resumed_;
};
