#include "obn/cover_cache.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <vector>

#include "obn/ftps.hpp"
#include "obn/log.hpp"
#include "obn/zip_reader.hpp"

namespace fs = std::filesystem;

namespace obn::cover_cache {

namespace {

// FNV-1a 32-bit: tiny, deterministic and the same across platforms.
// Collision risk at the 2^16 names scale is irrelevant for a local
// cover cache.
std::uint32_t fnv1a_32(const std::string& s)
{
    std::uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

// Tracks (name+plate) combos currently being fetched so back-to-back
// push_status frames don't spawn duplicate FTPS sessions.
std::mutex                g_inflight_mu;
std::unordered_set<std::string> g_inflight;

std::string inflight_key(const std::string& name, int plate)
{
    return std::to_string(plate) + "\x01" + name;
}

// Drops a name into the inflight set and returns true if it wasn't
// already there (the caller "won" and should do the fetch).
bool claim_inflight(const std::string& key)
{
    std::lock_guard<std::mutex> lk(g_inflight_mu);
    return g_inflight.insert(key).second;
}

void release_inflight(const std::string& key)
{
    std::lock_guard<std::mutex> lk(g_inflight_mu);
    g_inflight.erase(key);
}

bool download_ftps_file(const std::string& host,
                        const std::string& user,
                        const std::string& password,
                        const std::string& ca_file,
                        const std::string& remote_path,
                        std::vector<std::uint8_t>* out)
{
    obn::ftps::ConnectConfig cfg;
    cfg.host     = host;
    cfg.username = user.empty() ? std::string{"bblp"} : user;
    cfg.password = password;
    cfg.ca_file  = ca_file;

    obn::ftps::Client c;
    if (std::string err = c.connect(cfg); !err.empty()) {
        OBN_DEBUG("cover_cache: ftps connect %s: %s",
                  host.c_str(), err.c_str());
        return false;
    }
    out->clear();
    out->reserve(4 * 1024 * 1024);
    std::string err = c.retr(remote_path,
        [&](const void* data, std::size_t len) {
            const auto* p = static_cast<const std::uint8_t*>(data);
            out->insert(out->end(), p, p + len);
            // Hard cap: .3mf shouldn't exceed 200 MB; if it does we
            // stop and log, since any cover is better than OOM.
            return out->size() < static_cast<std::size_t>(200) << 20;
        });
    c.quit();
    if (!err.empty()) {
        OBN_DEBUG("cover_cache: retr %s: %s", remote_path.c_str(), err.c_str());
        return false;
    }
    return !out->empty();
}

// Writes `bytes` to `final_path` atomically via a .tmp sibling + rename.
// rename(2) / std::filesystem::rename is atomic on the same volume,
// which is what we need so wxWebRequest never observes a half-written
// PNG.
bool write_atomic(const fs::path& final_path,
                  const std::vector<std::uint8_t>& bytes)
{
    fs::path tmp = final_path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, final_path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// Runs on a detached worker. Everything it touches is either local or
// behind a mutex; it does not touch any Agent state.
void fetch_worker(std::string host,
                  std::string user,
                  std::string password,
                  std::string ca_file,
                  std::string subtask_name,
                  int         plate_idx,
                  std::string inflight)
{
    struct ScopeRelease {
        std::string key;
        ~ScopeRelease() { release_inflight(key); }
    } release{inflight};

    // Pick a .3mf filename candidate list. Bambu names are inconsistent
    // between firmware and Studio versions:
    //   * New: "<subtask_name>.gcode.3mf" uploaded via Studio's
    //     SelectMachineDialog / "Send to Printer" flow.
    //   * Old/export: "<subtask_name>.3mf".
    // HA (ha_bambu_lab/.../models.py ~line 1296) does the same fan-out.
    std::vector<std::string> candidates;
    auto push_candidate = [&](std::string name) {
        if (!name.empty()) candidates.push_back("/cache/" + name);
    };
    // If subtask_name already ends in an extension, trust it.
    auto ends_with = [](const std::string& s, const std::string& suf) {
        return s.size() >= suf.size() &&
               std::equal(suf.rbegin(), suf.rend(), s.rbegin());
    };
    if (ends_with(subtask_name, ".3mf")) {
        push_candidate(subtask_name);
    } else {
        push_candidate(subtask_name + ".gcode.3mf");
        push_candidate(subtask_name + ".3mf");
    }

    std::vector<std::uint8_t> zipbuf;
    for (const auto& c : candidates) {
        zipbuf.clear();
        if (download_ftps_file(host, user, password, ca_file, c, &zipbuf)) {
            OBN_DEBUG("cover_cache: fetched %s (%zu bytes)",
                      c.c_str(), zipbuf.size());
            break;
        }
    }
    if (zipbuf.empty()) {
        OBN_DEBUG("cover_cache: no .3mf found for subtask '%s'",
                  subtask_name.c_str());
        return;
    }

    std::vector<obn::zip::Entry> dir;
    if (!obn::zip::read_central(zipbuf, &dir)) {
        OBN_DEBUG("cover_cache: zip central dir parse failed (%zu bytes)",
                  zipbuf.size());
        return;
    }

    // Extract plate_<N>.png with a fallback to plate_1.png. Bambu's
    // plate images are stored at both Metadata/plate_N.png and
    // Metadata/plate_no_light_N.png (the latter is dimmer); prefer the
    // lit version.
    auto try_names = [&](int idx) -> std::vector<std::string> {
        std::string n = std::to_string(idx);
        return {
            "Metadata/plate_" + n + ".png",
            "Metadata/plate_no_light_" + n + ".png",
        };
    };
    std::vector<obn::zip::Entry> candidates_entries;
    for (int idx : {plate_idx, 1}) {
        if (idx < 1) continue;
        for (const auto& n : try_names(idx)) {
            if (const auto* e = obn::zip::find(dir, n)) {
                candidates_entries.push_back(*e);
                break;
            }
        }
        if (!candidates_entries.empty()) break;
    }
    if (candidates_entries.empty()) {
        OBN_DEBUG("cover_cache: no plate png in zip for subtask '%s'",
                  subtask_name.c_str());
        return;
    }

    std::vector<std::uint8_t> png;
    if (!obn::zip::extract(zipbuf, candidates_entries.front(), &png) ||
        png.empty()) {
        OBN_DEBUG("cover_cache: zip extract failed for '%s'",
                  subtask_name.c_str());
        return;
    }

    fs::path out = path_for(subtask_name, plate_idx);
    if (!write_atomic(out, png)) {
        OBN_DEBUG("cover_cache: write %s failed", out.string().c_str());
        return;
    }
    OBN_INFO("cover_cache: wrote %s (%zu bytes)",
             out.string().c_str(), png.size());
}

} // namespace

std::string temp_dir()
{
    if (const char* v = std::getenv("OBN_COVER_DIR")) {
        if (*v) return v;
    }
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec);
    if (ec || base.empty()) {
#ifdef _WIN32
        base = "C:\\Windows\\Temp";
#else
        base = "/tmp";
#endif
    }
    fs::path dir = base / "obn-covers";
    fs::create_directories(dir, ec);
    return dir.string();
}

std::string path_for(const std::string& subtask_name, int plate_idx)
{
    if (subtask_name.empty()) return {};
    std::uint32_t h = fnv1a_32(subtask_name);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "cover-%08x-p%d.png", h,
                  plate_idx > 0 ? plate_idx : 1);
    return (fs::path(temp_dir()) / buf).string();
}

void ensure(const std::string& host,
            const std::string& user,
            const std::string& password,
            const std::string& ca_file,
            const std::string& subtask_name,
            int                plate_idx)
{
    if (host.empty() || subtask_name.empty()) return;

    std::string target = path_for(subtask_name, plate_idx);
    if (target.empty()) return;
    std::error_code ec;
    if (fs::exists(target, ec) && !ec) {
        auto sz = fs::file_size(target, ec);
        if (!ec && sz > 0) return; // already cached
    }

    std::string key = inflight_key(subtask_name, plate_idx);
    if (!claim_inflight(key)) return; // another worker is already on it.

    std::thread(fetch_worker, host, user, password, ca_file,
                subtask_name, plate_idx, std::move(key)).detach();
}

} // namespace obn::cover_cache
