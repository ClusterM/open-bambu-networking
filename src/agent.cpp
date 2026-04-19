#include "obn/agent.hpp"

#include <utility>

namespace obn {

Agent::Agent(std::string log_dir) : log_dir_(std::move(log_dir)) {}
Agent::~Agent() = default;

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
