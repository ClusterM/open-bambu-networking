#include <string>

#include "obn/abi_export.hpp"
#include "obn/agent.hpp"
#include "obn/bambu_networking.hpp"
#include "obn/log.hpp"

using obn::as_agent;

namespace {
void log_print_params(const char* which, const BBL::PrintParams& p)
{
    OBN_INFO("%s dev=%s ip=%s ssl_mqtt=%d ssl_ftp=%d task=%s plate=%d ams=%s 3mf=%s md5=%s",
             which, p.dev_id.c_str(), p.dev_ip.c_str(), p.use_ssl_for_mqtt, p.use_ssl_for_ftp,
             p.task_name.c_str(), p.plate_index,
             p.ams_mapping.c_str(),
             p.ftp_file.c_str(), p.ftp_file_md5.c_str());
}
} // namespace

OBN_ABI int bambu_network_start_print(void* /*agent*/,
                                      BBL::PrintParams      params,
                                      BBL::OnUpdateStatusFn update_fn,
                                      BBL::WasCancelledFn   /*cancel_fn*/,
                                      BBL::OnWaitFn         /*wait_fn*/)
{
    log_print_params("start_print", params);
    if (update_fn) update_fn(BBL::PrintingStageERROR,
                             BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED,
                             "obn: start_print not implemented");
    return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
}

OBN_ABI int bambu_network_start_local_print_with_record(void* /*agent*/,
                                                        BBL::PrintParams      params,
                                                        BBL::OnUpdateStatusFn update_fn,
                                                        BBL::WasCancelledFn   /*cancel_fn*/,
                                                        BBL::OnWaitFn         /*wait_fn*/)
{
    log_print_params("start_local_print_with_record", params);
    if (update_fn) update_fn(BBL::PrintingStageERROR,
                             BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED,
                             "obn: start_local_print_with_record not implemented");
    return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
}

OBN_ABI int bambu_network_start_send_gcode_to_sdcard(void* /*agent*/,
                                                     BBL::PrintParams      params,
                                                     BBL::OnUpdateStatusFn update_fn,
                                                     BBL::WasCancelledFn   /*cancel_fn*/,
                                                     BBL::OnWaitFn         /*wait_fn*/)
{
    log_print_params("start_send_gcode_to_sdcard", params);
    if (update_fn) update_fn(BBL::PrintingStageERROR,
                             BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED,
                             "obn: start_send_gcode_to_sdcard not implemented");
    return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
}

OBN_ABI int bambu_network_start_local_print(void* /*agent*/,
                                            BBL::PrintParams      params,
                                            BBL::OnUpdateStatusFn update_fn,
                                            BBL::WasCancelledFn   /*cancel_fn*/)
{
    log_print_params("start_local_print", params);
    if (update_fn) update_fn(BBL::PrintingStageERROR,
                             BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED,
                             "obn: start_local_print not implemented");
    return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
}

OBN_ABI int bambu_network_start_sdcard_print(void* /*agent*/,
                                             BBL::PrintParams      params,
                                             BBL::OnUpdateStatusFn update_fn,
                                             BBL::WasCancelledFn   /*cancel_fn*/)
{
    log_print_params("start_sdcard_print", params);
    if (update_fn) update_fn(BBL::PrintingStageERROR,
                             BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED,
                             "obn: start_sdcard_print not implemented");
    return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
}
