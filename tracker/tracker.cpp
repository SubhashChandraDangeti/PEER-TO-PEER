// Request handlers: validate, check session/permissions, build op, commit().
#include "tracker.h"
#include "config.h"
#include "crypto.h"
#include "log.h"
#include "net.h"
#include "proto.h"
#include "replication.h"
#include "util.h"

using namespace proto;
using Fields = std::vector<std::string>;

const char* role_name(Role r) {
    switch (r) {
        case Role::STARTING: return "STARTING";
        case Role::BACKUP: return "BACKUP";
        case Role::PRIMARY: return "PRIMARY";
    }
    return "?";
}

Tracker::Tracker(int id, const std::vector<TrackerAddr>& trackers) : id_(id), trackers_(trackers) {}

void Tracker::set_role(Role r) {
    Role old = role_.exchange(r);
    if (old != r) logx::info("role %s -> %s", role_name(old), role_name(r));
}

void Tracker::commit(const std::string& op) {
    // backup first, so an OK reply is never lost on failover
    if (repl_) repl_->replicate(op);
    st_.apply(op);
}

bool Tracker::authed(ConnCtx& c) {
    if (c.user.empty()) return false;
    auto it = st_.sessions.find(c.user);
    if (it == st_.sessions.end() || it->second.token_hash != c.token_hash) {
        // Logged out elsewhere, or replaced by a newer login.
        c.user.clear();
        c.token_hash.clear();
        return false;
    }
    return true;
}

std::string Tracker::need_member(ConnCtx& c, const std::string& group) {
    if (!authed(c)) return reply(NOT_LOGGED_IN, "please log in first");
    auto g = st_.groups.find(group);
    if (g == st_.groups.end()) return reply(NOT_FOUND, "no such group");
    if (!g->second.members.count(c.user)) return reply(INVALID_ACTION, "you are not a member of " + group);
    return "";
}

std::string Tracker::handle(ConnCtx& c, const std::string& body) {
    Fields f = util::split(body, '|');
    const std::string cmd = f[0];

    std::lock_guard<std::mutex> lk(mu_);
    if (role_.load() != Role::PRIMARY)
        return reply(NOT_PRIMARY, "this tracker is the backup", {partner().client_addr()});

    // log command + user only, never the body
    logx::info("cmd %s from %s", cmd.c_str(), c.user.empty() ? "(anon)" : c.user.c_str());

    if (cmd == "CREATE_USER") return cmd_create_user(c, f);
    if (cmd == "LOGIN") return cmd_login(c, f);
    if (cmd == "RESUME") return cmd_resume(c, f);
    if (cmd == "LOGOUT") return cmd_logout(c);
    if (cmd == "CREATE_GROUP") return cmd_create_group(c, f);
    if (cmd == "JOIN_GROUP") return cmd_join_group(c, f);
    if (cmd == "LEAVE_GROUP") return cmd_leave_group(c, f);
    if (cmd == "LIST_GROUPS") return authed(c) ? cmd_list_groups() : reply(NOT_LOGGED_IN, "please log in first");
    if (cmd == "LIST_REQUESTS") return cmd_list_requests(c, f);
    if (cmd == "ACCEPT_REQUEST") return cmd_accept_request(c, f);
    if (cmd == "UPLOAD_FILE") return cmd_upload_file(c, f);
    if (cmd == "LIST_FILES") return cmd_list_files(c, f);
    if (cmd == "DOWNLOAD_FILE") return cmd_download_file(c, f);
    if (cmd == "STOP_SHARE") return cmd_stop_share(c, f);
    if (cmd == "ADD_SEEDER") return cmd_add_seeder(c, f);
    return reply(INVALID_ACTION, "unknown command");
}

void Tracker::on_disconnect(ConnCtx& c) {
    std::lock_guard<std::mutex> lk(mu_);
    // stop sharing but keep the session: the client may be failing over and
    // will RESUME, and an old connection closing must not log out a live client
    if (role_.load() == Role::PRIMARY && authed(c)) {
        logx::info("%s disconnected, removing their seeder entries", c.user.c_str());
        commit("SEEDERS_DROP|" + c.user);
    }
}

// ---------------------------------------------------------------- accounts

std::string Tracker::cmd_create_user(ConnCtx&, const Fields& f) {
    if (f.size() != 3 || !util::valid_name(f[1]) || !util::valid_password(f[2]))
        return reply(INVALID_ACTION, "usage: create_user <user_id> <password> (names: A-Z a-z 0-9 . _ -)");
    if (st_.users.count(f[1])) return reply(ALREADY_EXISTS, "user already exists");
    std::string salt = crypto::random_hex(cfg::SALT_BYTES);
    commit("USER_ADD|" + f[1] + "|" + salt + "|" + crypto::hash_password(salt, f[2]));
    return reply(OK, "user " + f[1] + " created");
}

std::string Tracker::cmd_login(ConnCtx& c, const Fields& f) {
    std::string ip;
    int port = 0;
    if (f.size() != 4 || !util::valid_name(f[1]) || !util::valid_password(f[2]) || !net::parse_addr(f[3], ip, port))
        return reply(INVALID_ACTION, "usage: login <user_id> <password>");
    if (authed(c)) return reply(INVALID_ACTION, "already logged in as " + c.user + " on this client");

    auto u = st_.users.find(f[1]);
    // same reply (and similar work) for unknown user and wrong password
    std::string salt = u != st_.users.end() ? u->second.salt : std::string(32, '0');
    std::string expected = u != st_.users.end() ? u->second.pwhash : std::string(64, '0');
    bool ok = crypto::ct_equal(crypto::hash_password(salt, f[2]), expected) && u != st_.users.end();
    if (!ok) return reply(AUTH_FAILED, "invalid user id or password");

    // a new login replaces the old session
    std::string token = crypto::random_hex(cfg::TOKEN_BYTES);
    std::string token_hash = crypto::sha256_hex(token);
    commit("SESSION_SET|" + f[1] + "|" + token_hash + "|" + f[3]);
    c.user = f[1];
    c.token_hash = token_hash;
    return reply(OK, "logged in as " + f[1], {token});
}

// After a failover the client presents its token instead of the password.
std::string Tracker::cmd_resume(ConnCtx& c, const Fields& f) {
    std::string ip;
    int port = 0;
    if (f.size() != 4 || !util::valid_name(f[1]) || !util::is_hex(f[2], cfg::TOKEN_BYTES * 2) ||
        !net::parse_addr(f[3], ip, port))
        return reply(INVALID_ACTION, "bad resume request");
    auto s = st_.sessions.find(f[1]);
    std::string th = crypto::sha256_hex(f[2]);
    if (s == st_.sessions.end() || !crypto::ct_equal(th, s->second.token_hash))
        return reply(AUTH_FAILED, "session expired, please log in again");
    if (s->second.addr != f[3]) commit("SESSION_SET|" + f[1] + "|" + th + "|" + f[3]);
    c.user = f[1];
    c.token_hash = th;
    return reply(OK, "session resumed for " + f[1]);
}

std::string Tracker::cmd_logout(ConnCtx& c) {
    if (!authed(c)) return reply(NOT_LOGGED_IN, "not logged in");
    commit("SESSION_DEL|" + c.user);
    std::string u = c.user;
    c.user.clear();
    c.token_hash.clear();
    return reply(OK, "logged out " + u + "; your files are no longer shared");
}

// ------------------------------------------------------------------ groups

std::string Tracker::cmd_create_group(ConnCtx& c, const Fields& f) {
    if (!authed(c)) return reply(NOT_LOGGED_IN, "please log in first");
    if (f.size() != 2 || !util::valid_name(f[1])) return reply(INVALID_ACTION, "usage: create_group <group_id>");
    std::string g = util::to_lower(f[1]);  // group names are case-insensitive
    if (st_.groups.count(g)) return reply(ALREADY_EXISTS, "group already exists");
    commit("GROUP_ADD|" + g + "|" + c.user);
    return reply(OK, "group " + g + " created; you are the owner");
}

std::string Tracker::cmd_join_group(ConnCtx& c, const Fields& f) {
    if (!authed(c)) return reply(NOT_LOGGED_IN, "please log in first");
    if (f.size() != 2 || !util::valid_name(f[1])) return reply(INVALID_ACTION, "usage: join_group <group_id>");
    std::string g = util::to_lower(f[1]);
    auto it = st_.groups.find(g);
    if (it == st_.groups.end()) return reply(NOT_FOUND, "no such group");
    if (it->second.members.count(c.user)) return reply(ALREADY_EXISTS, "already a member");
    if (it->second.requests.count(c.user)) return reply(ALREADY_EXISTS, "request already pending");
    commit("REQ_ADD|" + g + "|" + c.user);
    return reply(OK, "join request sent to the owner of " + g);
}

std::string Tracker::cmd_leave_group(ConnCtx& c, const Fields& f) {
    if (f.size() != 2 || !util::valid_name(f[1])) return reply(INVALID_ACTION, "usage: leave_group <group_id>");
    std::string g = util::to_lower(f[1]);
    std::string err = need_member(c, g);
    if (!err.empty()) return err;
    if (st_.groups[g].owner == c.user) return reply(INVALID_ACTION, "the owner cannot leave the group");
    commit("MEMBER_DEL|" + g + "|" + c.user);
    return reply(OK, "left " + g + "; your files in it are no longer shared");
}

std::string Tracker::cmd_list_groups() {
    Fields names;
    for (const auto& g : st_.groups) names.push_back(g.first);
    return reply(OK, std::to_string(names.size()) + " group(s)", names);
}

std::string Tracker::cmd_list_requests(ConnCtx& c, const Fields& f) {
    if (f.size() != 2 || !util::valid_name(f[1])) return reply(INVALID_ACTION, "usage: list_requests <group_id>");
    std::string g = util::to_lower(f[1]);
    std::string err = need_member(c, g);
    if (!err.empty()) return err;
    if (st_.groups[g].owner != c.user) return reply(INVALID_ACTION, "only the owner can list requests");
    const auto& r = st_.groups[g].requests;
    return reply(OK, std::to_string(r.size()) + " pending request(s)", Fields(r.begin(), r.end()));
}

std::string Tracker::cmd_accept_request(ConnCtx& c, const Fields& f) {
    if (f.size() != 3 || !util::valid_name(f[1]) || !util::valid_name(f[2]))
        return reply(INVALID_ACTION, "usage: accept_request <group_id> <user_id>");
    std::string g = util::to_lower(f[1]);
    std::string err = need_member(c, g);
    if (!err.empty()) return err;
    if (st_.groups[g].owner != c.user) return reply(INVALID_ACTION, "only the owner can accept requests");
    if (!st_.groups[g].requests.count(f[2])) return reply(NOT_FOUND, "no pending request from " + f[2]);
    commit("MEMBER_ADD|" + g + "|" + f[2]);
    return reply(OK, f[2] + " is now a member of " + g);
}

// ------------------------------------------------------------------- files

// UPLOAD_FILE|group|name|size|file_sha1|h1,h2,...
std::string Tracker::cmd_upload_file(ConnCtx& c, const Fields& f) {
    if (f.size() != 6 || !util::valid_name(f[1]) || !util::valid_name(f[2]))
        return reply(INVALID_ACTION, "usage: upload_file <group_id> <file_path>");
    std::string g = util::to_lower(f[1]);
    std::string err = need_member(c, g);
    if (!err.empty()) return err;

    uint64_t size = 0;
    if (!util::parse_u64(f[3], size) || size > cfg::MAX_FILE_SIZE)
        return reply(INVALID_ACTION, "file size must be at most 4 GiB");
    if (!util::is_hex(f[4], 40)) return reply(INVALID_ACTION, "bad file hash");
    Fields hashes = f[5].empty() ? Fields{} : util::split(f[5], ',');
    if (hashes.size() != util::piece_count(size)) return reply(INVALID_ACTION, "piece hash count does not match size");
    for (const auto& h : hashes)
        if (!util::is_hex(h, 40)) return reply(INVALID_ACTION, "bad piece hash");

    auto& files = st_.groups[g].files;
    auto it = files.find(f[2]);
    if (it != files.end()) {
        // Same name: only allowed if it is the same content (same hash).
        if (it->second.sha1 != f[4] || it->second.size != size || it->second.pieces != hashes)
            return reply(ALREADY_EXISTS, "a different file with this name already exists");
        commit("SEEDER_ADD|" + g + "|" + f[2] + "|" + c.user);
        return reply(OK, "file already in group; you are now also a seeder");
    }
    commit("FILE_ADD|" + g + "|" + f[2] + "|" + f[3] + "|" + f[4] + "|" + f[5]);
    commit("SEEDER_ADD|" + g + "|" + f[2] + "|" + c.user);
    return reply(OK, "uploaded " + f[2] + " to " + g + " (" + std::to_string(hashes.size()) + " pieces)");
}

std::string Tracker::cmd_list_files(ConnCtx& c, const Fields& f) {
    if (f.size() != 2 || !util::valid_name(f[1])) return reply(INVALID_ACTION, "usage: list_files <group_id>");
    std::string g = util::to_lower(f[1]);
    std::string err = need_member(c, g);
    if (!err.empty()) return err;
    Fields out;  // name:size:seeder_count, only files someone is sharing
    for (const auto& file : st_.groups[g].files)
        if (!file.second.seeders.empty())
            out.push_back(file.first + ":" + std::to_string(file.second.size) + ":" +
                          std::to_string(file.second.seeders.size()));
    return reply(OK, std::to_string(out.size()) + " file(s)", out);
}

// Reply: OK|msg|size|file_sha1|h1,h2,...|user@ip:port,user@ip:port
std::string Tracker::cmd_download_file(ConnCtx& c, const Fields& f) {
    if (f.size() != 3 || !util::valid_name(f[1]) || !util::valid_name(f[2]))
        return reply(INVALID_ACTION, "usage: download_file <group_id> <file_name> <destination_path>");
    std::string g = util::to_lower(f[1]);
    std::string err = need_member(c, g);
    if (!err.empty()) return err;
    auto it = st_.groups[g].files.find(f[2]);
    if (it == st_.groups[g].files.end()) return reply(NOT_FOUND, "no such file in " + g);
    Fields seeders;
    for (const auto& s : it->second.seeders) {
        auto sess = st_.sessions.find(s);
        if (s != c.user && sess != st_.sessions.end()) seeders.push_back(s + "@" + sess->second.addr);
    }
    if (seeders.empty()) return reply(NOT_FOUND, "no other seeder is online for this file");
    return reply(OK, "file info", {std::to_string(it->second.size), it->second.sha1,
                                   util::join(it->second.pieces, ','), util::join(seeders, ',')});
}

std::string Tracker::cmd_stop_share(ConnCtx& c, const Fields& f) {
    if (f.size() != 3 || !util::valid_name(f[1]) || !util::valid_name(f[2]))
        return reply(INVALID_ACTION, "usage: stop_share <group_id> <file_name>");
    std::string g = util::to_lower(f[1]);
    std::string err = need_member(c, g);
    if (!err.empty()) return err;
    auto it = st_.groups[g].files.find(f[2]);
    if (it == st_.groups[g].files.end() || !it->second.seeders.count(c.user))
        return reply(NOT_FOUND, "you are not sharing " + f[2] + " in " + g);
    // only the caller's own entry
    commit("SEEDER_DEL|" + g + "|" + f[2] + "|" + c.user);
    return reply(OK, "stopped sharing " + f[2] + " in " + g);
}

// After a verified download, or to re-announce files after login/failover.
std::string Tracker::cmd_add_seeder(ConnCtx& c, const Fields& f) {
    if (f.size() != 3 || !util::valid_name(f[1]) || !util::valid_name(f[2]))
        return reply(INVALID_ACTION, "bad add_seeder request");
    std::string g = util::to_lower(f[1]);
    std::string err = need_member(c, g);
    if (!err.empty()) return err;
    if (!st_.groups[g].files.count(f[2])) return reply(NOT_FOUND, "no such file in " + g);
    // user comes from the session, so you can only add yourself
    commit("SEEDER_ADD|" + g + "|" + f[2] + "|" + c.user);
    return reply(OK, "now seeding " + f[2]);
}
