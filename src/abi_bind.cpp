#include <string>
#include <vector>

#include "obn/abi_export.hpp"
#include "obn/agent.hpp"
#include "obn/bambu_networking.hpp"

using obn::as_agent;

OBN_ABI int bambu_network_ping_bind(void* /*agent*/, std::string /*ping_code*/)
{
    return BAMBU_NETWORK_ERR_BIND_FAILED;
}

OBN_ABI int bambu_network_bind_detect(void* /*agent*/,
                                      std::string /*dev_ip*/,
                                      std::string /*sec_link*/,
                                      BBL::detectResult& /*detect*/)
{
    return BAMBU_NETWORK_ERR_BIND_FAILED;
}

OBN_ABI int bambu_network_bind(void* /*agent*/,
                               std::string /*dev_ip*/,
                               std::string /*dev_id*/,
                               std::string /*sec_link*/,
                               std::string /*timezone*/,
                               bool        /*improved*/,
                               BBL::OnUpdateStatusFn /*update_fn*/)
{
    return BAMBU_NETWORK_ERR_BIND_FAILED;
}

OBN_ABI int bambu_network_unbind(void* /*agent*/, std::string /*dev_id*/)
{
    return BAMBU_NETWORK_ERR_UNBIND_FAILED;
}

OBN_ABI int bambu_network_request_bind_ticket(void* /*agent*/, std::string* ticket)
{
    if (ticket) ticket->clear();
    return BAMBU_NETWORK_ERR_INVALID_RESULT;
}

OBN_ABI int bambu_network_query_bind_status(void* /*agent*/,
                                            std::vector<std::string> /*query_list*/,
                                            unsigned int* http_code,
                                            std::string*  http_body)
{
    if (http_code) *http_code = 0;
    if (http_body) http_body->clear();
    return BAMBU_NETWORK_ERR_QUERY_BIND_INFO_FAILED;
}

OBN_ABI int bambu_network_modify_printer_name(void* /*agent*/,
                                              std::string /*dev_id*/,
                                              std::string /*dev_name*/)
{
    return BAMBU_NETWORK_ERR_MODIFY_PRINTER_NAME_FAILED;
}

OBN_ABI int bambu_network_report_consent(void* /*agent*/, std::string /*expand*/)
{
    return BAMBU_NETWORK_SUCCESS;
}
