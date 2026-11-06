#include "shared_files.h"

void SharedFiles::add(const std::string& group, const std::string& name, const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    files_[{group, name}] = path;
}

void SharedFiles::remove(const std::string& group, const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    files_.erase({group, name});
}

bool SharedFiles::lookup(const std::string& group, const std::string& name, std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = files_.find({group, name});
    if (it == files_.end()) return false;
    path = it->second;
    return true;
}

std::vector<std::pair<std::string, std::string>> SharedFiles::list() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& f : files_) out.push_back(f.first);
    return out;
}

void SharedFiles::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    files_.clear();
}
