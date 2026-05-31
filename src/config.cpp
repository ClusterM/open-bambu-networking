#include "obn/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace obn::config {
namespace {

std::mutex g_mu;
Settings   g_current;

std::string trim(const std::string& s)
{
    std::size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    std::size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string to_lower(const std::string& s)
{
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return r;
}

bool parse_line(const std::string& line, std::string& key, std::string& val)
{
    key.clear();
    val.clear();
    if (line.empty() || line[0] == '#') return false;
    const auto eq = line.find('=');
    if (eq == std::string::npos || eq == 0) return false;
    key = trim(line.substr(0, eq));
    val = trim(line.substr(eq + 1));
    return !key.empty();
}

void apply_key(Settings& out, const std::string& key, const std::string& val)
{
    if      (key == "log_level")      out.log_level = val;
    else if (key == "log_stderr")     out.log_stderr = val;
    else if (key == "log_to_file")    out.log_to_file = val;
    else if (key == "log_file")       out.log_file = val;
#ifndef OBN_LAN_ONLY
    else if (key == "cloud_api_host") out.cloud_api_host = val;
    else if (key == "cloud_web_host") out.cloud_web_host = val;
    else if (key == "cloud_mqtt_host") out.cloud_mqtt_host = val;
#endif
    else if (key == "lan_tls_skip_verify")      out.lan_tls_skip_verify = truthy(val);
#ifndef OBN_LAN_ONLY
    else if (key == "cloud_mqtt_port") {
        int p = std::atoi(val.c_str());
        if (p > 0 && p <= 65535) out.cloud_mqtt_port = p;
    }
    else if (key == "block_cloud")              out.block_cloud = truthy(val);
#endif
    else if (key == "force_timelapse_external")  out.force_timelapse_external = truthy(val);
    else if (key == "bambusource_log_level")     out.bambusource_log_level = val;
    else if (key == "bambusource_log_file")      out.bambusource_log_file = val;
}

Settings parse_file(const std::filesystem::path& path)
{
    Settings out;
    std::ifstream in(path);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        std::string key, val;
        if (!parse_line(line, key, val)) continue;
        apply_key(out, key, val);
    }
    return out;
}

bool write_default_template(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    const std::filesystem::path tmp = path.string() + ".tmp";
    std::ofstream f(tmp, std::ios::binary);
    if (!f) return false;

    f << "## Open Bambu Networking — user settings\n"
      << "##\n"
      << "## Lines starting with ## are comments.\n"
      << "## Lines starting with a single # are disabled settings;\n"
      << "## remove the # to enable them.\n"
      << "##\n"
      << "## Log settings can also be set via OBN_LOG_* environment variables;\n"
      << "## when set, they take priority over this file.\n"
      << "\n"
      << "## ─── Logging ────────────────────────────────────────────────\n"
      << "\n"
      << "log_level = info\n"
      << "log_stderr = 1\n"
      << "log_to_file = 0\n"
      << "# log_file = /absolute/path/to/obn.log\n"
      << "\n"
#ifndef OBN_LAN_ONLY
      << "# --- Cloud endpoints ---\n"
      << "# Leave empty for production US/CN by country_code.\n"
      << "# Production global: https://api.bambulab.com / https://bambulab.com / us.mqtt.bambulab.com\n"
      << "# Production CN:      https://api.bambulab.cn / https://bambulab.cn / cn.mqtt.bambulab.com\n"
      << "# Dev/QA (Studio):    https://api-dev.bambulab.net / https://api-qa.bambulab.net / ...\n"
#endif
#ifndef OBN_LAN_ONLY
      << "# cloud_api_host = https://api.bambulab.com\n"
      << "# cloud_web_host = https://bambulab.com\n"
      << "# cloud_mqtt_host = us.mqtt.bambulab.com\n"
#endif
      << "\n"
      << "# Cloud MQTT port (default 8883).\n"
      << "# cloud_mqtt_port = 8883\n"
      << "\n"
      << "## ─── Cloud access ────────────────────────────────────────\n"
      << "##\n"
      << "## Block background cloud MQTT/REST connections.\n"
      << "## Auth, preset sync, and bind/unbind are still allowed.\n"
      << "\n"
      << "# block_cloud = 1\n"
      << "\n"
      << "## ─── LAN TLS ─────────────────────────────────────────────\n"
      << "##\n"
      << "## Skip TLS certificate verification for LAN MQTT/FTPS connections.\n"
      << "\n"
      << "# lan_tls_skip_verify = 0\n"
      << "\n"
      << "## ─── Print behavior ──────────────────────────────────────\n"
      << "##\n"
      << "## Always save timelapse to external storage (USB/SD), ignoring the\n"
      << "## Internal/External toggle in the print dialog.  Studio defaults\n"
      << "## that toggle to internal storage, so this avoids switching it every time.\n"
      << "\n"
      << "# force_timelapse_external = 0\n"
      << "\n"
      << "## ─── BambuSource logging ──────────────────────────────────\n"
      << "##\n"
      << "## Separate log level/file for the BambuSource (video/CTRL) library.\n"
      << "## Only read if BambuSource is loaded; file is never created by it.\n"
      << "\n"
      << "# bambusource_log_level = info\n"
      << "# bambusource_log_file = /absolute/path/to/bambusource.log\n";

    if (!f) {
        f.close();
        std::filesystem::remove(tmp, ec);
        return false;
    }
    f.close();

#if defined(_WIN32)
    std::filesystem::remove(path, ec);
#endif
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

std::filesystem::path config_path(const std::string& config_dir)
{
    std::string dir = config_dir;
    if (dir.empty()) return {};
    char last = dir.back();
    if (last != '/' && last != '\\') dir += '/';
    return std::filesystem::path(dir) / kConfigFileName;
}

} // namespace

bool truthy(const std::string& val, bool fallback)
{
    const std::string lc = to_lower(val);
    if (lc == "1" || lc == "true" || lc == "yes") return true;
    if (lc == "0" || lc == "false" || lc == "no") return false;
    return fallback;
}

Settings load_or_create(const std::string& config_dir)
{
    std::lock_guard<std::mutex> lk(g_mu);
    if (config_dir.empty()) return g_current;

    const auto path = config_path(config_dir);
    if (path.empty()) return g_current;

    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
        (void)write_default_template(path);

    g_current = parse_file(path);
    return g_current;
}

Settings load_if_exists(const std::string& config_dir)
{
    if (config_dir.empty()) return {};
    const auto path = config_path(config_dir);
    if (path.empty()) return {};
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return {};
    return parse_file(path);
}

const Settings& current()
{
    std::lock_guard<std::mutex> lk(g_mu);
    return g_current;
}

} // namespace obn::config
