#include <map>
#include <string>

#include "obn/abi_export.hpp"
#include "obn/agent.hpp"
#include "obn/bambu_networking.hpp"

using obn::as_agent;

OBN_ABI int bambu_network_get_user_presets(
    void* /*agent*/,
    std::map<std::string, std::map<std::string, std::string>>* user_presets)
{
    if (user_presets) user_presets->clear();
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI std::string bambu_network_request_setting_id(
    void* /*agent*/,
    std::string /*name*/,
    std::map<std::string, std::string>* /*values_map*/,
    unsigned int* http_code)
{
    if (http_code) *http_code = 0;
    return {};
}

OBN_ABI int bambu_network_put_setting(void* /*agent*/,
                                      std::string /*setting_id*/,
                                      std::string /*name*/,
                                      std::map<std::string, std::string>* /*values_map*/,
                                      unsigned int* http_code)
{
    if (http_code) *http_code = 0;
    return BAMBU_NETWORK_ERR_PUT_SETTING_FAILED;
}

OBN_ABI int bambu_network_get_setting_list(void* /*agent*/,
                                           std::string /*bundle_version*/,
                                           BBL::ProgressFn       /*pro_fn*/,
                                           BBL::WasCancelledFn   /*cancel_fn*/)
{
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_get_setting_list2(void* /*agent*/,
                                            std::string /*bundle_version*/,
                                            BBL::CheckFn          /*chk_fn*/,
                                            BBL::ProgressFn       /*pro_fn*/,
                                            BBL::WasCancelledFn   /*cancel_fn*/)
{
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_delete_setting(void* /*agent*/, std::string /*setting_id*/)
{
    return BAMBU_NETWORK_SUCCESS;
}
