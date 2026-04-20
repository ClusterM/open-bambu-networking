#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "obn/abi_export.hpp"
#include "obn/agent.hpp"
#include "obn/bambu_networking.hpp"
#include "obn/cloud_auth.hpp"
#include "obn/http_client.hpp"
#include "obn/json_lite.hpp"
#include "obn/log.hpp"

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

namespace {

// Serialize a json_lite value back to compact JSON text. We only need
// this for the pass-through "whatever was in the server's device
// entry" payload; json::Value::dump() does the heavy lifting for us.
std::string dump_or_null(const obn::json::Value& v)
{
    return v.is_null() ? std::string{"null"} : v.dump();
}

// The Bambu cloud returns device-bind info at
//   GET /v1/iot-service/api/user/bind
// with body shape:
//   { "devices":[{
//       "dev_id":"22E8BJ610801473",
//       "name":"3Д принтерик",
//       "online":true,
//       "print_status":"SUCCESS",
//       "dev_model_name":"N7-V2",
//       "dev_product_name":"P2S",
//       "dev_access_code":"03f06755",
//       ... }]}
// Studio's DeviceManager::parse_user_print_info however reads slightly
// different field names - {dev_name, dev_online, task_status}. We
// translate here so Studio's parser finds everything. Pass-through
// fields that Studio doesn't care about (print_job, nozzle_diameter,
// dev_structure...) are preserved verbatim in case anything else on
// the Studio side picks them up.
std::string remap_bind_payload(const std::string& raw_body,
                               std::vector<std::string>* out_dev_ids)
{
    std::string perr;
    auto root = obn::json::parse(raw_body, &perr);
    if (!root) {
        OBN_WARN("get_user_print_info: bad JSON from server: %s", perr.c_str());
        return R"({"devices":[]})";
    }

    std::ostringstream out;
    out << "{\"message\":\"success\",\"devices\":[";
    // Copy the devices array out of the temporary Value to avoid
    // dangling reference (as_array() returns a reference to storage
    // owned by the temporary returned from find()).
    auto devs_v = root->find("devices");
    const auto& devs = devs_v.as_array();
    bool first = true;
    for (const auto& d : devs) {
        if (!first) out << ',';
        first = false;
        out << '{';
        // Required by Studio's parser.
        const auto dev_id = d.find("dev_id").as_string();
        if (out_dev_ids && !dev_id.empty()) out_dev_ids->push_back(dev_id);
        out << "\"dev_id\":"          << obn::json::escape(dev_id) << ',';
        out << "\"dev_name\":"        << obn::json::escape(d.find("name").as_string()) << ',';
        out << "\"dev_online\":"      << (d.find("online").as_bool() ? "true" : "false") << ',';
        out << "\"dev_model_name\":"  << obn::json::escape(d.find("dev_model_name").as_string()) << ',';
        out << "\"task_status\":"     << obn::json::escape(d.find("print_status").as_string()) << ',';
        out << "\"dev_access_code\":" << obn::json::escape(d.find("dev_access_code").as_string());
        // Pass-through extras; Studio code paths occasionally look them up.
        if (auto v = d.find("dev_product_name"); !v.is_null())
            out << ",\"dev_product_name\":" << obn::json::escape(v.as_string());
        if (auto v = d.find("print_job"); !v.is_null())
            out << ",\"print_job\":" << dump_or_null(v);
        if (auto v = d.find("nozzle_diameter"); !v.is_null())
            out << ",\"nozzle_diameter\":" << dump_or_null(v);
        if (auto v = d.find("dev_structure"); !v.is_null())
            out << ",\"dev_structure\":" << obn::json::escape(v.as_string());
        out << '}';
    }
    out << "]}";
    return out.str();
}

} // namespace

OBN_ABI int bambu_network_get_user_print_info(void* agent,
                                              unsigned int* http_code, std::string* http_body)
{
    if (http_code) *http_code = 0;
    if (http_body) http_body->clear();

    auto* a = as_agent(agent);
    if (!a) return BAMBU_NETWORK_ERR_GET_USER_PRINTINFO_FAILED;
    auto s = a->user_session_snapshot();
    if (s.access_token.empty()) {
        OBN_WARN("get_user_print_info: no access token");
        return BAMBU_NETWORK_ERR_GET_USER_PRINTINFO_FAILED;
    }

    const std::string url = obn::cloud::api_host(a->cloud_region())
                          + "/v1/iot-service/api/user/bind";
    std::map<std::string, std::string> hdrs{
        {"Authorization", "Bearer " + s.access_token},
    };

    auto resp = obn::http::get_json(url, hdrs);
    if (http_code) *http_code = static_cast<unsigned int>(resp.status_code);

    if (!resp.error.empty()) {
        OBN_WARN("get_user_print_info: transport: %s", resp.error.c_str());
        if (http_body) *http_body = resp.body;
        return BAMBU_NETWORK_ERR_GET_USER_PRINTINFO_FAILED;
    }
    if (resp.status_code != 200) {
        OBN_WARN("get_user_print_info: HTTP %ld body=%s",
                 resp.status_code, resp.body.c_str());
        if (http_body) *http_body = resp.body;
        return BAMBU_NETWORK_ERR_GET_USER_PRINTINFO_FAILED;
    }

    std::vector<std::string> dev_ids;
    std::string mapped = remap_bind_payload(resp.body, &dev_ids);
    OBN_INFO("get_user_print_info: mapped %zu -> %zu bytes, %zu device(s)",
             resp.body.size(), mapped.size(), dev_ids.size());

    // Studio's DeviceManager never calls add_subscribe() for single-
    // machine cloud mode (the only call sites in GUI_App.cpp / DevManager
    // are either commented out or gated on the multi-machine flag), yet
    // the stock Bambu plugin still receives per-device pushes from the
    // cloud. We replicate that behaviour here: every device the /user/
    // bind endpoint returns becomes an implicit subscription to
    // device/<id>/report. Cloud MQTT connect may not have landed yet -
    // CloudSession buffers the desired set and re-applies it on the
    // next CONNACK, so ordering doesn't matter.
    if (!dev_ids.empty()) {
        a->cloud_add_subscribe(dev_ids);
    }

    if (http_body) *http_body = std::move(mapped);
    return BAMBU_NETWORK_SUCCESS;
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
