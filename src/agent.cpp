#include "obn/agent.hpp"

#include <utility>

#include "obn/bambu_networking.hpp"
#include "obn/log.hpp"

namespace obn {

Agent::Agent(std::string log_dir) : log_dir_(std::move(log_dir)) {}
Agent::~Agent() = default;

int Agent::connect_printer(std::string dev_id,
                           std::string dev_ip,
                           std::string username,
                           std::string password,
                           bool        use_ssl)
{
    // Studio calls connect_printer() again when the user switches to a
    // different printer or re-enters the access code. Tear down any prior
    // session cleanly so we don't leak MQTT threads.
    {
        std::unique_ptr<LanSession> prev;
        {
            std::lock_guard<std::mutex> lk(mu_);
            prev = std::move(lan_session_);
        }
        // prev.reset() happens outside the lock; destructor joins the MQTT
        // loop thread which may call back into notify_local_connected under
        // mu_.
    }

    auto session = std::make_unique<LanSession>(std::move(dev_id),
                                                std::move(dev_ip),
                                                std::move(username),
                                                std::move(password),
                                                use_ssl);

    std::string sess_dev_id = session->dev_id();

    int rc = session->start(
        [this, sess_dev_id](int status, std::string msg) {
            notify_local_connected(status, sess_dev_id, msg);
        },
        [this](std::string d, std::string json) {
            notify_local_message(d, json);
        });

    if (rc == BAMBU_NETWORK_SUCCESS) {
        std::lock_guard<std::mutex> lk(mu_);
        lan_session_ = std::move(session);
    }
    return rc;
}

int Agent::disconnect_printer()
{
    std::unique_ptr<LanSession> session;
    {
        std::lock_guard<std::mutex> lk(mu_);
        session = std::move(lan_session_);
    }
    if (session) session->disconnect();
    return BAMBU_NETWORK_SUCCESS;
}

int Agent::send_message_to_printer(const std::string& dev_id,
                                   const std::string& json_str,
                                   int                qos)
{
    LanSession* session = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (lan_session_ && lan_session_->dev_id() == dev_id)
            session = lan_session_.get();
    }
    if (!session) return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    return session->publish_json(json_str, qos);
}

void Agent::notify_local_connected(int status, const std::string& dev_id, const std::string& msg)
{
    BBL::OnLocalConnectedFn cb;
    BBL::QueueOnMainFn      queue;
    {
        std::lock_guard<std::mutex> lk(mu_);
        cb    = on_local_connect_;
        queue = queue_on_main_;
    }
    OBN_DEBUG("notify_local_connected status=%d dev=%s msg=%s cb=%d queued=%d",
              status, dev_id.c_str(), msg.c_str(), cb ? 1 : 0, queue ? 1 : 0);
    if (!cb) return;
    auto invoke = [cb, status, dev_id, msg]() { cb(status, dev_id, msg); };
    if (queue) queue(invoke);
    else       invoke();
}

void Agent::notify_local_message(const std::string& dev_id, const std::string& json)
{
    BBL::OnMessageFn cb;
    {
        // Per Studio's NetworkAgent wiring, local MQTT report messages go to
        // on_local_message_. We intentionally do not marshal through
        // queue_on_main_ here: DeviceManager.cpp does its own thread hop
        // based on the JSON content (some update paths are fast-path).
        std::lock_guard<std::mutex> lk(mu_);
        cb = on_local_message_;
    }
    OBN_DEBUG("notify_local_message dev=%s bytes=%zu cb=%d",
              dev_id.c_str(), json.size(), cb ? 1 : 0);
    if (cb) cb(dev_id, json);
}

void Agent::set_config_dir(std::string dir)
{
    std::lock_guard<std::mutex> lk(mu_);
    config_dir_ = std::move(dir);
}

void Agent::set_cert_file(std::string folder, std::string filename)
{
    std::lock_guard<std::mutex> lk(mu_);
    cert_folder_   = std::move(folder);
    cert_filename_ = std::move(filename);
}

void Agent::set_country_code(std::string code)
{
    std::lock_guard<std::mutex> lk(mu_);
    country_code_ = std::move(code);
}

void Agent::set_extra_http_headers(std::map<std::string, std::string> headers)
{
    std::lock_guard<std::mutex> lk(mu_);
    extra_http_headers_ = std::move(headers);
}

void Agent::set_user_selected_machine(std::string dev_id)
{
    std::lock_guard<std::mutex> lk(mu_);
    user_selected_machine_ = std::move(dev_id);
}

std::string Agent::country_code() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return country_code_;
}

std::string Agent::config_dir() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return config_dir_;
}

std::string Agent::user_selected_machine() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return user_selected_machine_;
}

#define OBN_SETTER(method, field, type)                 \
    void Agent::method(type fn)                         \
    {                                                   \
        std::lock_guard<std::mutex> lk(mu_);            \
        field = std::move(fn);                          \
    }

OBN_SETTER(set_on_ssdp_msg_fn,          on_ssdp_msg_,          BBL::OnMsgArrivedFn)
OBN_SETTER(set_on_user_login_fn,        on_user_login_,        BBL::OnUserLoginFn)
OBN_SETTER(set_on_printer_connected_fn, on_printer_connected_, BBL::OnPrinterConnectedFn)
OBN_SETTER(set_on_server_connected_fn,  on_server_connected_,  BBL::OnServerConnectedFn)
OBN_SETTER(set_on_http_error_fn,        on_http_error_,        BBL::OnHttpErrorFn)
OBN_SETTER(set_get_country_code_fn,     get_country_code_,     BBL::GetCountryCodeFn)
OBN_SETTER(set_on_subscribe_failure_fn, on_subscribe_failure_, BBL::GetSubscribeFailureFn)
OBN_SETTER(set_on_message_fn,           on_message_,           BBL::OnMessageFn)
OBN_SETTER(set_on_user_message_fn,      on_user_message_,      BBL::OnMessageFn)
OBN_SETTER(set_on_local_connect_fn,     on_local_connect_,     BBL::OnLocalConnectedFn)
OBN_SETTER(set_on_local_message_fn,     on_local_message_,     BBL::OnMessageFn)
OBN_SETTER(set_queue_on_main_fn,        queue_on_main_,        BBL::QueueOnMainFn)
OBN_SETTER(set_server_callback,         server_err_,           BBL::OnServerErrFn)

#undef OBN_SETTER

} // namespace obn
