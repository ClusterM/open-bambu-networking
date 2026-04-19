#include <string>
#include <vector>

#include "obn/abi_export.hpp"
#include "obn/agent.hpp"
#include "obn/bambu_networking.hpp"
#include "obn/log.hpp"

using obn::as_agent;

OBN_ABI int bambu_network_ping_bind(void* /*agent*/, std::string ping_code)
{
    OBN_INFO("ping_bind code=%s", ping_code.c_str());
    return BAMBU_NETWORK_ERR_BIND_FAILED;
}

OBN_ABI int bambu_network_bind_detect(void*       /*agent*/,
                                      std::string dev_ip,
                                      std::string sec_link,
                                      BBL::detectResult& detect)
{
    OBN_INFO("bind_detect dev_ip=%s sec_link=%s", dev_ip.c_str(), sec_link.c_str());
    // Until Phase 3 (SSDP) lands we can't auto-discover the printer's serial
    // from IP alone. Returning -5 (BIND_FAILED) makes Studio's
    // InputIpAddressDialog silently eat the error and leave the "Connect"
    // button greyed out forever (see ReleaseNote.cpp workerThreadFunc branch
    // on `result < 0`). Returning -3 instead triggers
    // EVT_CHECK_IP_ADDRESS_LAYOUT(1), which swaps the dialog to the manual
    // "enter SN" step so the user can actually proceed.
    detect.command      = "bind_detect";
    detect.dev_id       = {};
    detect.dev_name     = {};
    detect.model_id     = {};
    detect.version      = {};
    detect.bind_state   = "free";
    detect.connect_type = "lan";
    detect.result_msg   = "obn: bind_detect not yet implemented (SSDP pending)";
    OBN_WARN("bind_detect: returning -3 to force manual-SN fallback "
             "(SSDP auto-discovery lands in Phase 3)");
    return -3;
}

OBN_ABI int bambu_network_bind(void*       /*agent*/,
                               std::string dev_ip,
                               std::string dev_id,
                               std::string sec_link,
                               std::string timezone,
                               bool        improved,
                               BBL::OnUpdateStatusFn update_fn)
{
    OBN_INFO("bind dev_ip=%s dev_id=%s sec_link=%s tz=%s improved=%d",
             dev_ip.c_str(), dev_id.c_str(), sec_link.c_str(),
             timezone.c_str(), improved);
    // Keep the UI responsive: Studio's BindJob blocks on update_fn reporting
    // a terminal status. Without these callbacks the dialog would spin
    // forever (which is exactly what the user observed in full LAN mode).
    if (update_fn) {
        update_fn(BBL::LoginStageConnect, BAMBU_NETWORK_ERR_BIND_FAILED,
                  "obn bind stub: reachable-check not implemented");
        update_fn(BBL::LoginStageFinished, BAMBU_NETWORK_ERR_BIND_FAILED,
                  "obn bind stub: not implemented");
    }
    return BAMBU_NETWORK_ERR_BIND_FAILED;
}

OBN_ABI int bambu_network_unbind(void* /*agent*/, std::string dev_id)
{
    OBN_INFO("unbind dev_id=%s", dev_id.c_str());
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_request_bind_ticket(void* /*agent*/, std::string* ticket)
{
    OBN_DEBUG("request_bind_ticket");
    if (ticket) ticket->clear();
    return BAMBU_NETWORK_ERR_INVALID_RESULT;
}

OBN_ABI int bambu_network_query_bind_status(void* /*agent*/,
                                            std::vector<std::string> query_list,
                                            unsigned int* http_code,
                                            std::string*  http_body)
{
    OBN_DEBUG("query_bind_status count=%zu", query_list.size());
    if (http_code) *http_code = 0;
    if (http_body) http_body->clear();
    return BAMBU_NETWORK_ERR_QUERY_BIND_INFO_FAILED;
}

OBN_ABI int bambu_network_modify_printer_name(void* /*agent*/,
                                              std::string dev_id,
                                              std::string dev_name)
{
    OBN_INFO("modify_printer_name dev=%s name=%s", dev_id.c_str(), dev_name.c_str());
    return BAMBU_NETWORK_ERR_MODIFY_PRINTER_NAME_FAILED;
}

OBN_ABI int bambu_network_report_consent(void* /*agent*/, std::string expand)
{
    OBN_DEBUG("report_consent %s", expand.c_str());
    return BAMBU_NETWORK_SUCCESS;
}
