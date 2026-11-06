// Thread-safe logging to stderr. Never log request bodies (passwords, tokens).
#pragma once
#include <cstdio>
#include <string>

namespace logx {

void set_name(const std::string& component);  // e.g. "tracker1", "client:alice"
void set_quiet(bool q);                       // suppress INFO (tests)
void info(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void warn(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace logx
