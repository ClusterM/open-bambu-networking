#pragma once

// Shared helpers for LAN-FTPS + project_file MQTT print submission.
//
// The LAN `run_local_print_job` and the cloud `run_cloud_print_job`
// pipelines both need to (a) pick a printer-friendly remote filename,
// (b) push the .3mf over FTPS to /cache/ on the printer, and (c) build
// a `{"print":{"command":"project_file", ...}}` payload for MQTT. We
// keep those three concerns here so both pipelines stay in sync.

#include <cstdint>
#include <functional>
#include <string>

#include "obn/bambu_networking.hpp"

namespace obn::print_job {

// Computes the printer-side filename we upload and later reference in
// the project_file MQTT payload. Uses project_name/task_name when
// possible and falls back to the basename of params.filename.
std::string pick_remote_name(const BBL::PrintParams& p);

// Performs the actual FTPS STOR of params.filename to remote_path,
// streaming progress through update_fn as PrintingStageUpload. Obeys
// cancel_fn. Returns 0 on success or a BAMBU_NETWORK_ERR_* code
// (err_code_on_failure is used for generic transport/upload errors; a
// cancellation is always reported as BAMBU_NETWORK_ERR_CANCELED).
int ftp_upload(const BBL::PrintParams& p,
               const std::string&      remote_path,
               const std::string&      ca_file,
               BBL::OnUpdateStatusFn   update_fn,
               BBL::WasCancelledFn     cancel_fn,
               int                     err_code_on_failure,
               std::uint64_t&          total_bytes_out);

// Options controlling the project_file payload we publish over MQTT.
// Cloud print fills in the real ids/url; LAN print uses "0" for ids
// and ftp://<file> for url.
struct ProjectFileOpts {
    std::string file_path;    // leading-slash path on the printer FS ("/cache/x.gcode.3mf")
    std::string url;          // "ftp://<file_path>" for LAN, or a presigned HTTPS URL for cloud
    std::string md5;          // hex MD5 of the uploaded file; "" if unknown
    std::string project_id{"0"};
    std::string profile_id{"0"};
    std::string task_id{"0"};
    std::string subtask_id{"0"};
};

std::string build_project_file_json(const BBL::PrintParams& p,
                                    const ProjectFileOpts&  opts);

} // namespace obn::print_job
