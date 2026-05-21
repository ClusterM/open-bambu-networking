#pragma once

#include <string>

// Utility helpers for capturing and storing printer TLS certificates, used by
// Agent::install_device_cert() to mirror the behaviour of Bambu's own plugin
// which stashes each printer's self-signed server certificate in the user's
// Studio config dir.
namespace obn::cert_store {

// Builds "<config_dir>/certs/<dev_id>.pem". Does not touch the filesystem.
std::string device_cert_path(const std::string& config_dir, const std::string& dev_id);

// Ensures the parent directory of `file_path` exists (mkdir -p semantics).
// Returns true on success or if the directory already exists.
bool ensure_parent_dir(const std::string& file_path);

// Opens a short-lived TLS connection to `host:port`, captures the peer
// certificate leaf and writes it as PEM to `out_pem_path`. No chain
// verification is performed (TOFU bootstrap snapshot only).
// `tls_sni` is sent as SNI when non-empty (use dev_id/serial); otherwise `host`.
bool capture_peer_cert_pem(const std::string& host,
                           int                port,
                           int                timeout_ms,
                           const std::string& out_pem_path,
                           const std::string& tls_sni = {});

} // namespace obn::cert_store
