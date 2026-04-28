// MJPEG-over-TLS reader for Bambu's port-6000 protocol.
//
// A1 / A1 mini / P1 / P1P expose their preview camera as a TLS-wrapped
// TCP stream on port 6000. The wire format is documented in
// OpenBambuAPI/video.md and reverse-engineered in stubs/BambuSource.cpp:
// after a TLS handshake and an 80-byte auth packet, the printer keeps
// pushing 16-byte frame headers + JPEG payloads indefinitely.
//
// This module wraps that protocol into a small `Reader` interface so
// the macOS BambuPlayer.mm can consume it the same way the C-API in
// BambuSource.cpp does -- without dragging in the entire CTRL bridge,
// FTPS client, or PrinterFileSystem state. The reader runs its own
// background thread and exposes a non-blocking try_pull() that hands
// back the latest JPEG frame, mailbox-style (drop on stall to keep a
// slow consumer from accumulating arbitrary backlog).
//
// Currently only used by stubs/BambuPlayer.mm; the C-API in
// stubs/BambuSource.cpp keeps its own inline implementation because
// it shares state (mjpg_io_mu, frame_buf reuse, the CTRL-mode switch)
// with the rest of the tunnel and isn't worth refactoring just for
// this. If we ever stand up a Windows BambuPlayer equivalent, this
// header is the natural place to grow that out from.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "source_log.hpp"

namespace obn::mjpg {

struct Config {
    std::string host;
    int         port   = 6000;
    std::string user   = "bblp";
    std::string passwd;
    int         connect_timeout_ms = 5000;
};

// One JPEG frame handed back to the caller. `jpeg` is borrowed: it
// points into storage owned by the reader and is invalidated by the
// next try_pull() call.
struct Frame {
    const std::uint8_t* jpeg     = nullptr;
    std::size_t         size     = 0;
    std::uint64_t       dt_100ns = 0;
    int                 itrack   = 0;
    int                 flags    = 0;
};

enum PullResult {
    Pull_Ok          = 0,
    Pull_StreamEnd   = 1,
    Pull_WouldBlock  = 2,
    Pull_Error       = -1,
};

class Reader {
public:
    virtual ~Reader() = default;

    // Open TCP+TLS, send auth packet, kick off the reader thread.
    // Returns 0 on success; -1 on dial / handshake / auth failure
    // (use obn::source::get_last_error for a human message).
    virtual int start(const Config& cfg) = 0;

    // Non-blocking. Pull_Ok on success (out->jpeg valid until next
    // call), Pull_WouldBlock if no frame ready, Pull_StreamEnd once
    // when the server closes the connection (then Pull_Error).
    virtual PullResult try_pull(Frame* out) = 0;

    // Idempotent.
    virtual void stop() = 0;
};

// Logger forwarded to obn::source::log_*. A null logger maps to the
// noop implementation.
std::unique_ptr<Reader> make_reader(
    obn::source::Logger logger, void* log_ctx);

} // namespace obn::mjpg
