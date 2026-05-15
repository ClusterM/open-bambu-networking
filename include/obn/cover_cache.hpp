#pragma once

// Cover-image cache for the "currently printing" thumbnail in Studio's
// Device tab.
//
// Stock Studio fetches print covers via wxWebRequest over the cloud,
// keyed by a server-side subtask id. On LAN/Developer-Mode printers
// there is no cloud subtask, but the original .3mf is still sitting in
// the printer's /cache/ directory: we can pull it back over FTPS,
// crack the ZIP open, and hand Studio Metadata/plate_<N>.png directly.
//
// This service is the FTPS/ZIP half of the workaround. A small sibling
// HTTP server (cover_server.hpp) exposes the decoded PNGs on a random
// localhost port so wxWebRequest can fetch them the same way it fetches
// cloud covers.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace obn::cover_cache {

// Base scratch directory for cached cover PNGs. Honours $OBN_COVER_DIR
// when set; otherwise uses std::filesystem::temp_directory_path()/obn.
// On first access creates the directory if missing. Safe on any OS;
// std::filesystem handles TMPDIR/TEMP/TMP as the platform expects.
std::string temp_dir();

// Stable per-(subtask,plate,version) filename under temp_dir(). Filename
// is a hash of (subtask name + version) so it's filesystem-safe on every
// OS. Does not touch the filesystem.
//
// `version` is an opaque per-print token — typically `gcode_start_time`
// from the push_status frame — that lets us detect "same filename, new
// content" the user might have re-uploaded between prints. Pass an empty
// string when no version is known; behaviour then reduces to the legacy
// name-only key (kept for backward compatibility with frames that don't
// carry a start time).
std::string path_for(const std::string& subtask_name,
                     int                plate_idx,
                     const std::string& version = {});

// Background fetcher: if the file at path_for(...) doesn't already exist,
// spawn a detached thread that connects to `host` (FTP or implicit TLS per
// `use_ssl_for_ftp`, consistent with PrintParams) using the printer's LAN
// credentials (`user`/`password`), downloads
// /cache/<subtask_name>{.gcode.3mf,.3mf}, parses the ZIP central
// directory, extracts Metadata/plate_<plate_idx>.png (falling back to
// plate_1.png) and atomically writes it to disk.
//
// The function is idempotent and concurrency-safe: a second call for
// the same (name,plate,version) while a previous fetch is still in
// flight is a no-op. A different `version` for the same name forces a
// re-fetch even if a stale PNG is still on disk.
// `use_ssl_for_ftp` comes from the last PrintParams seen on this agent
// (see print_params_ftp_prefs).
void ensure(const std::string& host,
            const std::string& user,
            const std::string& password,
            const std::string& ca_file,
            bool               use_ssl_for_ftp,
            const std::string& subtask_name,
            int                plate_idx,
            const std::string& version = {});

} // namespace obn::cover_cache
