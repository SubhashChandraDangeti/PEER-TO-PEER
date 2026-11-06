// Tracker request handling.
// One mutex (mu_) covers the whole state for a request, including the round
// trip to the backup. Keeps check-then-change atomic and ops in order on the
// backup; requests are serialized as a result.
#pragma once
#include "state.h"
#include "tracker_info.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

// who is logged in on this connection
struct ConnCtx {
    int fd = -1;
    std::string user;        // empty = not logged in
    std::string token_hash;  // must match State::sessions[user] to be valid
};

enum class Role { STARTING, BACKUP, PRIMARY };
const char* role_name(Role r);

class Replicator;  // tracker <-> tracker link (replication.h)

class Tracker {
public:
    Tracker(int id, const std::vector<TrackerAddr>& trackers);

    // Handle one request frame from a client; returns the reply body.
    std::string handle(ConnCtx& c, const std::string& body);
    // connection closed: drop the user's seeder entries
    void on_disconnect(ConnCtx& c);

    int id() const { return id_; }
    Role role() const { return role_.load(); }
    void set_role(Role r);
    const TrackerAddr& self() const { return trackers_[id_ - 1]; }
    const TrackerAddr& partner() const { return trackers_[2 - id_]; }
    int partner_id() const { return 3 - id_; }

    // Used by the replicator and the console.
    std::mutex& mu() { return mu_; }
    State& state() { return st_; }
    void set_replicator(Replicator* r) { repl_ = r; }

private:
    // send to backup (if any), then apply. mu_ held.
    void commit(const std::string& op);
    bool authed(ConnCtx& c);

    std::string cmd_create_user(ConnCtx& c, const std::vector<std::string>& f);
    std::string cmd_login(ConnCtx& c, const std::vector<std::string>& f);
    std::string cmd_resume(ConnCtx& c, const std::vector<std::string>& f);
    std::string cmd_logout(ConnCtx& c);
    std::string cmd_create_group(ConnCtx& c, const std::vector<std::string>& f);
    std::string cmd_join_group(ConnCtx& c, const std::vector<std::string>& f);
    std::string cmd_leave_group(ConnCtx& c, const std::vector<std::string>& f);
    std::string cmd_list_groups();
    std::string cmd_list_requests(ConnCtx& c, const std::vector<std::string>& f);
    std::string cmd_accept_request(ConnCtx& c, const std::vector<std::string>& f);
    std::string cmd_upload_file(ConnCtx& c, const std::vector<std::string>& f);
    std::string cmd_list_files(ConnCtx& c, const std::vector<std::string>& f);
    std::string cmd_download_file(ConnCtx& c, const std::vector<std::string>& f);
    std::string cmd_stop_share(ConnCtx& c, const std::vector<std::string>& f);
    std::string cmd_add_seeder(ConnCtx& c, const std::vector<std::string>& f);

    // each returns an error reply, or "" if ok
    std::string need_member(ConnCtx& c, const std::string& group);

    int id_;
    std::vector<TrackerAddr> trackers_;
    std::atomic<Role> role_{Role::PRIMARY};
    std::mutex mu_;
    State st_;
    Replicator* repl_ = nullptr;
};
