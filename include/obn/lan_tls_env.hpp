#pragma once

// Header-only ENV IPC between libbambu_networking and libBambuSource (same
// process, separate dlopen). Networking syncs registry -> setenv; BambuSource
// reads via env_var_get().
//
// Windows: registry writes use SetEnvironmentVariableA AND _putenv_s; reads use
// GetEnvironmentVariableA. Mixing SetEnvironmentVariable with getenv() does NOT
// work — the CRT cache is not updated (wait_env_serial would spin forever).

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

namespace obn::lan_tls {

// Platform-correct env read (GetEnvironmentVariableA on Windows, getenv elsewhere).
const char* env_var_get(const char* key);

inline constexpr const char* kEnvCaFile       = "OBN_LAN_TLS_CA_FILE";
inline constexpr const char* kEnvIpPrefix     = "OBN_LAN_TLS_IP_";
inline constexpr const char* kEnvPeerPrefix   = "OBN_LAN_TLS_PEER_";
inline constexpr const char* kEnvSkipVerify   = "OBN_SKIP_TLS_VERIFY";
inline constexpr const char* kEnvSerialWaitMs = "OBN_LAN_TLS_SERIAL_WAIT_MS";

inline constexpr int kDefaultSerialEnvWaitMs = 5000;
inline constexpr int kSerialEnvPollMs        = 100;

inline std::string env_key_for_ip_prefix(const std::string& ip,
                                         const char*        prefix)
{
    std::string key = prefix;
    for (char c : ip) {
        key += (c == '.') ? '_' : c;
    }
    return key;
}

inline std::string env_key_for_ip(const std::string& ip)
{
    return env_key_for_ip_prefix(ip, kEnvIpPrefix);
}

inline std::string peer_env_key_for_ip(const std::string& ip)
{
    return env_key_for_ip_prefix(ip, kEnvPeerPrefix);
}

inline const char* peer_cert_path_for_ip(const char* ip)
{
    if (!ip || !*ip) return nullptr;
    return env_var_get(peer_env_key_for_ip(ip).c_str());
}

inline int serial_env_wait_ms()
{
    const char* v = env_var_get(kEnvSerialWaitMs);
    if (!v || !*v) return kDefaultSerialEnvWaitMs;
    char* end = nullptr;
    long  ms  = std::strtol(v, &end, 10);
    if (end == v || ms < 0) return kDefaultSerialEnvWaitMs;
    if (ms > 60000) ms = 60000;
    return static_cast<int>(ms);
}

inline bool skip_verify_from_env()
{
    const char* v = env_var_get(kEnvSkipVerify);
    if (!v || !*v) return false;
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T';
}

// Poll env until non-empty or timeout_ms elapses.
inline const char* wait_env_serial(const char* ip, int timeout_ms)
{
    if (!ip || !*ip) return nullptr;
    const std::string key = env_key_for_ip(ip);
    const auto        deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (const char* v = env_var_get(key.c_str())) {
            if (*v) return v;
        }
        if (std::chrono::steady_clock::now() >= deadline) return nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(kSerialEnvPollMs));
    }
}

} // namespace obn::lan_tls
