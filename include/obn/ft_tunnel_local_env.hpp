#pragma once

// Runtime OBN_FT_TUNNEL_LOCAL env override (shared by ft_* ABI and print path).

#include <cstdlib>

namespace obn::ft_tunnel_local {

inline bool env_truthy(const char* name, bool default_val)
{
    const char* v = std::getenv(name);
    if (!v || !*v) return default_val;
    return v[0] != '0';
}

inline bool runtime_enabled()
{
#if OBN_FT_TUNNEL_LOCAL
    return env_truthy("OBN_FT_TUNNEL_LOCAL", true);
#else
    return env_truthy("OBN_FT_TUNNEL_LOCAL", false);
#endif
}

} // namespace obn::ft_tunnel_local
