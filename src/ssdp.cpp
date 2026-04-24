#include "obn/ssdp.hpp"

#include "obn/log.hpp"
#include "obn/platform.hpp"

#include <cerrno>
#include <cstring>
#include <sstream>
#include <string>

namespace obn::ssdp {

namespace {

std::string to_lower(std::string s)
{
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

// Strict-enough JSON string escaper: handles the cases we actually see in
// headers (UTF-8, quotes, backslash, control bytes). No fancy pretty
// printing; Studio just calls json::parse() on the result.
std::string json_escape(const std::string& in)
{
    std::string out;
    out.reserve(in.size() + 2);
    out.push_back('"');
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    // Pass UTF-8 bytes through untouched - json::parse in
                    // Studio is nlohmann::json which accepts UTF-8.
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

} // namespace

// -------------------------------------------------------------------
// Headers
// -------------------------------------------------------------------

void Headers::set(std::string key, std::string value)
{
    kv_.emplace(to_lower(std::move(key)), std::move(value));
}

const std::string* Headers::get(const std::string& lc_key) const
{
    auto it = kv_.find(lc_key);
    return it == kv_.end() ? nullptr : &it->second;
}

std::string Headers::value(const std::string& lc_key) const
{
    auto* p = get(lc_key);
    return p ? *p : std::string{};
}

// -------------------------------------------------------------------
// Parser
// -------------------------------------------------------------------

bool parse(const char* data, std::size_t size, Headers& out)
{
    if (!data || size < 4) return false;

    // Find the end of the start line.
    std::string msg(data, size);
    size_t sol_end = msg.find("\r\n");
    if (sol_end == std::string::npos) sol_end = msg.find('\n');
    if (sol_end == std::string::npos) return false;

    std::string start = msg.substr(0, sol_end);
    // Accept both "NOTIFY * HTTP/1.1" and "M-SEARCH * HTTP/1.1" start lines,
    // plus "HTTP/1.1 200 OK" responses. We don't actually need the method,
    // just validation, so a cheap prefix check suffices.
    if (start.find("HTTP/1.") == std::string::npos) return false;

    size_t pos = (sol_end < msg.size() && msg[sol_end] == '\r') ? sol_end + 2 : sol_end + 1;
    while (pos < msg.size()) {
        size_t eol = msg.find('\n', pos);
        if (eol == std::string::npos) eol = msg.size();
        size_t line_end = eol;
        if (line_end > pos && msg[line_end - 1] == '\r') --line_end;
        if (line_end == pos) { // blank line - end of headers
            break;
        }
        std::string line = msg.substr(pos, line_end - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string k = trim(line.substr(0, colon));
            std::string v = trim(line.substr(colon + 1));
            if (!k.empty()) out.set(std::move(k), std::move(v));
        }
        pos = eol + 1;
    }
    return true;
}

// -------------------------------------------------------------------
// JSON builder (matches DeviceManager::on_machine_alive expectations)
// -------------------------------------------------------------------

std::string to_device_info_json(const Headers& h)
{
    // Studio's DeviceManager::on_machine_alive() calls j[key].get<string>()
    // without checking for presence, so every field below must exist in the
    // emitted JSON. Missing values become "" which Studio tolerates.
    const std::string dev_id      = h.value("usn");
    const std::string dev_ip      = h.value("location");
    const std::string dev_type    = h.value("devmodel.bambu.com");
    const std::string dev_name    = h.value("devname.bambu.com");
    const std::string connect_t   = h.value("devconnect.bambu.com");
    const std::string bind_state  = h.value("devbind.bambu.com");
    const std::string sec_link    = h.value("devseclink.bambu.com");
    const std::string ssdp_ver    = h.value("devversion.bambu.com");
    const std::string conn_name   = h.value("devinf.bambu.com");

    std::ostringstream os;
    os << "{"
       << "\"dev_name\":"        << json_escape(dev_name)
       << ",\"dev_id\":"         << json_escape(dev_id)
       << ",\"dev_ip\":"         << json_escape(dev_ip)
       << ",\"dev_type\":"       << json_escape(dev_type)
       << ",\"dev_signal\":"     << json_escape("")
       << ",\"connect_type\":"   << json_escape(connect_t)
       << ",\"bind_state\":"     << json_escape(bind_state)
       << ",\"sec_link\":"       << json_escape(sec_link)
       << ",\"ssdp_version\":"   << json_escape(ssdp_ver)
       << ",\"connection_name\":"<< json_escape(conn_name)
       << "}";
    return os.str();
}

// -------------------------------------------------------------------
// Discovery
// -------------------------------------------------------------------

Discovery::Discovery() = default;

Discovery::~Discovery()
{
    stop();
}

bool Discovery::start(int port, OnMessage cb)
{
    if (running_.load(std::memory_order_acquire)) {
        // Already running; just update the callback so a re-binding doesn't
        // lose messages.
        std::lock_guard<std::mutex> lk(cb_mu_);
        cb_ = std::move(cb);
        return true;
    }

    port_ = port;
    {
        std::lock_guard<std::mutex> lk(cb_mu_);
        cb_ = std::move(cb);
    }

    obn::plat::ensure_sockets_initialised();

    obn::plat::socket_t fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (!obn::plat::is_valid_socket(fd)) {
        OBN_ERROR("ssdp: socket() failed: %s",
                  obn::plat::socket_strerror(obn::plat::socket_last_error()).c_str());
        return false;
    }

    int yes = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&yes), sizeof(yes)) < 0) {
        OBN_WARN("ssdp: SO_REUSEADDR failed: %s",
                 obn::plat::socket_strerror(obn::plat::socket_last_error()).c_str());
    }
    // Required on Linux to receive packets destined for 255.255.255.255 on
    // sockets bound to INADDR_ANY. Harmless on Windows.
    if (::setsockopt(fd, SOL_SOCKET, SO_BROADCAST,
                     reinterpret_cast<const char*>(&yes), sizeof(yes)) < 0) {
        OBN_WARN("ssdp: SO_BROADCAST failed: %s",
                 obn::plat::socket_strerror(obn::plat::socket_last_error()).c_str());
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        OBN_ERROR("ssdp: bind(:%d) failed: %s (another SSDP listener running?)",
                  port,
                  obn::plat::socket_strerror(obn::plat::socket_last_error()).c_str());
        obn::plat::close_socket(fd);
        return false;
    }

    // Also join the canonical SSDP multicast group for future-proofing (some
    // models also advertise on 239.255.255.250). Best-effort.
    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = ::inet_addr("239.255.255.250");
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                     reinterpret_cast<const char*>(&mreq), sizeof(mreq)) < 0) {
        OBN_DEBUG("ssdp: IP_ADD_MEMBERSHIP 239.255.255.250 failed (ignored): %s",
                  obn::plat::socket_strerror(obn::plat::socket_last_error()).c_str());
    }

    fd_ = fd;
    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this] { run_(); });
    OBN_INFO("ssdp: listening on 0.0.0.0:%d", port_);
    return true;
}

void Discovery::stop()
{
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;

    obn::plat::socket_t fd = fd_;
    fd_ = obn::plat::kInvalidSocket;
    // Closing the socket wakes up recvfrom() with EBADF / zero bytes.
#if defined(_WIN32)
    constexpr int kShutdownBoth = SD_BOTH;
#else
    constexpr int kShutdownBoth = SHUT_RDWR;
#endif
    if (obn::plat::is_valid_socket(fd)) ::shutdown(fd, kShutdownBoth);
    if (obn::plat::is_valid_socket(fd)) obn::plat::close_socket(fd);

    if (worker_.joinable()) worker_.join();

    std::lock_guard<std::mutex> lk(cb_mu_);
    cb_ = {};
    OBN_INFO("ssdp: stopped");
}

void Discovery::run_()
{
    constexpr std::size_t kBufSize = 4096;
    std::string buf;
    buf.resize(kBufSize);

    while (running_.load(std::memory_order_acquire)) {
        sockaddr_in src{};
        socklen_t   slen = sizeof(src);
        int n = ::recvfrom(fd_, buf.data(), static_cast<int>(kBufSize), 0,
                           reinterpret_cast<sockaddr*>(&src), &slen);
        if (n <= 0) {
            if (!running_.load(std::memory_order_acquire)) break;
            int e = obn::plat::socket_last_error();
            if (e == obn::plat::kEINTR) continue;
            if (e == obn::plat::kEBADF) break;
            OBN_WARN("ssdp: recvfrom failed: %s",
                     obn::plat::socket_strerror(e).c_str());
            break;
        }

        Headers h;
        if (!parse(buf.data(), static_cast<std::size_t>(n), h)) continue;

        // Filter out packets that aren't about a Bambu printer. Studio's
        // parser would throw on the missing fields, but we'd rather skip
        // them quietly than generate a useless callback per Google Chrome
        // DIAL M-SEARCH.
        const std::string* usn   = h.get("usn");
        const std::string* model = h.get("devmodel.bambu.com");
        if (!usn || !model || usn->empty() || model->empty()) continue;

        // If "Location" is missing (some firmwares omit it on responses),
        // fall back to the source IP.
        if (!h.get("location")) {
            char addrbuf[INET_ADDRSTRLEN]{};
            ::inet_ntop(AF_INET, &src.sin_addr, addrbuf, sizeof(addrbuf));
            h.set("Location", addrbuf);
        }

        std::string json = to_device_info_json(h);
        OBN_DEBUG("ssdp: printer seen usn=%s model=%s ip=%s name.len=%zu",
                  usn->c_str(), model->c_str(),
                  h.value("location").c_str(),
                  h.value("devname.bambu.com").size());

        OnMessage cb;
        {
            std::lock_guard<std::mutex> lk(cb_mu_);
            cb = cb_;
        }
        if (cb) cb(json);
    }
}

} // namespace obn::ssdp
