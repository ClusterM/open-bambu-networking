#include <string>

#include "obn/abi_export.hpp"
#include "obn/agent.hpp"
#include "obn/bambu_networking.hpp"

using obn::Agent;
using obn::as_agent;

// Studio sometimes accesses these as extern globals; define them here so the
// linker is happy if the headers are included via InitFTModule in-process.
std::string g_log_folder;
std::string g_log_start_time;

OBN_ABI void* bambu_network_create_agent(std::string log_dir)
{
    try {
        return new Agent(std::move(log_dir));
    } catch (...) {
        return nullptr;
    }
}

OBN_ABI int bambu_network_destroy_agent(void* agent)
{
    delete as_agent(agent);
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_init_log(void* /*agent*/)
{
    return BAMBU_NETWORK_SUCCESS;
}

OBN_ABI int bambu_network_set_config_dir(void* agent, std::string config_dir)
{
    if (auto* a = as_agent(agent)) {
        a->set_config_dir(std::move(config_dir));
        return BAMBU_NETWORK_SUCCESS;
    }
    return BAMBU_NETWORK_ERR_INVALID_HANDLE;
}

OBN_ABI int bambu_network_set_cert_file(void* agent, std::string folder, std::string filename)
{
    if (auto* a = as_agent(agent)) {
        a->set_cert_file(std::move(folder), std::move(filename));
        return BAMBU_NETWORK_SUCCESS;
    }
    return BAMBU_NETWORK_ERR_INVALID_HANDLE;
}

OBN_ABI int bambu_network_set_country_code(void* agent, std::string country_code)
{
    if (auto* a = as_agent(agent)) {
        a->set_country_code(std::move(country_code));
        return BAMBU_NETWORK_SUCCESS;
    }
    return BAMBU_NETWORK_ERR_INVALID_HANDLE;
}

OBN_ABI int bambu_network_start(void* /*agent*/)
{
    // Phase 1 stub: success without starting any background work.
    return BAMBU_NETWORK_SUCCESS;
}
