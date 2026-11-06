// Tracker state. Every change is an op string (e.g. MEMBER_ADD|g1|bob)
// applied by apply(). Same ops -> same state, so the backup, snapshots and
// check_sync all reuse it. Salts/tokens are generated before the op is built
// so both trackers apply identical ops.
//
// Ops:
//   USER_ADD|user|salt|pwhash          SESSION_SET|user|tokenhash|addr
//   SESSION_DEL|user                   SEEDERS_DROP|user
//   GROUP_ADD|group|owner
//   REQ_ADD|group|user                 MEMBER_ADD|group|user
//   MEMBER_DEL|group|user              FILE_ADD|group|file|size|sha1|hashes
//   SEEDER_ADD|group|file|user         SEEDER_DEL|group|file|user
//
// Ordered maps so snapshots/digests come out the same on both trackers.
// No locking here; Tracker holds its mutex around every call.
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

struct User {
    std::string salt;     // 16 random bytes, hex
    std::string pwhash;   // SHA-256(salt || password), hex
};

struct Session {
    std::string token_hash;  // SHA-256 of the token; the token itself is never stored
    std::string addr;        // client's listen address ip:port (where it serves pieces)
};

struct FileMeta {
    uint64_t size = 0;
    std::string sha1;                 // whole-file hash
    std::vector<std::string> pieces;  // one SHA-1 per 512 KB piece
    std::set<std::string> seeders;    // users currently sharing it
};

struct Group {
    std::string owner;
    std::set<std::string> members;    // includes the owner
    std::set<std::string> requests;   // pending join requests
    std::map<std::string, FileMeta> files;
};

class State {
public:
    // false if the op is malformed
    bool apply(const std::string& op);

    // ops that rebuild the state from empty
    std::vector<std::string> snapshot_ops() const;
    // SHA-256 per section, for check_sync
    std::map<std::string, std::string> digests() const;
    std::string summary() const;  // "5 users, 2 groups, ..." for humans
    void clear();

    // read by the handlers
    std::map<std::string, User> users;
    std::map<std::string, Session> sessions;  // one session per user
    std::map<std::string, Group> groups;
};
