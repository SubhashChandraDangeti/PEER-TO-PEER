// Files this client shares, keyed by (group, name). Thread-safe.
#pragma once
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class SharedFiles {
public:
    void add(const std::string& group, const std::string& name, const std::string& path);
    void remove(const std::string& group, const std::string& name);
    bool lookup(const std::string& group, const std::string& name, std::string& path);
    std::vector<std::pair<std::string, std::string>> list();  // (group, name)
    void clear();

private:
    std::mutex mu_;
    std::map<std::pair<std::string, std::string>, std::string> files_;
};
