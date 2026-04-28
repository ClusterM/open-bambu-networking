#include "source_log.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

namespace obn::source {

void noop_logger(void*, int, char const*) {}

namespace {

LogLevel parse_log_level(const char* s, LogLevel fallback)
{
    if (!s || !*s) return fallback;
    auto eq = [&](const char* a) {
        for (size_t i = 0;; ++i) {
            char x = s[i];
            char y = a[i];
            if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
            if (x != y) return false;
            if (!x) return true;
        }
    };
    if (eq("trace")) return LL_TRACE;
    if (eq("debug")) return LL_DEBUG;
    if (eq("info"))  return LL_INFO;
    if (eq("warn") || eq("warning")) return LL_WARN;
    if (eq("error") || eq("err"))    return LL_ERROR;
    if (eq("off") || eq("none") || eq("silent") || eq("0")) return LL_OFF;
    return fallback;
}

const char* level_tag(LogLevel lvl)
{
    switch (lvl) {
        case LL_TRACE: return "TRACE";
        case LL_DEBUG: return "DEBUG";
        case LL_INFO:  return "INFO";
        case LL_WARN:  return "WARN";
        case LL_ERROR: return "ERROR";
        case LL_OFF:   return "OFF";
    }
    return "?";
}

thread_local std::string g_last_error;

} // namespace

LogLevel current_log_level()
{
    static const LogLevel lvl = []() {
        return parse_log_level(std::getenv("OBN_BAMBUSOURCE_LOG_LEVEL"),
                               LL_INFO);
    }();
    return lvl;
}

// Resolution order:
//   1. $OBN_BAMBUSOURCE_LOG_FILE (set to "off"/"none"/empty to disable
//      the file mirror entirely; "stderr" routes to stderr).
//   2. $XDG_STATE_HOME/bambu-studio/obn-bambusource.log
//   3. $HOME/.local/state/bambu-studio/obn-bambusource.log
//   4. /tmp/obn-bambusource.log (last resort)
FILE* mirror_log_fp()
{
    static FILE* fp = []() -> FILE* {
        if (const char* env = std::getenv("OBN_BAMBUSOURCE_LOG_FILE")) {
            if (!*env || !std::strcmp(env, "off") ||
                !std::strcmp(env, "none") || !std::strcmp(env, "0"))
                return nullptr;
            if (!std::strcmp(env, "stderr") || !std::strcmp(env, "-"))
                return stderr;
            if (FILE* f = std::fopen(env, "a")) {
                std::setvbuf(f, nullptr, _IOLBF, 0);
                std::fprintf(f, "--- obn libBambuSource opened ---\n");
                return f;
            }
            // Fall through to the default search if the user-supplied
            // path could not be opened — better than dropping logs.
        }

        const char* paths[3] = {nullptr, nullptr, "/tmp/obn-bambusource.log"};
        static std::string p0, p1;
        if (const char* xdg = std::getenv("XDG_STATE_HOME")) {
            p0 = std::string(xdg) + "/bambu-studio/obn-bambusource.log";
            paths[0] = p0.c_str();
        }
        if (const char* home = std::getenv("HOME")) {
            p1 = std::string(home) + "/.local/state/bambu-studio/obn-bambusource.log";
            paths[1] = p1.c_str();
        }
        for (const char* path : paths) {
            if (!path) continue;
            if (FILE* f = std::fopen(path, "a")) {
                std::setvbuf(f, nullptr, _IOLBF, 0);
                std::fprintf(f, "--- obn libBambuSource opened ---\n");
                return f;
            }
        }
        return nullptr;
    }();
    return fp;
}

void log_at(LogLevel lvl, Logger logger, void* ctx, const char* fmt, ...)
{
    if (lvl < current_log_level()) return;

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (FILE* fp = mirror_log_fp()) {
        auto now = std::chrono::system_clock::now();
        auto tt  = std::chrono::system_clock::to_time_t(now);
        char ts[32];
        std::strftime(ts, sizeof(ts), "%F %T",
                      std::localtime(&tt));
        std::fprintf(fp, "%s [%s] %s\n", ts, level_tag(lvl), buf);
    }

    if (logger) {
        // Studio expects a heap-allocated buffer that it will free via
        // Bambu_FreeLogMsg. strdup() is the idiomatic way.
        logger(ctx, /*level=*/static_cast<int>(lvl), strdup(buf));
    }
}

void log_fmt(Logger logger, void* ctx, const char* fmt, ...)
{
    if (LL_INFO < current_log_level()) return;

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (FILE* fp = mirror_log_fp()) {
        auto now = std::chrono::system_clock::now();
        auto tt  = std::chrono::system_clock::to_time_t(now);
        char ts[32];
        std::strftime(ts, sizeof(ts), "%F %T",
                      std::localtime(&tt));
        std::fprintf(fp, "%s [INFO] %s\n", ts, buf);
    }

    if (logger) {
        logger(ctx, /*level=*/static_cast<int>(LL_INFO), strdup(buf));
    }
}

void set_last_error(const char* msg)
{
    g_last_error.assign(msg ? msg : "");
}

const char* get_last_error()
{
    return g_last_error.c_str();
}

} // namespace obn::source
