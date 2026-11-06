// String helpers and input validation.
// Names: 1..64 chars of [A-Za-z0-9._-], not "." or "..".
// Numbers are parsed by hand and never throw.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace util {

// Split on `sep`, keeping empty fields ("a||b" -> {"a","","b"}).
std::vector<std::string> split(const std::string& s, char sep);
std::string join(const std::vector<std::string>& v, char sep);
std::string trim(const std::string& s);
// Split a CLI line on spaces/tabs, dropping empty tokens.
std::vector<std::string> split_ws(const std::string& s);

bool valid_name(const std::string& s);
// Passwords: 1..64 printable ASCII, no '|' and no spaces.
bool valid_password(const std::string& s);
bool parse_u64(const std::string& s, uint64_t& out);
bool is_hex(const std::string& s, size_t len);  // exactly len hex chars

std::string to_hex(const unsigned char* data, size_t len);
std::string to_lower(std::string s);

// Number of pieces for a file of `size` bytes (0 for an empty file).
uint64_t piece_count(uint64_t size);
// Length of piece `index` (the last one may be shorter).
uint64_t piece_length(uint64_t size, uint64_t index);

// last path component
std::string base_name(const std::string& path);

}  // namespace util
