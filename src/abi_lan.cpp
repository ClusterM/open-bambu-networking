#include <string>

#include "obn/abi_export.hpp"
#include "obn/agent.hpp"
#include "obn/bambu_networking.hpp"
#include "obn/log.hpp"

using obn::as_agent;

OBN_ABI int bambu_network_connect_printer(void* agent,
                                          std::string dev_id,
                                          std::string dev_ip,
                                          std::string username,
                                          std::string password,
                                          bool        use_ssl)
{
    OBN_INFO("connect_printer dev=%s ip=%s user=%s pwd.len=%zu ssl=%d",
             dev_id.c_str(), dev_ip.c_str(), username.c_str(),
             password.size(), use_ssl);
    auto* a = as_agent(agent);
    if (!a) {
        OBN_ERROR("connect_printer: null agent handle");
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    int rc = a->connect_printer(std::move(dev_id),
                                std::move(dev_ip),
                                std::move(username),
                                std::move(password),
                                use_ssl);
    OBN_INFO("connect_printer -> rc=%d", rc);
    return rc;
}

OBN_ABI int bambu_network_disconnect_printer(void* agent)
{
    OBN_INFO("disconnect_printer");
    auto* a = as_agent(agent);
    if (!a) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return a->disconnect_printer();
}

OBN_ABI int bambu_network_send_message_to_printer(void* agent,
                                                  std::string dev_id,
                                                  std::string json_str,
                                                  int         qos,
                                                  int         flag)
{
    OBN_DEBUG("send_message_to_printer dev=%s qos=%d flag=%d payload=%s",
              dev_id.c_str(), qos, flag,
              obn::log::redact(json_str, 200).c_str());
    auto* a = as_agent(agent);
    if (!a) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    int rc = a->send_message_to_printer(dev_id, json_str, qos);
    if (rc != BAMBU_NETWORK_SUCCESS)
        OBN_WARN("send_message_to_printer dev=%s -> rc=%d", dev_id.c_str(), rc);
    return rc;
}

OBN_ABI int bambu_network_update_cert(void* /*agent*/)
{
    OBN_DEBUG("update_cert");
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI void bambu_network_install_device_cert(void* agent,
                                               std::string dev_id,
                                               bool        lan_only)
{
    // Demoted to DEBUG because Studio calls this ~1 Hz from its refresh
    // timer; the agent internally dedups so real work only happens once per
    // session and is logged there at INFO level.
    OBN_DEBUG("install_device_cert dev=%s lan_only=%d", dev_id.c_str(), lan_only);
    auto* a = as_agent(agent);
    if (!a) return;
    a->install_device_cert(dev_id, lan_only);
}

OBN_ABI bool bambu_network_start_discovery(void* agent, bool start, bool sending)
{
    OBN_DEBUG("bambu_network_start_discovery start=%d sending=%d", start, sending);
    auto* a = as_agent(agent);
    if (!a) return false;
    return a->start_discovery(start, sending);
}
