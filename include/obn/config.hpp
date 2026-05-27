#pragma once

#include <cstdlib>
#include <optional>
#include <string>

#ifndef OBN_CLOUD_API_URL_DEFAULT
#define OBN_CLOUD_API_URL_DEFAULT ""
#endif
#ifndef OBN_CLOUD_WEB_URL_DEFAULT
#define OBN_CLOUD_WEB_URL_DEFAULT ""
#endif
#ifndef OBN_CLOUD_MQTT_HOSTNAME_DEFAULT
#define OBN_CLOUD_MQTT_HOSTNAME_DEFAULT ""
#endif

namespace obn::config {

// Resolve config in this order of precedence: env var -> compiled default (CMake -D...)
inline std::optional<std::string> resolve_override(const char* env_var,
                                                   const char* compiled_default)
{
    if (const char* v = std::getenv(env_var); v && *v) return std::string(v);
    if (compiled_default && *compiled_default) return std::string(compiled_default);
    return std::nullopt;
}

} // namespace obn::config
