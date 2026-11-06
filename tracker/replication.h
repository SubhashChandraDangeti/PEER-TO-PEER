// Tracker-to-tracker link (primary/backup replication).
#pragma once
#include <string>

class Tracker;

class Replicator {
public:
    explicit Replicator(Tracker& t);
    ~Replicator();
    void start();
    void stop();
    // primary only, tracker mutex held: send op and wait for the ACK
    void replicate(const std::string& op);
    // console: compare state with the partner
    std::string check_sync();

private:
    struct Impl;
    Impl* impl_;
};
