// Shared logging primitives for libBambuSource.
//
// libBambuSource is a single shared object that bundles several
// independent submodules (the C-ABI tunnel layer in BambuSource.cpp,
// the GStreamer/FFmpeg video backends, the macOS BambuPlayer wrapper).
// They all need the same three things:
//
//   * a level-aware printf-style sink that fans out to (a) Studio's
//     Logger callback when one is set on the tunnel and (b) a file
//     mirror in $XDG_STATE_HOME/bambu-studio/obn-bambusource.log so
//     LAN-mode camera issues remain debuggable when Studio's own log
//     swallows them;
//
//   * a way to honor OBN_BAMBUSOURCE_LOG_LEVEL=<level> uniformly so
//     turning the noise floor up or down works for every submodule
//     at once;
//
//   * a thread-local "last error" message that Bambu_GetLastErrorMsg
//     can return.
//
// Everything here is process-static and intentionally leaks: the
// plugin lives until OrcaSlicer exits.
#pragma once

#include <cstdarg>
#include <cstdio>

namespace obn::source {

// Studio's Logger typedef in tchar form, repeated here so submodules
// don't need to pull in BambuTunnel.h. char* on Linux/macOS;
// gstbambusrc and our own callers free the message with
// Bambu_FreeLogMsg, so log_at()/log_fmt() always allocate via strdup.
using Logger = void (*)(void* ctx, int level, char const* msg);

enum LogLevel {
    LL_TRACE = 0,
    LL_DEBUG = 1,
    LL_INFO  = 2,
    LL_WARN  = 3,
    LL_ERROR = 4,
    LL_OFF   = 5,
};

// Default logger for tunnels where the caller hasn't set one yet.
void noop_logger(void*, int, char const*);

// Honor-OBN_BAMBUSOURCE_LOG_LEVEL formatter. Drops messages below the
// configured threshold for both the file mirror and the Studio
// callback (so OBN_BAMBUSOURCE_LOG_LEVEL=debug enables extra detail
// without spamming Studio's own log when set higher).
[[gnu::format(printf, 4, 5)]]
void log_at(LogLevel lvl, Logger logger, void* ctx, const char* fmt, ...);

// Backwards-compatible default (INFO) so existing call sites keep
// working unchanged. New code should prefer log_at() with an explicit
// level when the message is debug/warn/error in nature.
[[gnu::format(printf, 3, 4)]]
void log_fmt(Logger logger, void* ctx, const char* fmt, ...);

// Threshold derived from OBN_BAMBUSOURCE_LOG_LEVEL. Cached on first use.
LogLevel current_log_level();

// Writes `msg` into a thread-local string accessed by
// Bambu_GetLastErrorMsg. Pass nullptr to clear.
void set_last_error(const char* msg);
const char* get_last_error();

// File handle for the on-disk mirror or nullptr if disabled. The same
// FILE* is reused process-wide; backends write a single line per
// emit, no synchronisation beyond stdio's internal lock.
FILE* mirror_log_fp();

} // namespace obn::source
