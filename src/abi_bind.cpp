#include <string>
#include <vector>

#include "obn/abi_export.hpp"
#include "obn/agent.hpp"
#include "obn/bambu_networking.hpp"
#ifndef OBN_LAN_ONLY
#include "obn/bind_cloud.hpp"
#endif
#include "obn/log.hpp"

using obn::as_agent;

OBN_ABI int bambu_network_ping_bind(void* agent, std::string ping_code)
{
#ifdef OBN_LAN_ONLY
    (void)agent; (void)ping_code;
    return BAMBU_NETWORK_ERR_BIND_FAILED;
#else
    auto* a = as_agent(agent);
    if (!a) return BAMBU_NETWORK_ERR_BIND_FAILED;
    OBN_INFO("ping_bind code.len=%zu", ping_code.size());
    return obn::cloud_bind::ping_bind(a, ping_code);
#endif
}

OBN_ABI int bambu_network_bind_detect(void*       agent,
                                      std::string dev_ip,
                                      std::string sec_link,
                                      BBL::detectResult& detect)
{
    auto* a = as_agent(agent);
    if (!a) return -1;
    OBN_INFO("bind_detect dev_ip=%s sec_link=%s", dev_ip.c_str(), sec_link.c_str());
    // Wait ~4.5s for an SSDP NOTIFY from the printer on UDP :2021. No access
    // code is available in this ABI — same as the stock plugin, which also
    // discovers identity from LAN broadcast rather than MQTT here.
    return a->lookup_bind_detect(dev_ip, detect, 4500);
}

OBN_ABI int bambu_network_bind(void*       agent,
                               std::string dev_ip,
                               std::string dev_id,
                               std::string sec_link,
                               std::string timezone,
                               bool        improved,
                               BBL::OnUpdateStatusFn update_fn)
{
#ifdef OBN_LAN_ONLY
    (void)agent; (void)dev_ip; (void)dev_id; (void)sec_link;
    (void)timezone; (void)improved; (void)update_fn;
    return BAMBU_NETWORK_ERR_BIND_FAILED;
#else
    auto* a = as_agent(agent);
    if (!a) return BAMBU_NETWORK_ERR_BIND_FAILED;
    OBN_INFO("bind dev_ip=%s dev_id=%s sec_link=%s tz=%s improved=%d",
             dev_ip.c_str(),
             dev_id.c_str(),
             sec_link.c_str(),
             timezone.c_str(),
             improved);
    return obn::cloud_bind::bind_lan_to_account(
        a, dev_ip, dev_id, sec_link, timezone, improved, std::move(update_fn));
#endif
}

OBN_ABI int bambu_network_unbind(void* agent, std::string dev_id)
{
#ifdef OBN_LAN_ONLY
    (void)agent; (void)dev_id;
    return BAMBU_NETWORK_ERR_UNBIND_FAILED;
#else
    auto* a = as_agent(agent);
    if (!a) return BAMBU_NETWORK_ERR_UNBIND_FAILED;
    OBN_INFO("unbind dev_id=%s", dev_id.c_str());
    return obn::cloud_bind::unbind_device(a, dev_id);
#endif
}

OBN_ABI int bambu_network_request_bind_ticket(void* agent, std::string* ticket)
{
#ifdef OBN_LAN_ONLY
    (void)agent;
    if (ticket) ticket->clear();
    return BAMBU_NETWORK_ERR_INVALID_RESULT;
#else
    auto* a = as_agent(agent);
    if (!a) return BAMBU_NETWORK_ERR_INVALID_RESULT;
    OBN_DEBUG("request_bind_ticket");
    return obn::cloud_bind::request_web_sso_ticket(a, ticket);
#endif
}

OBN_ABI int bambu_network_query_bind_status(void* agent,
                                            std::vector<std::string> query_list,
                                            unsigned int* http_code,
                                            std::string* http_body)
{
#ifdef OBN_LAN_ONLY
    (void)agent; (void)query_list;
    if (http_code) *http_code = 0;
    if (http_body) http_body->clear();
    return BAMBU_NETWORK_ERR_QUERY_BIND_INFO_FAILED;
#else
    auto* a = as_agent(agent);
    if (!a) return BAMBU_NETWORK_ERR_QUERY_BIND_INFO_FAILED;
    OBN_DEBUG("query_bind_status count=%zu", query_list.size());
    return obn::cloud_bind::query_bind_status(a, query_list, http_code, http_body);
#endif
}

OBN_ABI int bambu_network_modify_printer_name(void*       agent,
                                               std::string dev_id,
                                               std::string dev_name)
{
#ifdef OBN_LAN_ONLY
    (void)agent; (void)dev_id; (void)dev_name;
    return BAMBU_NETWORK_ERR_MODIFY_PRINTER_NAME_FAILED;
#else
    auto* a = as_agent(agent);
    if (!a) return BAMBU_NETWORK_ERR_MODIFY_PRINTER_NAME_FAILED;
    OBN_INFO("modify_printer_name dev=%s name=%s", dev_id.c_str(), dev_name.c_str());
    return obn::cloud_bind::modify_printer_name(a, dev_id, dev_name);
#endif
}

OBN_ABI int bambu_network_report_consent(void* /*agent*/, std::string expand)
{
    OBN_DEBUG("report_consent %s", expand.c_str());
    return BAMBU_NETWORK_SUCCESS;
}
