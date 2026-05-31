#include <string>

#include "obn/abi_export.hpp"
#include "obn/agent.hpp"
#include "obn/bambu_networking.hpp"
#ifndef OBN_LAN_ONLY
#include "obn/cloud_filament.hpp"
#endif

using obn::as_agent;

// ABI shims for the Filament Manager cloud endpoints. The work happens
// inside `obn::cloud_filament::*`; this file just resolves the agent
// pointer and forwards.

OBN_ABI int bambu_network_get_filament_spools(void* agent,
                                              BBL::FilamentQueryParams params,
                                              std::string* http_body)
{
#ifdef OBN_LAN_ONLY
    (void)agent; (void)params;
    if (http_body) http_body->clear();
    return BAMBU_NETWORK_ERR_INVALID_HANDLE;
#else
    auto* a = as_agent(agent);
    if (!a) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return obn::cloud_filament::list(a, params, http_body);
#endif
}

OBN_ABI int bambu_network_create_filament_spool(void* agent,
                                                std::string request_body,
                                                std::string* http_body)
{
#ifdef OBN_LAN_ONLY
    (void)agent; (void)request_body;
    if (http_body) http_body->clear();
    return BAMBU_NETWORK_ERR_INVALID_HANDLE;
#else
    auto* a = as_agent(agent);
    if (!a) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return obn::cloud_filament::create(a, request_body, http_body);
#endif
}

OBN_ABI int bambu_network_update_filament_spool(void* agent,
                                                std::string spool_id,
                                                std::string request_body,
                                                std::string* http_body)
{
#ifdef OBN_LAN_ONLY
    (void)agent; (void)spool_id; (void)request_body;
    if (http_body) http_body->clear();
    return BAMBU_NETWORK_ERR_INVALID_HANDLE;
#else
    auto* a = as_agent(agent);
    if (!a) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return obn::cloud_filament::update(a, spool_id, request_body, http_body);
#endif
}

OBN_ABI int bambu_network_delete_filament_spools(void* agent,
                                                 BBL::FilamentDeleteParams params,
                                                 std::string* http_body)
{
#ifdef OBN_LAN_ONLY
    (void)agent; (void)params;
    if (http_body) http_body->clear();
    return BAMBU_NETWORK_ERR_INVALID_HANDLE;
#else
    auto* a = as_agent(agent);
    if (!a) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return obn::cloud_filament::batch_delete(a, params, http_body);
#endif
}

OBN_ABI int bambu_network_get_filament_config(void* agent,
                                              std::string* http_body)
{
#ifdef OBN_LAN_ONLY
    (void)agent;
    if (http_body) http_body->clear();
    return BAMBU_NETWORK_ERR_INVALID_HANDLE;
#else
    auto* a = as_agent(agent);
    if (!a) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return obn::cloud_filament::config(a, http_body);
#endif
}
