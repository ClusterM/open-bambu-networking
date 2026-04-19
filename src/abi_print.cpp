#include <string>

#include "obn/abi_export.hpp"
#include "obn/agent.hpp"
#include "obn/bambu_networking.hpp"

using obn::as_agent;

// All start_*_print flavours return the generic connection failure error in
// Phase 1: we have no MQTT/FTPS backend yet, so calling them must fail
// gracefully rather than silently report success.

OBN_ABI int bambu_network_start_print(void* /*agent*/,
                                      BBL::PrintParams      /*params*/,
                                      BBL::OnUpdateStatusFn /*update_fn*/,
                                      BBL::WasCancelledFn   /*cancel_fn*/,
                                      BBL::OnWaitFn         /*wait_fn*/)
{
    return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
}

OBN_ABI int bambu_network_start_local_print_with_record(void* /*agent*/,
                                                        BBL::PrintParams      /*params*/,
                                                        BBL::OnUpdateStatusFn /*update_fn*/,
                                                        BBL::WasCancelledFn   /*cancel_fn*/,
                                                        BBL::OnWaitFn         /*wait_fn*/)
{
    return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
}

OBN_ABI int bambu_network_start_send_gcode_to_sdcard(void* /*agent*/,
                                                     BBL::PrintParams      /*params*/,
                                                     BBL::OnUpdateStatusFn /*update_fn*/,
                                                     BBL::WasCancelledFn   /*cancel_fn*/,
                                                     BBL::OnWaitFn         /*wait_fn*/)
{
    return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
}

OBN_ABI int bambu_network_start_local_print(void* /*agent*/,
                                            BBL::PrintParams      /*params*/,
                                            BBL::OnUpdateStatusFn /*update_fn*/,
                                            BBL::WasCancelledFn   /*cancel_fn*/)
{
    return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
}

OBN_ABI int bambu_network_start_sdcard_print(void* /*agent*/,
                                             BBL::PrintParams      /*params*/,
                                             BBL::OnUpdateStatusFn /*update_fn*/,
                                             BBL::WasCancelledFn   /*cancel_fn*/)
{
    return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
}
