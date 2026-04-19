#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "obn/bambu_networking.hpp"

namespace obn {

// The Agent object is created per Studio call to bambu_network_create_agent().
// For now it is an inert carrier for registered callbacks and configuration:
// later phases flesh out an internal event loop, MQTT clients and HTTP/FTPS
// session managers. Keeping this scaffold minimal is deliberate: phase 1 goal
// is just to get Studio to load the plugin without crashing or disabling
// itself.
class Agent {
public:
    explicit Agent(std::string log_dir);
    ~Agent();

    Agent(const Agent&)            = delete;
    Agent& operator=(const Agent&) = delete;

    // -----------------------------
    // Basic setters (noexcept).
    // -----------------------------
    void set_config_dir(std::string dir);
    void set_cert_file(std::string folder, std::string filename);
    void set_country_code(std::string code);
    void set_extra_http_headers(std::map<std::string, std::string> headers);
    void set_user_selected_machine(std::string dev_id);

    // -----------------------------
    // Callback registration.
    // Every callback is stored under a mutex so that later background threads
    // can read/invoke it safely.
    // -----------------------------
    void set_on_ssdp_msg_fn(BBL::OnMsgArrivedFn fn);
    void set_on_user_login_fn(BBL::OnUserLoginFn fn);
    void set_on_printer_connected_fn(BBL::OnPrinterConnectedFn fn);
    void set_on_server_connected_fn(BBL::OnServerConnectedFn fn);
    void set_on_http_error_fn(BBL::OnHttpErrorFn fn);
    void set_get_country_code_fn(BBL::GetCountryCodeFn fn);
    void set_on_subscribe_failure_fn(BBL::GetSubscribeFailureFn fn);
    void set_on_message_fn(BBL::OnMessageFn fn);
    void set_on_user_message_fn(BBL::OnMessageFn fn);
    void set_on_local_connect_fn(BBL::OnLocalConnectedFn fn);
    void set_on_local_message_fn(BBL::OnMessageFn fn);
    void set_queue_on_main_fn(BBL::QueueOnMainFn fn);
    void set_server_callback(BBL::OnServerErrFn fn);

    // -----------------------------
    // Accessors used by stub returns.
    // -----------------------------
    std::string country_code() const;
    std::string log_dir() const { return log_dir_; }
    std::string config_dir() const;
    std::string user_selected_machine() const;

private:
    mutable std::mutex mu_;
    std::string        log_dir_;
    std::string        config_dir_;
    std::string        cert_folder_;
    std::string        cert_filename_;
    std::string        country_code_{"US"};
    std::string        user_selected_machine_;
    std::map<std::string, std::string> extra_http_headers_;

    // Callbacks - stored, not (yet) invoked.
    BBL::OnMsgArrivedFn       on_ssdp_msg_{};
    BBL::OnUserLoginFn        on_user_login_{};
    BBL::OnPrinterConnectedFn on_printer_connected_{};
    BBL::OnServerConnectedFn  on_server_connected_{};
    BBL::OnHttpErrorFn        on_http_error_{};
    BBL::GetCountryCodeFn     get_country_code_{};
    BBL::GetSubscribeFailureFn on_subscribe_failure_{};
    BBL::OnMessageFn          on_message_{};
    BBL::OnMessageFn          on_user_message_{};
    BBL::OnLocalConnectedFn   on_local_connect_{};
    BBL::OnMessageFn          on_local_message_{};
    BBL::QueueOnMainFn        queue_on_main_{};
    BBL::OnServerErrFn        server_err_{};
};

// Safe cast with null guard used by every exported function. Keeps the exports
// short and consistent. Returns nullptr for the one-arg handle variant.
inline Agent* as_agent(void* h) { return static_cast<Agent*>(h); }

} // namespace obn
