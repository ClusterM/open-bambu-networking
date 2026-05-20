#include "obn/lan_tls.hpp"

#include "obn/lan_tls_env.hpp"
#include "obn/log.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include <filesystem>

#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <cerrno>
#  include <unistd.h>
#endif

namespace obn::lan_tls {
namespace {

std::mutex g_mu;
std::string g_ca_file;
std::unordered_map<std::string, std::string> g_ip_to_serial;
std::unordered_map<std::string, std::string> g_ip_to_peer_cert;
bool g_skip_warn_logged = false;

#if defined(_WIN32)
bool set_env_var(const char* key, const char* value)
{
    if (!key) return false;
    if (!value) value = "";
    const bool ok = ::SetEnvironmentVariableA(key, value) != 0;
    (void)::_putenv_s(key, value); // best-effort CRT sync for legacy getenv callers
    return ok;
}
#else
bool set_env_var(const char* key, const char* value)
{
    if (!key) return false;
    if (!value) value = "";
    return ::setenv(key, value, /*overwrite=*/1) == 0;
}
#endif

void sync_ca_env_locked()
{
    if (g_ca_file.empty()) {
        (void)set_env_var(kEnvCaFile, "");
        return;
    }
    if (!set_env_var(kEnvCaFile, g_ca_file.c_str())) {
        OBN_WARN("lan_tls: setenv(%s) failed", kEnvCaFile);
    }
}

void sync_ip_env_locked(const std::string& ip, const std::string& serial)
{
    const std::string key = env_key_for_ip(ip);
    if (!set_env_var(key.c_str(), serial.c_str())) {
        OBN_WARN("lan_tls: setenv(%s) failed", key.c_str());
    }
}

void sync_peer_env_locked(const std::string& ip, const std::string& path)
{
    const std::string key = peer_env_key_for_ip(ip);
    if (!set_env_var(key.c_str(), path.c_str())) {
        OBN_WARN("lan_tls: setenv(%s) failed", key.c_str());
    }
}

void warn_skip_once()
{
    if (g_skip_warn_logged) return;
    g_skip_warn_logged = true;
    OBN_WARN("OBN_SKIP_TLS_VERIFY set — printer TLS verification disabled");
}

} // namespace

const char* env_var_get(const char* key)
{
    if (!key || !*key) return nullptr;
#if defined(_WIN32)
    thread_local std::string buf;
    buf.assign(32768, '\0');
    const DWORD n = ::GetEnvironmentVariableA(
        key, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return nullptr;
    buf.resize(n);
    return buf.c_str();
#else
    const char* v = std::getenv(key);
    return (v && *v) ? v : nullptr;
#endif
}

bool verify_enabled()
{
    if (skip_verify_from_env()) {
        warn_skip_once();
        return false;
    }
    return true;
}

void registry_set_ca_file(const std::string& path)
{
    std::lock_guard<std::mutex> lk(g_mu);
    g_ca_file = path;
    sync_ca_env_locked();
    OBN_DEBUG("lan_tls: ca_file=%s", path.c_str());
}

void registry_put_ip_serial(const std::string& ip, const std::string& serial)
{
    if (ip.empty() || serial.empty()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_ip_to_serial[ip] = serial;
    sync_ip_env_locked(ip, serial);
    OBN_DEBUG("lan_tls: ip=%s serial=%s", ip.c_str(), serial.c_str());
}

void registry_set_peer_cert(const std::string& ip, const std::string& path)
{
    if (ip.empty()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (path.empty()) {
        g_ip_to_peer_cert.erase(ip);
        sync_peer_env_locked(ip, "");
        return;
    }
    g_ip_to_peer_cert[ip] = path;
    sync_peer_env_locked(ip, path);
    OBN_DEBUG("lan_tls: ip=%s peer_cert=%s", ip.c_str(), path.c_str());
}

bool configure_lan_ssl_verify(SSL_CTX*           ctx,
                              const std::string& ca_file,
                              const std::string& peer_cert_file,
                              std::string*       err)
{
    if (!ctx) {
        if (err) *err = "null SSL_CTX";
        return false;
    }
    if (ca_file.empty()) {
        if (err) *err = "empty ca_file";
        return false;
    }
    if (::SSL_CTX_load_verify_locations(ctx, ca_file.c_str(), nullptr) != 1) {
        if (err) *err = "load_verify_locations(ca)";
        return false;
    }
    if (!peer_cert_file.empty()) {
        if (::SSL_CTX_load_verify_locations(ctx, peer_cert_file.c_str(), nullptr) != 1) {
            if (err) *err = "load_verify_locations(peer)";
            return false;
        }
    }
    if (X509_VERIFY_PARAM* vpm = ::SSL_CTX_get0_param(ctx)) {
        const unsigned long flags = ::X509_VERIFY_PARAM_get_flags(vpm);
        ::X509_VERIFY_PARAM_set_flags(vpm, flags | X509_V_FLAG_PARTIAL_CHAIN);
    }
    ::SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    return true;
}

std::string read_pem_file(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string merged_trust_bundle_path(const std::string& ca_file,
                                     const std::string& peer_cert_file)
{
    if (peer_cert_file.empty()) return ca_file;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(ca_file, ec)
        || !std::filesystem::is_regular_file(peer_cert_file, ec)) {
        return ca_file;
    }
    const std::string ca   = read_pem_file(ca_file);
    const std::string peer = read_pem_file(peer_cert_file);
    if (ca.empty() || peer.empty()) return ca_file;

    std::ostringstream name;
    name << "obn-trust-"
#if !defined(_WIN32)
         << ::getpid() << '-'
#endif
         << std::chrono::steady_clock::now().time_since_epoch().count() << ".pem";
    std::filesystem::path out =
        std::filesystem::temp_directory_path(ec) / name.str();
    if (out.empty()) return ca_file;

    std::ofstream f(out, std::ios::binary);
    if (!f) return ca_file;
    f << ca;
    if (!ca.empty() && ca.back() != '\n') f << '\n';
    f << peer;
    if (!peer.empty() && peer.back() != '\n') f << '\n';
    if (!f) return ca_file;
    return out.string();
}

std::string registry_ca_file()
{
    std::lock_guard<std::mutex> lk(g_mu);
    return g_ca_file;
}

std::optional<std::string> registry_lookup_serial(const std::string& ip)
{
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_ip_to_serial.find(ip);
    if (it == g_ip_to_serial.end()) return std::nullopt;
    return it->second;
}

const char* resolve_lan_ca_file()
{
    return env_var_get(kEnvCaFile);
}

const char* resolve_lan_peer_cert(const char* ip, const char* /*serial*/)
{
    return peer_cert_path_for_ip(ip);
}

} // namespace obn::lan_tls
