#include <functional>
#include <string>

#include "obn/abi_export.hpp"
#include "obn/agent.hpp"
#include "obn/bambu_networking.hpp"

using obn::as_agent;

OBN_ABI int bambu_network_get_camera_url(void* /*agent*/,
                                         std::string /*dev_id*/,
                                         std::function<void(std::string)> callback)
{
    if (callback) callback(std::string{});
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_get_camera_url_for_golive(void* /*agent*/,
                                                    std::string /*dev_id*/,
                                                    std::string /*sdev_id*/,
                                                    std::function<void(std::string)> callback)
{
    if (callback) callback(std::string{});
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_get_hms_snapshot(void* /*agent*/,
                                           std::string& /*dev_id*/,
                                           std::string& /*file_name*/,
                                           std::function<void(std::string, int)> callback)
{
    if (callback) callback(std::string{}, -1);
    return BAMBU_NETWORK_SUCCESS;
}
