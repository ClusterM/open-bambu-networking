#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "obn/auth.hpp"
#include "obn/bambu_networking.hpp"

namespace obn {
namespace mqtt { class Client; }
namespace ssdp { class Discovery; }
class CloudSession;

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

    // Starts/stops the LAN SSDP listener that feeds on_ssdp_msg_fn. Bambu
    // printers advertise themselves once every ~5 s via a UDP broadcast on
    // port 2021. Returns true if the listener is running after the call.
    bool start_discovery(bool enable, bool sending);

    // Implements bambu_network_start_local_print: upload the .3mf over
    // FTPS to the printer's storage, then publish a `project_file`
    // command over the active LAN MQTT session. Runs synchronously on the
    // caller's thread - Studio invokes this from its PrintJob worker.
    // Returns BAMBU_NETWORK_* code; on failure stage == PrintingStageERROR.
    int run_local_print_job(const BBL::PrintParams&   params,
                            BBL::OnUpdateStatusFn     update_fn,
                            BBL::WasCancelledFn       cancel_fn);

    // Implements bambu_network_start_send_gcode_to_sdcard: upload only,
    // no MQTT command. Used by Studio's access-code probe and by
    // "Send to printer without starting a print".
    int run_send_gcode_to_sdcard(const BBL::PrintParams& params,
                                 BBL::OnUpdateStatusFn   update_fn,
                                 BBL::WasCancelledFn     cancel_fn);

    // Implements bambu_network_start_sdcard_print: the "Print" button
    // from Device -> Files. The file is already on the printer's
    // storage (we list/browse it via the PrinterFileSystem bridge), so
    // there's no FTPS upload - we just publish a `project_file` MQTT
    // command with url=ftp://<path> on the LAN channel. Studio
    // hard-codes this path to `start_sdcard_print` and calls it
    // "cloud service" in the UI, but on Developer Mode printers there
    // is no cloud route; going over LAN MQTT is the only thing the
    // printer will actually accept.
    int run_sdcard_print_job(const BBL::PrintParams& params,
                             BBL::OnUpdateStatusFn   update_fn,
                             BBL::WasCancelledFn     cancel_fn);

    // Implements bambu_network_start_print (use_lan_channel=false) and
    // bambu_network_start_local_print_with_record (use_lan_channel=true).
    //
    // Orchestrates Bambu's cloud-print sequence reverse-engineered from
    // MITM of the original plugin:
    //   1.  POST /iot-service/api/user/project          - create project
    //   2.  PUT  <presigned S3 url>                     - upload config 3mf
    //   3.  PUT  /iot-service/api/user/notification     - notify upload
    //   4.  GET  /iot-service/api/user/notification?... - poll
    //   5.  PATCH /iot-service/api/user/project/<pid>   - register (ftp:// url)
    //   6.  GET  /iot-service/api/user/upload?models=.. - request second url
    //   7.  PUT  <presigned S3 url>                     - upload full 3mf
    //   8.  PATCH /iot-service/api/user/project/<pid>   - register (https:// url)
    //   9.  (LAN only) FTPS STOR to /cache/<name>.gcode.3mf
    //  10.  POST /user-service/my/task                  - create task
    //  11.  MQTT publish project_file:
    //         - LAN channel: via LanSession, url=ftp://<name>
    //         - cloud channel: via CloudSession, url=<S3 presigned>
    int run_cloud_print_job(const BBL::PrintParams& params,
                            BBL::OnUpdateStatusFn   update_fn,
                            BBL::WasCancelledFn     cancel_fn,
                            bool                    use_lan_channel);

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

    // -----------------------------
    // Cloud MQTT (Studio's "server" connection).
    // -----------------------------
    // Opens the long-lived TLS MQTT connection to us.mqtt.bambulab.com
    // using the currently-stored access token. Idempotent: safe to
    // call repeatedly, subsequent calls are no-ops while already
    // connected. Returns BAMBU_NETWORK_* code.
    int  connect_cloud();
    int  disconnect_cloud();
    bool cloud_connected() const;
    int  cloud_refresh();
    int  cloud_add_subscribe(const std::vector<std::string>& dev_ids);
    int  cloud_del_subscribe(const std::vector<std::string>& dev_ids);
    int  cloud_send_message(const std::string& dev_id,
                            const std::string& json_str,
                            int qos);

    // -----------------------------
    // Cloud user session.
    // -----------------------------
    // Accept a login_info JSON (the same body the Bambu cloud returns
    // from /user/login). Extracts tokens + profile fields, stores them
    // under <config_dir>/obn.auth.json. Returns 0 on success.
    int apply_login_info(const std::string& login_info_json);

    // Forget the current session and delete the persisted file.
    void clear_session();

    // Consult the disk-backed store and, if the refresh token is fresh
    // enough, perform a silent refresh so the next HTTP call has a
    // valid Bearer. Called on Agent construction.
    void hydrate_session();

    bool        user_logged_in() const;
    obn::auth::Session user_session_snapshot() const
    {
        return auth_store_ ? auth_store_->snapshot() : obn::auth::Session{};
    }

    // Human-readable region identifier used by cloud endpoints.
    std::string cloud_region() const;

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
    std::unique_ptr<ssdp::Discovery> discovery_;
    std::unique_ptr<CloudSession>   cloud_session_;

    // First cloud report per dev_id flips this set, which is what
    // triggers the one-shot on_printer_connected("tunnel/<id>")
    // notification. Cleared on disconnect/resubscribe so reconnects
    // re-fire the notification.
    std::set<std::string> cloud_connected_devs_;

    // Holds the cloud session (tokens + profile). Lazily populated from
    // <config_dir>/obn.auth.json as soon as config_dir_ is set.
    std::unique_ptr<obn::auth::Store> auth_store_;

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
