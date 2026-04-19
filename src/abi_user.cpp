#include <string>

#include "obn/abi_export.hpp"
#include "obn/agent.hpp"
#include "obn/bambu_networking.hpp"

using obn::as_agent;

OBN_ABI int bambu_network_change_user(void* /*agent*/, std::string /*user_info*/)
{
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI bool bambu_network_is_user_login(void* /*agent*/)
{
    return false;
}

OBN_ABI int bambu_network_user_logout(void* /*agent*/, bool /*request*/)
{
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI std::string bambu_network_get_user_id(void* /*agent*/)           { return {}; }
OBN_ABI std::string bambu_network_get_user_name(void* /*agent*/)         { return {}; }
OBN_ABI std::string bambu_network_get_user_avatar(void* /*agent*/)       { return {}; }
OBN_ABI std::string bambu_network_get_user_nickanme(void* /*agent*/)     { return {}; }

OBN_ABI std::string bambu_network_build_login_cmd(void* /*agent*/)
{
    // The real plugin returns a JS snippet the Studio webview runs to start
    // its OAuth flow. Returning empty string means Studio will simply not
    // navigate anywhere - user still sees the login UI but nothing happens
    // when they click "Sign in". That's fine for the Phase 1 offline stub.
    return {};
}

OBN_ABI std::string bambu_network_build_logout_cmd(void* /*agent*/)      { return {}; }
OBN_ABI std::string bambu_network_build_login_info(void* /*agent*/)      { return "{}"; }

OBN_ABI int bambu_network_get_my_profile(void* /*agent*/,
                                         std::string  /*token*/,
                                         unsigned int* http_code,
                                         std::string*  http_body)
{
    if (http_code) *http_code = 0;
    if (http_body) http_body->clear();
    return BAMBU_NETWORK_ERR_INVALID_HANDLE;
}

OBN_ABI int bambu_network_get_my_token(void* /*agent*/,
                                       std::string  /*ticket*/,
                                       unsigned int* http_code,
                                       std::string*  http_body)
{
    if (http_code) *http_code = 0;
    if (http_body) http_body->clear();
    return BAMBU_NETWORK_ERR_INVALID_HANDLE;
}

OBN_ABI int bambu_network_get_user_info(void* /*agent*/, int* identifier)
{
    if (identifier) *identifier = 0;
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI std::string bambu_network_get_bambulab_host(void* agent)
{
    auto* a = as_agent(agent);
    if (!a) return "https://api.bambulab.com";
    return a->country_code() == "CN" ? "https://api.bambulab.cn"
                                     : "https://api.bambulab.com";
}

OBN_ABI std::string bambu_network_get_user_selected_machine(void* agent)
{
    auto* a = as_agent(agent);
    return a ? a->user_selected_machine() : std::string{};
}

OBN_ABI int bambu_network_set_user_selected_machine(void* agent, std::string dev_id)
{
    if (auto* a = as_agent(agent)) {
        a->set_user_selected_machine(std::move(dev_id));
        return BAMBU_NETWORK_SUCCESS;
    }
    return BAMBU_NETWORK_ERR_INVALID_HANDLE;
}
