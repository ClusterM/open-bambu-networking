#pragma once

namespace obn {

// Snapshot of PrintParams::use_ssl_for_ftp from the last job that received
// PrintParams (local print, send-to-sdcard, cloud+LAN upload, etc.). Other
// code (cover_cache, ft_tunnel) reads this when it has no PrintParams.
// Defaults to true; reset to true on disconnect_printer.

void print_params_set_use_ssl_for_ftp(bool v);
bool print_params_get_use_ssl_for_ftp();

} // namespace obn
