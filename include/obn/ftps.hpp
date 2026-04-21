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
// connect/login, STOR (upload), DELE, SIZE, LIST, QUIT.

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

// One entry returned from `list_entries`. Field values that the server
// didn't provide are left empty / zero.
struct Entry {
    std::string name;        // file name only, no directory prefix
    std::uint64_t size = 0;  // bytes; 0 for directories
    std::uint64_t mtime = 0; // seconds since epoch (UTC) if known
    bool          is_dir = false;
};

// Streaming sink for `retr`. Called repeatedly with non-empty chunks of
// file data until the transfer completes. Return false to abort the
// transfer (RETR gets torn down cleanly afterwards).
using DataSinkFn = std::function<bool(const void* data, std::size_t len)>;

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

    // Structured directory listing. Issues LIST and runs a best-effort
    // parser on the `ls -l` style output (Bambu firmware does not
    // implement MLSD). On success *entries is populated and the
    // returned string is empty; on error *entries is cleared and the
    // returned string describes the failure.
    std::string list_entries(const std::string& path,
                             std::vector<Entry>* entries);

    // Downloads `remote_path`. The callback is invoked repeatedly with
    // streaming chunks; returning false from it aborts the transfer
    // cleanly. Returns empty on success, otherwise an error string.
    std::string retr(const std::string& remote_path, DataSinkFn sink);

    // SIZE: returns 0 and an empty error on success, with *size_out set;
    // any non-empty return value indicates a protocol/transport error.
    std::string size(const std::string& remote_path,
                     std::uint64_t* size_out);

    // Deletes a remote file. Returns empty string on success.
    std::string dele(const std::string& remote_path);

    // Change working directory. Returns empty on success (reply 250),
    // otherwise a human-readable error (used as a cheap "does this
    // storage mount exist?" probe - 550 comes back when the path is
    // missing, which we convert to a non-empty error).
    std::string cwd(const std::string& path);

    // Graceful shutdown (QUIT + TLS close + TCP close). Safe to call on a
    // disconnected client.
    void quit();

private:
    std::unique_ptr<Impl> p_;
};

} // namespace obn::ftps
