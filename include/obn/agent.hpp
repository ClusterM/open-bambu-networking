#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "obn/bambu_networking.hpp"

namespace obn {
namespace mqtt { class Client; }
namespace ssdp { class Discovery; }

// Per-printer LAN MQTT session. Studio only holds one such connection at a
// time (multi-printer LAN view is a future extension), so Agent owns a single
// LanSession and tears it down before opening a new one.
class LanSession {
public:
    LanSession(std::string dev_id,
               std::string dev_ip,
               std::string username,
               std::string password,
               bool        use_ssl,
               std::string ca_file);
    ~LanSession();

    LanSession(const LanSession&)            = delete;
    LanSession& operator=(const LanSession&) = delete;

    // Dispatches its two callbacks on the MQTT network thread; the receiver
    // is responsible for queueing UI updates through Agent::queue_on_main.
    using ConnectedCb = std::function<void(int status /*ConnectStatus*/, std::string msg)>;
    using MessageCb   = std::function<void(std::string dev_id, std::string json)>;

    // Starts the MQTT connection asynchronously and returns once loop_start
    // succeeds. Returns a BAMBU_NETWORK_ERR_* code.
    int start(ConnectedCb on_connected, MessageCb on_message);

    int publish_json(const std::string& json_str, int qos);
    int disconnect();

    const std::string& dev_id() const { return dev_id_; }
    const std::string& dev_ip() const { return dev_ip_; }

private:
    std::string report_topic_() const;
    std::string request_topic_() const;

    std::string dev_id_;
    std::string dev_ip_;
    std::string username_;
    std::string password_;
    bool        use_ssl_;
    std::string ca_file_;

    std::unique_ptr<mqtt::Client> client_;
    ConnectedCb                   on_connected_;
    MessageCb                     on_message_;
};

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
    // LAN printer session (one at a time).
    // -----------------------------
    int  connect_printer(std::string dev_id,
                         std::string dev_ip,
                         std::string username,
                         std::string password,
                         bool        use_ssl);
    int  disconnect_printer();
    int  send_message_to_printer(const std::string& dev_id,
                                 const std::string& json_str,
                                 int                qos);

    // Studio calls this every ~1 s from its refresh timer, plus once right
    // after on_printer_connected_fn. We only do real work the first time a
    // given `dev_id` is seen (and only in lan_only mode for now): capture the
    // printer's self-signed server certificate into <config_dir>/certs/.
    void install_device_cert(const std::string& dev_id, bool lan_only);

    // -----------------------------
    // Accessors used by stub returns.
    // -----------------------------
    std::string country_code() const;
    std::string log_dir() const { return log_dir_; }
    std::string config_dir() const;
    std::string cert_folder() const;
    std::string cert_filename() const;
    // Returns "<cert_folder>/printer.cer" if the file exists, otherwise "".
    // Used as the CA trust store for LAN MQTT so we can validate the chain
    // the same way Bambu's own plugin does.
    std::string bambu_ca_bundle_path() const;
    std::string user_selected_machine() const;

    // Invoked by LanSession from the MQTT network thread. Marshals the call
    // through queue_on_main_ when Studio registered one, so status callbacks
    // reach the Studio UI thread safely.
    void notify_local_connected(int status, const std::string& dev_id, const std::string& msg);
    void notify_local_message(const std::string& dev_id, const std::string& json);

private:
    mutable std::mutex mu_;
    std::string        log_dir_;
    std::string        config_dir_;
    std::string        cert_folder_;
    std::string        cert_filename_;
    std::string        country_code_{"US"};
    std::string        user_selected_machine_;
    std::map<std::string, std::string> extra_http_headers_;

    std::unique_ptr<LanSession> lan_session_;

    // Tracks which printers we've already snapshotted a server cert for in
    // the current process. Keyed by dev_id. Studio's refresh timer calls
    // install_device_cert() ~1 Hz, and we don't want to pound the printer
    // with a fresh TLS handshake every tick.
    std::set<std::string> certified_devs_;

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
