#pragma once

// Implicit-TLS FTPS client for Bambu printers.
//
// Bambu printers speak a quirky flavour of FTPS:
//   * Implicit TLS on port 990 (the TLS handshake starts immediately after
//     TCP connect; there is no AUTH TLS command).
//   * Self-signed server certificate. We verify against the bundled
//     printer.cer chain if provided, otherwise fall back to "no-verify".
//   * PASV replies include an unreachable IP (often 0.0.0.0 or the
//     printer's private IP). We must ignore that IP and connect the data
//     channel back to the control-connection host.
//   * The data channel also runs TLS (PROT P), and some firmwares expect
//     the data socket to reuse the control socket's TLS session, which we
//     opt into via SSL_SESSION_dup when available.
//
// Only the operations needed by print and probe paths are implemented:
// connect/login, STOR (upload), DELE, SIZE, MLSD/LIST, QUIT.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace obn::ftps {

struct ConnectConfig {
    std::string host;
    int         port       = 990;
    std::string username   = "bblp";
    std::string password;

    // Path to a CA bundle (PEM). If empty, TLS verification is disabled
    // (matching the behaviour we already use for MQTT against printers
    // that present a self-signed cert without a matching SAN).
    std::string ca_file;

    // Seconds for individual read/write syscalls on the control
    // connection. Transfers use a separate timeout.
    int control_timeout_s = 10;
    int data_timeout_s    = 60;
};

// (uploaded_bytes, total_bytes) - total may be 0 if unknown. Return false
// to abort the transfer.
using ProgressFn = std::function<bool(std::uint64_t uploaded,
                                      std::uint64_t total)>;

class Client {
public:
    struct Impl;
    Client();
    ~Client();

    Client(const Client&)            = delete;
    Client& operator=(const Client&) = delete;

    // Establishes the TLS control connection, authenticates, switches to
    // binary mode and enables PROT P on data transfers. Returns an empty
    // string on success or a human-readable error description.
    std::string connect(const ConnectConfig& cfg);

    // Uploads the file at `local_path` to `remote_path` on the printer's
    // storage. `remote_path` is relative to the printer's root and may
    // contain sub-directories (we send them verbatim to STOR).
    // `progress` is optional and may be nullptr.
    std::string stor(const std::string& local_path,
                     const std::string& remote_path,
                     ProgressFn         progress);

    // One-shot LIST for sanity tests / diagnostics. Returns the raw LIST
    // body joined with newlines, or an error in `err_out` (err_out is
    // empty on success).
    std::string list(const std::string& path, std::string& err_out);

    // Deletes a remote file. Returns empty string on success.
    std::string dele(const std::string& remote_path);

    // Graceful shutdown (QUIT + TLS close + TCP close). Safe to call on a
    // disconnected client.
    void quit();

private:
    std::unique_ptr<Impl> p_;
};

} // namespace obn::ftps
