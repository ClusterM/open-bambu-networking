#include <string>

#include "obn/abi_export.hpp"
#include "obn/agent.hpp"
#include "obn/bambu_networking.hpp"

using obn::as_agent;

OBN_ABI int bambu_network_connect_printer(void* /*agent*/,
                                          std::string /*dev_id*/,
                                          std::string /*dev_ip*/,
                                          std::string /*username*/,
                                          std::string /*password*/,
                                          bool        /*use_ssl*/)
{
    return BAMBU_NETWORK_ERR_CONNECT_FAILED;
}

OBN_ABI int bambu_network_disconnect_printer(void* /*agent*/)
{
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_send_message_to_printer(void* /*agent*/,
                                                  std::string /*dev_id*/,
                                                  std::string /*json_str*/,
                                                  int         /*qos*/,
                                                  int         /*flag*/)
{
    return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
}

// Studio resolves this as bambu_network_update_cert (see NetworkAgent.cpp:312).
OBN_ABI int bambu_network_update_cert(void* /*agent*/)
{
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI void bambu_network_install_device_cert(void* /*agent*/,
                                               std::string /*dev_id*/,
                                               bool        /*lan_only*/)
{
}

OBN_ABI bool bambu_network_start_discovery(void* /*agent*/, bool /*start*/, bool /*sending*/)
{
    return false;
}
