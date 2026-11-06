#include "util.h"
#include "config.h"

#include <algorithm>
#include <cctype>

namespace util {

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t pos = s.find(sep, start);
        if (pos == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

std::string join(const std::vector<std::string>& v, char sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!cur.empty()) out.push_back(cur), cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool valid_name(const std::string& s) {
    if (s.empty() || s.size() > cfg::MAX_NAME_LEN) return false;
    if (s == "." || s == "..") return false;
    for (unsigned char c : s)
        if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
    return true;
}

bool valid_password(const std::string& s) {
    if (s.empty() || s.size() > cfg::MAX_NAME_LEN) return false;
    for (unsigned char c : s)
        if (c <= 32 || c >= 127 || c == '|') return false;
    return true;
}

bool parse_u64(const std::string& s, uint64_t& out) {
    if (s.empty() || s.size() > 20) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        uint64_t d = static_cast<uint64_t>(c - '0');
        if (v > (UINT64_MAX - d) / 10) return false;  // would overflow
        v = v * 10 + d;
    }
    out = v;
    return true;
}

bool is_hex(const std::string& s, size_t len) {
    if (s.size() != len) return false;
    for (unsigned char c : s)
        if (!std::isxdigit(c)) return false;
    return true;
}

std::string to_hex(const unsigned char* data, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string out(len * 2, '0');
    for (size_t i = 0; i < len; ++i) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 0xF];
    }
    return out;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

uint64_t piece_count(uint64_t size) {
    return (size + cfg::PIECE_SIZE - 1) / cfg::PIECE_SIZE;
}

uint64_t piece_length(uint64_t size, uint64_t index) {
    uint64_t start = index * cfg::PIECE_SIZE;  // 64-bit: no overflow up to 4 GiB
    if (start >= size) return 0;
    return std::min<uint64_t>(cfg::PIECE_SIZE, size - start);
}

std::string base_name(const std::string& path) {
    size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

}  // namespace util
