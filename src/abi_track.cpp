#include <string>

#include "obn/abi_export.hpp"
#include "obn/bambu_networking.hpp"

// Telemetry / analytics: disabled in this plugin. Everything is a no-op that
// claims success so Studio's "report consent" dialogs stay quiet.

OBN_ABI int bambu_network_track_enable(void* /*agent*/, bool /*enable*/)
{
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_track_remove_files(void* /*agent*/)
{
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_track_event(void* /*agent*/,
                                      std::string /*evt_key*/,
                                      std::string /*content*/)
{
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_track_header(void* /*agent*/, std::string /*header*/)
{
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_track_update_property(void* /*agent*/,
                                                std::string /*name*/,
                                                std::string /*value*/,
                                                std::string /*type*/)
{
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_track_get_property(void* /*agent*/,
                                             std::string /*name*/,
                                             std::string& value,
                                             std::string  /*type*/)
{
    value.clear();
    return BAMBU_NETWORK_SUCCESS;
}
