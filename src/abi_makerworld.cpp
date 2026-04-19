#include <functional>
#include <string>
#include <vector>

#include "obn/abi_export.hpp"
#include "obn/agent.hpp"
#include "obn/bambu_networking.hpp"

using obn::as_agent;

// MakerWorld / model mall / OSS: no open specification exists. These all
// return success with empty payloads so Studio's UI degrades gracefully
// instead of crashing.

OBN_ABI int bambu_network_get_design_staffpick(void* /*agent*/,
                                               int /*offset*/, int /*limit*/,
                                               std::function<void(std::string)> cb)
{
    if (cb) cb("{\"list\":[],\"total\":0}");
    return BAMBU_NETWORK_SUCCESS;
}

// The real plugin spells this `start_publish` when resolved (NetworkAgent.cpp
// uses the typo `start_pubilsh` only on the Studio side for the function
// pointer name), so we export the canonical name.
OBN_ABI int bambu_network_start_publish(void* /*agent*/,
                                        BBL::PublishParams     /*params*/,
                                        BBL::OnUpdateStatusFn  /*update_fn*/,
                                        BBL::WasCancelledFn    /*cancel_fn*/,
                                        std::string*           out)
{
    if (out) out->clear();
    return BAMBU_NETWORK_ERR_INVALID_RESULT;
}

OBN_ABI int bambu_network_get_model_publish_url(void* /*agent*/, std::string* url)
{
    if (url) *url = "https://makerworld.com/";
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_get_subtask(void*        /*agent*/,
                                      void*        /*task*/,
                                      std::function<void(int, std::string)> cb)
{
    if (cb) cb(-1, std::string{});
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_get_model_mall_home_url(void* /*agent*/, std::string* url)
{
    if (url) *url = "https://makerworld.com/";
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_get_model_mall_detail_url(void* /*agent*/,
                                                    std::string* url,
                                                    std::string  id)
{
    if (url) *url = std::string("https://makerworld.com/models/") + id;
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_put_model_mall_rating(void* /*agent*/,
                                                int /*rating_id*/, int /*score*/,
                                                std::string /*content*/,
                                                std::vector<std::string> /*images*/,
                                                unsigned int& http_code,
                                                std::string&  http_error)
{
    http_code = 0;
    http_error.clear();
    return BAMBU_NETWORK_ERR_INVALID_RESULT;
}

OBN_ABI int bambu_network_get_oss_config(void* /*agent*/,
                                         std::string& config,
                                         std::string  /*country_code*/,
                                         unsigned int& http_code,
                                         std::string&  http_error)
{
    config.clear();
    http_code = 0;
    http_error.clear();
    return BAMBU_NETWORK_ERR_INVALID_RESULT;
}

OBN_ABI int bambu_network_put_rating_picture_oss(void* /*agent*/,
                                                 std::string& /*config*/,
                                                 std::string& pic_oss_path,
                                                 std::string  /*model_id*/,
                                                 int          /*profile_id*/,
                                                 unsigned int& http_code,
                                                 std::string&  http_error)
{
    pic_oss_path.clear();
    http_code = 0;
    http_error.clear();
    return BAMBU_NETWORK_ERR_INVALID_RESULT;
}

OBN_ABI int bambu_network_get_model_mall_rating(void* /*agent*/,
                                                int /*job_id*/,
                                                std::string&  rating_result,
                                                unsigned int& http_code,
                                                std::string&  http_error)
{
    rating_result.clear();
    http_code = 0;
    http_error.clear();
    return BAMBU_NETWORK_ERR_INVALID_RESULT;
}

OBN_ABI int bambu_network_get_mw_user_preference(void* /*agent*/,
                                                 std::function<void(std::string)> cb)
{
    if (cb) cb("{}");
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_get_mw_user_4ulist(void* /*agent*/,
                                             int /*seed*/, int /*limit*/,
                                             std::function<void(std::string)> cb)
{
    if (cb) cb("{\"list\":[],\"total\":0}");
    return BAMBU_NETWORK_SUCCESS;
}

// -----------------------------------------------------------------------
// Additional symbols exported by the real plugin. Bambu Studio's current
// NetworkAgent.cpp does not resolve them, but newer Studio builds might; we
// export them as no-ops to stay binary-compatible.
// -----------------------------------------------------------------------

OBN_ABI int bambu_network_check_user_report(void* /*agent*/, int* /*id*/, bool* printable)
{
    if (printable) *printable = false;
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_del_rating_picture_oss(void* /*agent*/,
                                                 std::string& /*config*/,
                                                 std::string& pic_oss_path,
                                                 std::string  /*model_id*/,
                                                 int          /*profile_id*/,
                                                 unsigned int& http_code,
                                                 std::string&  http_error)
{
    pic_oss_path.clear();
    http_code = 0;
    http_error.clear();
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_get_model_instance_id(void* /*agent*/,
                                                std::string /*model_id*/,
                                                std::string* instance_id,
                                                unsigned int& http_code,
                                                std::string&  http_error)
{
    if (instance_id) instance_id->clear();
    http_code = 0;
    http_error.clear();
    return BAMBU_NETWORK_ERR_GET_INSTANCE_ID_FAILED;
}

OBN_ABI int bambu_network_get_model_rating_id(void* /*agent*/,
                                              std::string  /*model_id*/,
                                              int          /*profile_id*/,
                                              int*         rating_id,
                                              unsigned int& http_code,
                                              std::string&  http_error)
{
    if (rating_id) *rating_id = 0;
    http_code = 0;
    http_error.clear();
    return BAMBU_NETWORK_ERR_GET_RATING_ID_FAILED;
}
