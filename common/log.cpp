#include "log.h"

#include <cstdarg>
#include <ctime>
#include <mutex>
#include <sys/time.h>

namespace logx {

static std::mutex g_mu;  // one line at a time
static std::string g_name = "p2p";
static bool g_quiet = false;

void set_name(const std::string& component) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_name = component;
}

void set_quiet(bool q) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_quiet = q;
}

static void emit(const char* level, const char* fmt, va_list ap) {
    timeval tv;
    gettimeofday(&tv, nullptr);
    tm t;
    localtime_r(&tv.tv_sec, &t);
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_quiet && level[0] == 'I') return;
    std::fprintf(stderr, "[%02d:%02d:%02d.%03d %s %s] ", t.tm_hour, t.tm_min, t.tm_sec,
                 static_cast<int>(tv.tv_usec / 1000), g_name.c_str(), level);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
}

void info(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit("INFO", fmt, ap);
    va_end(ap);
}

void warn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit("WARN", fmt, ap);
    va_end(ap);
}

}  // namespace logx
