#include <string>
#include <vector>

#include "obn/abi_export.hpp"
#include "obn/bambu_networking.hpp"
#include "obn/log.hpp"

// Filament spool management: endpoints not yet reverse-engineered.
// All functions log their arguments and return the corresponding stub
// error code so the caller can detect the missing implementation.

OBN_ABI int bambu_network_get_filament_spools(void* /*agent*/,
                                              BBL::FilamentQueryParams params,
                                              std::string* http_body)
{
    OBN_INFO("get_filament_spools: category='%s' status='%s' spool_id='%s' "
             "rfid='%s' offset=%d limit=%d",
             params.category.c_str(), params.status.c_str(),
             params.spool_id.c_str(), params.rfid.c_str(),
             params.offset, params.limit);
    if (http_body) http_body->clear();
    return BAMBU_NETWORK_ERR_GET_FILAMENTS_FAILED;
}

OBN_ABI int bambu_network_create_filament_spool(void* /*agent*/,
                                                std::string request_body,
                                                std::string* http_body)
{
    OBN_INFO("create_filament_spool: body_len=%zu body='%s'",
             request_body.size(), request_body.c_str());
    if (http_body) http_body->clear();
    return BAMBU_NETWORK_ERR_CREATE_FILAMENT_FAILED;
}

OBN_ABI int bambu_network_update_filament_spool(void* /*agent*/,
                                                std::string spool_id,
                                                std::string request_body,
                                                std::string* http_body)
{
    OBN_INFO("update_filament_spool: spool_id='%s' body_len=%zu body='%s'",
             spool_id.c_str(), request_body.size(), request_body.c_str());
    if (http_body) http_body->clear();
    return BAMBU_NETWORK_ERR_UPDATE_FILAMENT_FAILED;
}

OBN_ABI int bambu_network_delete_filament_spools(void* /*agent*/,
                                                 BBL::FilamentDeleteParams params,
                                                 std::string* http_body)
{
    std::string ids_str, rfids_str;
    for (const auto& id : params.ids)   { if (!ids_str.empty())   ids_str   += ','; ids_str   += id; }
    for (const auto& r  : params.rfids) { if (!rfids_str.empty()) rfids_str += ','; rfids_str += r;  }
    OBN_INFO("delete_filament_spools: ids=[%s] rfids=[%s]",
             ids_str.c_str(), rfids_str.c_str());
    if (http_body) http_body->clear();
    return BAMBU_NETWORK_ERR_DELETE_FILAMENT_FAILED;
}

OBN_ABI int bambu_network_get_filament_config(void* /*agent*/,
                                              std::string* http_body)
{
    OBN_INFO("get_filament_config");
    if (http_body) http_body->clear();
    return BAMBU_NETWORK_ERR_GET_FILAMENT_CONFIG_FAILED;
}
