#include <map>
#include <string>

#include "obn/abi_export.hpp"
#include "obn/agent.hpp"
#include "obn/bambu_networking.hpp"

using obn::as_agent;

OBN_ABI std::string bambu_network_get_studio_info_url(void* /*agent*/)
{
    return {};
}

OBN_ABI int bambu_network_set_extra_http_header(void* agent,
                                                std::map<std::string, std::string> extra_headers)
{
    if (auto* a = as_agent(agent)) {
        a->set_extra_http_headers(std::move(extra_headers));
        return BAMBU_NETWORK_SUCCESS;
    }
    return BAMBU_NETWORK_ERR_INVALID_HANDLE;
}

OBN_ABI int bambu_network_get_my_message(void* /*agent*/,
                                         int /*type*/, int /*after*/, int /*limit*/,
                                         unsigned int* http_code, std::string* http_body)
{
    if (http_code) *http_code = 0;
    if (http_body) http_body->clear();
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_check_user_task_report(void* /*agent*/, int* task_id, bool* printable)
{
    if (task_id)   *task_id = 0;
    if (printable) *printable = false;
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_get_user_print_info(void* /*agent*/,
                                              unsigned int* http_code, std::string* http_body)
{
    if (http_code) *http_code = 0;
    if (http_body) http_body->clear();
    return BAMBU_NETWORK_ERR_GET_USER_PRINTINFO_FAILED;
}

OBN_ABI int bambu_network_get_user_tasks(void* /*agent*/,
                                         BBL::TaskQueryParams /*params*/,
                                         std::string* http_body)
{
    if (http_body) http_body->clear();
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_get_printer_firmware(void* /*agent*/,
                                               std::string /*dev_id*/,
                                               unsigned* http_code, std::string* http_body)
{
    if (http_code) *http_code = 0;
    if (http_body) http_body->clear();
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_get_task_plate_index(void* /*agent*/,
                                               std::string /*task_id*/, int* plate_index)
{
    if (plate_index) *plate_index = -1;
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_get_subtask_info(void* /*agent*/,
                                           std::string /*subtask_id*/,
                                           std::string* task_json,
                                           unsigned int* http_code,
                                           std::string*  http_body)
{
    if (task_json) task_json->clear();
    if (http_code) *http_code = 0;
    if (http_body) http_body->clear();
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_get_slice_info(void* /*agent*/,
                                         std::string /*project_id*/,
                                         std::string /*profile_id*/,
                                         int         /*plate_index*/,
                                         std::string* slice_json)
{
    if (slice_json) slice_json->clear();
    return BAMBU_NETWORK_SUCCESS;
}
