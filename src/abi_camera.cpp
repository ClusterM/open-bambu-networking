#include <functional>
#include <string>

#include "obn/abi_export.hpp"
#include "obn/agent.hpp"
#include "obn/bambu_networking.hpp"

using obn::as_agent;

// Cloud-signed TUTK/Agora liveview is intentionally not implemented -
// this plugin is a LAN-first replacement. On LAN-only printers
// MediaPlayCtrl takes its native LAN branch and never even calls us
// here (see src/slic3r/GUI/MediaPlayCtrl.cpp:~308); on cloud-paired
// printers it calls get_camera_url() expecting a `bambu:///tutk?...`
// URL which we can't mint without shipping the proprietary TUTK SDK.
// Returning an empty URL drives Studio into its normal "connection
// failed" path.
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
