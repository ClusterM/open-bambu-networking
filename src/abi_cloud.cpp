#include <string>
#include <vector>

#include "obn/abi_export.hpp"
#include "obn/agent.hpp"
#include "obn/bambu_networking.hpp"

using obn::as_agent;

// Server connectivity -------------------------------------------------------

OBN_ABI int bambu_network_connect_server(void* /*agent*/)
{
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI bool bambu_network_is_server_connected(void* /*agent*/)
{
    // Phase 1: report disconnected so Studio falls back to LAN flows whenever
    // it can, instead of assuming the cloud is ready.
    return false;
}

OBN_ABI int bambu_network_refresh_connection(void* /*agent*/)
{
    return BAMBU_NETWORK_SUCCESS;
}

// Topic subscriptions -------------------------------------------------------

OBN_ABI int bambu_network_start_subscribe(void* /*agent*/, std::string /*module*/)
{
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_stop_subscribe(void* /*agent*/, std::string /*module*/)
{
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_add_subscribe(void* /*agent*/, std::vector<std::string> /*dev_list*/)
{
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_del_subscribe(void* /*agent*/, std::vector<std::string> /*dev_list*/)
{
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI void bambu_network_enable_multi_machine(void* /*agent*/, bool /*enable*/)
{
}

OBN_ABI int bambu_network_send_message(void* /*agent*/,
                                       std::string /*dev_id*/,
                                       std::string /*json_str*/,
                                       int         /*qos*/,
                                       int         /*flag*/)
{
    return BAMBU_NETWORK_SUCCESS;
}
