#include "obn/abi_export.hpp"
#include "obn/agent.hpp"
#include "obn/bambu_networking.hpp"
#include "obn/log.hpp"

using obn::Agent;
using obn::as_agent;

#define OBN_CB_EXPORT(sym, method, cb_t)                            \
    OBN_ABI int sym(void* agent, BBL::cb_t fn)                      \
    {                                                               \
        OBN_DEBUG(#sym " set=%d", fn ? 1 : 0);                      \
        if (auto* a = as_agent(agent)) {                            \
            a->method(std::move(fn));                               \
            return BAMBU_NETWORK_SUCCESS;                           \
        }                                                           \
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;                    \
    }

OBN_CB_EXPORT(bambu_network_set_on_ssdp_msg_fn,          set_on_ssdp_msg_fn,          OnMsgArrivedFn)
OBN_CB_EXPORT(bambu_network_set_on_user_login_fn,        set_on_user_login_fn,        OnUserLoginFn)
OBN_CB_EXPORT(bambu_network_set_on_printer_connected_fn, set_on_printer_connected_fn, OnPrinterConnectedFn)
OBN_CB_EXPORT(bambu_network_set_on_server_connected_fn,  set_on_server_connected_fn,  OnServerConnectedFn)
OBN_CB_EXPORT(bambu_network_set_on_http_error_fn,        set_on_http_error_fn,        OnHttpErrorFn)
OBN_CB_EXPORT(bambu_network_set_get_country_code_fn,     set_get_country_code_fn,     GetCountryCodeFn)
OBN_CB_EXPORT(bambu_network_set_on_subscribe_failure_fn, set_on_subscribe_failure_fn, GetSubscribeFailureFn)
OBN_CB_EXPORT(bambu_network_set_on_message_fn,           set_on_message_fn,           OnMessageFn)
OBN_CB_EXPORT(bambu_network_set_on_user_message_fn,      set_on_user_message_fn,      OnMessageFn)
OBN_CB_EXPORT(bambu_network_set_on_local_connect_fn,     set_on_local_connect_fn,     OnLocalConnectedFn)
OBN_CB_EXPORT(bambu_network_set_on_local_message_fn,     set_on_local_message_fn,     OnMessageFn)
OBN_CB_EXPORT(bambu_network_set_queue_on_main_fn,        set_queue_on_main_fn,        QueueOnMainFn)
OBN_CB_EXPORT(bambu_network_set_server_callback,         set_server_callback,         OnServerErrFn)

#undef OBN_CB_EXPORT
