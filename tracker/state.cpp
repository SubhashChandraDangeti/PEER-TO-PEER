#include "state.h"
#include "crypto.h"
#include "util.h"

bool State::apply(const std::string& op) {
    std::vector<std::string> f = util::split(op, '|');
    const std::string& t = f[0];

    if (t == "USER_ADD" && f.size() == 4) {
        users[f[1]] = User{f[2], f[3]};
    } else if (t == "SESSION_SET" && f.size() == 4) {
        // A new login or listen address cannot inherit another process's shares.
        auto old = sessions.find(f[1]);
        if (old != sessions.end() && (old->second.token_hash != f[2] || old->second.addr != f[3]))
            for (auto& g : groups)
                for (auto& file : g.second.files) file.second.seeders.erase(f[1]);
        sessions[f[1]] = Session{f[2], f[3]};  // a new login replaces the old one
    } else if (t == "SESSION_DEL" && f.size() == 2) {
        // logout: end session, stop sharing everything
        sessions.erase(f[1]);
        for (auto& g : groups)
            for (auto& file : g.second.files) file.second.seeders.erase(f[1]);
    } else if (t == "SEEDERS_DROP" && f.size() == 2) {
        // disconnect: stop sharing, keep the session for RESUME
        for (auto& g : groups)
            for (auto& file : g.second.files) file.second.seeders.erase(f[1]);
    } else if (t == "GROUP_ADD" && f.size() == 3) {
        Group& g = groups[f[1]];
        g.owner = f[2];
        g.members.insert(f[2]);
    } else if (t == "REQ_ADD" && f.size() == 3) {
        groups[f[1]].requests.insert(f[2]);
    } else if (t == "MEMBER_ADD" && f.size() == 3) {
        Group& g = groups[f[1]];
        g.requests.erase(f[2]);
        g.members.insert(f[2]);
    } else if (t == "MEMBER_DEL" && f.size() == 3) {
        // Leaving a group also stops sharing that group's files.
        Group& g = groups[f[1]];
        g.members.erase(f[2]);
        for (auto& file : g.files) file.second.seeders.erase(f[2]);
    } else if (t == "FILE_ADD" && f.size() == 6) {
        FileMeta& m = groups[f[1]].files[f[2]];
        uint64_t size = 0;
        util::parse_u64(f[3], size);
        m.size = size;
        m.sha1 = f[4];
        m.pieces = f[5].empty() ? std::vector<std::string>{} : util::split(f[5], ',');
    } else if (t == "SEEDER_ADD" && f.size() == 4) {
        groups[f[1]].files[f[2]].seeders.insert(f[3]);
    } else if (t == "SEEDER_DEL" && f.size() == 4) {
        auto g = groups.find(f[1]);
        if (g != groups.end()) {
            auto file = g->second.files.find(f[2]);
            if (file != g->second.files.end()) file->second.seeders.erase(f[3]);
        }
    } else {
        return false;
    }
    return true;
}

// order matters: each op only refers to things created earlier
std::vector<std::string> State::snapshot_ops() const {
    std::vector<std::string> ops;
    for (const auto& u : users) ops.push_back("USER_ADD|" + u.first + "|" + u.second.salt + "|" + u.second.pwhash);
    for (const auto& s : sessions)
        ops.push_back("SESSION_SET|" + s.first + "|" + s.second.token_hash + "|" + s.second.addr);
    for (const auto& g : groups) ops.push_back("GROUP_ADD|" + g.first + "|" + g.second.owner);
    for (const auto& g : groups)
        for (const auto& r : g.second.requests) ops.push_back("REQ_ADD|" + g.first + "|" + r);
    for (const auto& g : groups)
        for (const auto& m : g.second.members)
            if (m != g.second.owner) ops.push_back("MEMBER_ADD|" + g.first + "|" + m);
    for (const auto& g : groups)
        for (const auto& file : g.second.files)
            ops.push_back("FILE_ADD|" + g.first + "|" + file.first + "|" + std::to_string(file.second.size) +
                          "|" + file.second.sha1 + "|" + util::join(file.second.pieces, ','));
    for (const auto& g : groups)
        for (const auto& file : g.second.files)
            for (const auto& s : file.second.seeders)
                ops.push_back("SEEDER_ADD|" + g.first + "|" + file.first + "|" + s);
    return ops;
}

std::map<std::string, std::string> State::digests() const {
    // one digest per section of the (sorted) op list
    static const std::map<std::string, std::string> section = {
        {"USER_ADD", "users"},     {"SESSION_SET", "sessions"}, {"GROUP_ADD", "groups"},
        {"MEMBER_ADD", "members"}, {"REQ_ADD", "requests"},     {"FILE_ADD", "files"},
        {"SEEDER_ADD", "seeders"}};
    std::map<std::string, std::string> text;
    for (const auto& s : section) text[s.second] = "";
    for (const std::string& op : snapshot_ops()) text[section.at(op.substr(0, op.find('|')))] += op + "\n";
    std::map<std::string, std::string> out;
    for (const auto& t : text) out[t.first] = crypto::sha256_hex(t.second);
    return out;
}

std::string State::summary() const {
    size_t members = 0, requests = 0, files = 0, seeders = 0;
    for (const auto& g : groups) {
        members += g.second.members.size();
        requests += g.second.requests.size();
        files += g.second.files.size();
        for (const auto& f : g.second.files) seeders += f.second.seeders.size();
    }
    return std::to_string(users.size()) + " users, " + std::to_string(sessions.size()) + " sessions, " +
           std::to_string(groups.size()) + " groups, " + std::to_string(members) + " members, " +
           std::to_string(requests) + " requests, " + std::to_string(files) + " files, " +
           std::to_string(seeders) + " seeders";
}

void State::clear() {
    users.clear();
    sessions.clear();
    groups.clear();
}
