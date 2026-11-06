// Text protocol on top of frames.
// request: CMD|arg|...   reply: CODE|message|data...
// AUTH_FAILED is used for both unknown user and wrong password.
#pragma once
#include <string>
#include <vector>

namespace proto {

constexpr const char* OK = "OK";
constexpr const char* AUTH_FAILED = "AUTH_FAILED";
constexpr const char* NOT_LOGGED_IN = "NOT_LOGGED_IN";
constexpr const char* INVALID_ACTION = "INVALID_ACTION";
constexpr const char* NOT_FOUND = "NOT_FOUND";
constexpr const char* ALREADY_EXISTS = "ALREADY_EXISTS";
constexpr const char* NOT_PRIMARY = "NOT_PRIMARY";

inline std::string reply(const char* code, const std::string& msg,
                         const std::vector<std::string>& data = {}) {
    std::string out = std::string(code) + "|" + msg;
    for (const auto& d : data) out += "|" + d;
    return out;
}

}  // namespace proto
