#pragma once

#include <functional>
#include <string>
#include <vector>

#include "obn/bambu_networking.hpp"

namespace obn {

class Agent;

namespace cloud_bind {

// Cloud-side helpers for abi_bind.cpp (HTTP to api.bambulab.com / .cn).
// LAN printer steps (MQTT ticket exchange) are not replicated yet — bind()
// posts the same bind_code the user already uses for LAN (access code).

int ping_bind(Agent* agent, const std::string& ping_code);

int bind_lan_to_account(Agent* agent,
                        const std::string& dev_ip,
                        const std::string& dev_id,
                        const std::string& sec_link,
                        const std::string& timezone,
                        bool               improved,
                        BBL::OnUpdateStatusFn update_fn);

int query_bind_status(Agent* agent,
                        const std::vector<std::string>& query_list,
                        unsigned int* http_code,
                        std::string* http_body);

int modify_printer_name(Agent* agent,
                          const std::string& dev_id,
                          const std::string& dev_name);

int unbind_device(Agent* agent, const std::string& dev_id);

int request_web_sso_ticket(Agent* agent, std::string* ticket);

} // namespace cloud_bind
} // namespace obn
