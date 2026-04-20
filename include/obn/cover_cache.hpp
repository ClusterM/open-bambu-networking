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

// Stable per-(subtask,plate) filename under temp_dir(). Filename is a
// hash of the subtask name so it's filesystem-safe on every OS. Does
// not touch the filesystem.
std::string path_for(const std::string& subtask_name, int plate_idx);

// Background fetcher: if the file at path_for(...) doesn't already exist,
// spawn a detached thread that connects to `host` via FTPS using the
// printer's LAN credentials (`user`/`password`), downloads
// /cache/<subtask_name>{.gcode.3mf,.3mf}, parses the ZIP central
// directory, extracts Metadata/plate_<plate_idx>.png (falling back to
// plate_1.png) and atomically writes it to disk.
//
// The function is idempotent and concurrency-safe: a second call for
// the same (name,plate) while a previous fetch is still in flight is a
// no-op.
void ensure(const std::string& host,
            const std::string& user,
            const std::string& password,
            const std::string& ca_file,
            const std::string& subtask_name,
            int                plate_idx);

} // namespace obn::cover_cache
