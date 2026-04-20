// libBambuSource.so: LAN-only video source for Bambu Lab printers, as
// consumed by Bambu Studio's `gstbambusrc` element.
//
// Bambu Studio loads this library via NetworkAgent::get_bambu_source_entry()
// (`dlopen` of `<data_dir>/plugins/libBambuSource.so`). We support both
// LAN video protocols:
//
//   * MJPG over TLS on port 6000 - used by A1 / A1 mini / P1 / P1P.
//     Protocol is the 80-byte auth packet + 16-byte frame headers
//     documented in OpenBambuAPI/video.md and implemented below.
//
//   * RTSPS on port 322 - used by X1 / P1S / P2S. We delegate the
//     heavy lifting to the system's GStreamer rtspsrc element, parse
//     out H.264 access units via appsink, and hand them back to
//     gstbambusrc as Bambu_Samples. That gives us TLS, auth, and
//     robust TCP framing "for free".
//
// URL formats we accept (all three appear in Studio's source):
//
//   bambu:///local/<ip>?port=6000&user=<u>&passwd=<p>&...
//   bambu:///local/<ip>.?port=6000&user=<u>&passwd=<p>&...        (legacy)
//       -> TCP/TLS MJPG on port 6000 (P1/A1 firmware protocol)
//
//   bambu:///rtsps___<user>:<passwd>@<ip>/streaming/live/1?proto=rtsps
//   bambu:///rtsp___<user>:<passwd>@<ip>/streaming/live/1?proto=rtsp
//       -> RTSP(S) on port 322 (X1/P1S/P2S firmware protocol); we use the
//          system's GStreamer rtspsrc element to pull H.264.
//
// Any extra query parameters (device=, net_ver=, dev_ver=, cli_id=, ...)
// are ignored. The printer only cares about the auth packet (MJPG) or
// the RTSP DESCRIBE/SETUP/PLAY exchange.
//
// Protocol summary (see OpenBambuAPI/video.md for the canonical spec):
//
//   1. TLS handshake over TCP on <ip>:<port>; printer cert is self-signed,
//      we do NOT verify it (same as the stock plugin).
//   2. Send 80-byte auth packet:
//        [0..3]   little-endian uint32 = 0x40          (payload size)
//        [4..7]   little-endian uint32 = 0x3000        (type: auth)
//        [8..11]  little-endian uint32 = 0             (flags)
//        [12..15] little-endian uint32 = 0
//        [16..47] 32 bytes: ASCII username, NUL-padded
//        [48..79] 32 bytes: ASCII password, NUL-padded
//   3. Server then streams frames indefinitely. Each frame is:
//        16-byte header (payload_size u32, itrack u32, flags u32, pad u32)
//        followed by `payload_size` bytes of JPEG data (FF D8 ... FF D9).
//
// gstbambusrc contract (see gstbambusrc.c):
//
//   Bambu_Create    (parse URL, allocate tunnel)
//   Bambu_SetLogger (attach log callback)
//   Bambu_Open      (blocking connect + TLS handshake + auth)
//   Bambu_StartStream(video=1) until it returns != would_block
//   Bambu_GetStreamCount / Bambu_GetStreamInfo   (once)
//   loop {
//     Bambu_ReadSample()      // would_block is fine, gst sleeps 33 ms
//     ...if success, emit buffer...
//   }
//   Bambu_Close + Bambu_Destroy at teardown.
//
// Thread safety: `gstbambusrc` calls us from a single streaming thread per
// tunnel; we only need to be safe against the logger callback being fired
// from that same thread. No global locks are held.

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <jpeglib.h>
#include <png.h>
#include <zlib.h>

#include "obn/ftps.hpp"
#include "obn/json_lite.hpp"

#if defined(_WIN32)
#    define OBN_EXPORT extern "C" __declspec(dllexport)
#else
#    define OBN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// -----------------------------------------------------------------------
// Types redeclared from BambuTunnel.h. We do NOT include the original
// header because it is part of Bambu Studio's proprietary build tree
// (GPL-incompatible). All layout / enum values are checked against
// OpenBambuAPI documentation and gstbambusrc.c behaviour.
// -----------------------------------------------------------------------

extern "C" {

typedef void* Bambu_Tunnel;
using tchar = char; // Linux only for now; Windows would need wchar_t here.

enum Bambu_StreamType { VIDE = 0, AUDI = 1 };
enum Bambu_VideoSubType { AVC1 = 0, MJPG = 1 };
enum Bambu_FormatType {
    video_avc_packet = 0,
    video_avc_byte_stream,
    video_jpeg,
    audio_raw,
    audio_adts,
};
enum Bambu_Error { Bambu_success = 0, Bambu_stream_end, Bambu_would_block, Bambu_buffer_limit };

struct Bambu_StreamInfo {
    int type;       // Bambu_StreamType
    int sub_type;   // Bambu_VideoSubType / Bambu_AudioSubType
    union {
        struct {
            int width;
            int height;
            int frame_rate;
        } video;
        struct {
            int sample_rate;
            int channel_count;
            int sample_size;
        } audio;
    } format;
    int                   format_type;    // Bambu_FormatType
    int                   format_size;
    int                   max_frame_size;
    unsigned char const*  format_buffer;
};

struct Bambu_Sample {
    int                   itrack;
    int                   size;
    int                   flags;
    unsigned char const*  buffer;
    unsigned long long    decode_time; // 100ns units, per gstbambusrc expectations
};

using Logger = void (*)(void* context, int level, tchar const* msg);

} // extern "C"

// -----------------------------------------------------------------------
// Logger plumbing. gstbambusrc's `_log` helper calls `Bambu_FreeLogMsg(msg)`
// after printing each message, so we MUST pass messages through malloc'd
// storage and free them there. Use strdup/free to keep that contract.
// -----------------------------------------------------------------------

namespace {

void noop_logger(void*, int, tchar const*) {}

// A file-backed mirror of every log line. Studio's gstbambusrc routes
// our log callback through GST_DEBUG, which is invisible without
// GST_DEBUG=bambusrc:5 in the environment. Duplicating into a stable
// on-disk path lets us debug the camera pipeline regardless of how the
// user launched Bambu Studio.
//
// Path: $XDG_STATE_HOME/bambu-studio/obn-bambusource.log, or
//       $HOME/.local/state/bambu-studio/obn-bambusource.log,
//       or /tmp/obn-bambusource.log as last resort.
FILE* mirror_log_fp()
{
    static FILE* fp = []() -> FILE* {
        const char* paths[3] = {nullptr, nullptr, "/tmp/obn-bambusource.log"};
        static std::string p0, p1;
        if (const char* xdg = std::getenv("XDG_STATE_HOME")) {
            p0 = std::string(xdg) + "/bambu-studio/obn-bambusource.log";
            paths[0] = p0.c_str();
        }
        if (const char* home = std::getenv("HOME")) {
            p1 = std::string(home) + "/.local/state/bambu-studio/obn-bambusource.log";
            paths[1] = p1.c_str();
        }
        for (const char* path : paths) {
            if (!path) continue;
            if (FILE* f = std::fopen(path, "a")) {
                std::setvbuf(f, nullptr, _IOLBF, 0);
                std::fprintf(f, "--- obn libBambuSource opened ---\n");
                return f;
            }
        }
        return nullptr;
    }();
    return fp;
}

// Simple printf-style helper used by the tunnel internals. Writes to the
// (optional) logger callback AND to the on-disk mirror so we don't need
// GST_DEBUG set to diagnose handshake failures.
[[gnu::format(printf, 3, 4)]]
void log_fmt(Logger logger, void* ctx, const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (FILE* fp = mirror_log_fp()) {
        auto now = std::chrono::system_clock::now();
        auto tt  = std::chrono::system_clock::to_time_t(now);
        char ts[32];
        std::strftime(ts, sizeof(ts), "%F %T",
                      std::localtime(&tt));
        std::fprintf(fp, "%s %s\n", ts, buf);
    }

    if (logger) {
        // Studio expects a heap-allocated buffer that it will free via
        // Bambu_FreeLogMsg. strdup() is the idiomatic way.
        logger(ctx, /*level=*/0, strdup(buf));
    }
}

// Last error message kept in a tiny thread-local buffer so
// Bambu_GetLastErrorMsg() has something to return.
thread_local std::string g_last_error;

void set_last_error(const char* msg)
{
    g_last_error.assign(msg ? msg : "");
}

// -----------------------------------------------------------------------
// URL parser. Bambu URLs:
//   bambu:///local/<ip>?port=6000&user=<u>&passwd=<p>&...
//   bambu:///local/<ip>.?port=6000&...       (note the trailing dot)
// -----------------------------------------------------------------------

enum class Scheme {
    Local, // MJPG over TCP/TLS on <port> (default 6000)
    Rtsps, // RTSPS on <port> (default 322)
    Rtsp,  // plain RTSP on <port> (default 554)
};

struct TunnelUrl {
    Scheme      scheme = Scheme::Local;
    std::string host;
    int         port = 6000;
    std::string user = "bblp";
    std::string passwd;
    std::string device;
    std::string path = "/streaming/live/1"; // RTSP(S) only
};

std::string url_decode(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                return -1;
            };
            int a = hex(s[i + 1]);
            int b = hex(s[i + 2]);
            if (a >= 0 && b >= 0) {
                out.push_back(static_cast<char>((a << 4) | b));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i] == '+' ? ' ' : s[i]);
    }
    return out;
}

bool parse_url(const std::string& url, TunnelUrl* out)
{
    // Recognise the three URL shapes Studio hands us. Whichever it is,
    // strip the prefix and leave `rest` = "<...>[?query]".
    static const std::string p_local  = "bambu:///local/";
    static const std::string p_rtsps  = "bambu:///rtsps___";
    static const std::string p_rtsp   = "bambu:///rtsp___";

    std::string rest;
    if (url.compare(0, p_local.size(), p_local) == 0) {
        out->scheme = Scheme::Local;
        out->port   = 6000;
        rest = url.substr(p_local.size());
    } else if (url.compare(0, p_rtsps.size(), p_rtsps) == 0) {
        out->scheme = Scheme::Rtsps;
        out->port   = 322;
        rest = url.substr(p_rtsps.size());
    } else if (url.compare(0, p_rtsp.size(), p_rtsp) == 0) {
        out->scheme = Scheme::Rtsp;
        out->port   = 554;
        rest = url.substr(p_rtsp.size());
    } else {
        // Bare "<ip>:<port>/..." fallback.
        out->scheme = Scheme::Local;
        out->port   = 6000;
        rest = url;
    }

    // Split host.part vs ?query.
    auto q_pos = rest.find('?');
    std::string host_part = (q_pos == std::string::npos) ? rest : rest.substr(0, q_pos);
    std::string query     = (q_pos == std::string::npos) ? ""   : rest.substr(q_pos + 1);

    if (out->scheme == Scheme::Rtsps || out->scheme == Scheme::Rtsp) {
        // "<user>:<passwd>@<host>[:port]/<path>" (path is required and
        // Studio always sends "streaming/live/1").
        auto at_pos = host_part.find('@');
        if (at_pos != std::string::npos) {
            std::string userinfo = host_part.substr(0, at_pos);
            host_part            = host_part.substr(at_pos + 1);
            auto col             = userinfo.find(':');
            if (col != std::string::npos) {
                out->user   = url_decode(userinfo.substr(0, col));
                out->passwd = url_decode(userinfo.substr(col + 1));
            } else {
                out->user = url_decode(userinfo);
            }
        }
        auto slash = host_part.find('/');
        if (slash != std::string::npos) {
            out->path = host_part.substr(slash); // includes leading '/'
            host_part = host_part.substr(0, slash);
        }
        // Host may still carry ":<port>". Fall through to the colon
        // handling below.
    } else {
        // Legacy MJPG URL: "<ip>.?port=..." -> trim trailing . and /
        while (!host_part.empty() &&
               (host_part.back() == '/' || host_part.back() == '.'))
            host_part.pop_back();
    }

    // Optional ":<port>" in host_part.
    auto colon = host_part.find(':');
    if (colon != std::string::npos) {
        out->host = host_part.substr(0, colon);
        try {
            out->port = std::stoi(host_part.substr(colon + 1));
        } catch (...) {
            return false;
        }
    } else {
        out->host = host_part;
    }

    // Parse query. Local-scheme URLs carry user/passwd here;
    // RTSP(S) URLs carry them in the userinfo above, so these are
    // effectively a no-op for those.
    size_t i = 0;
    while (i < query.size()) {
        auto amp = query.find('&', i);
        if (amp == std::string::npos) amp = query.size();
        auto kv = query.substr(i, amp - i);
        auto eq = kv.find('=');
        std::string key = (eq == std::string::npos) ? kv : kv.substr(0, eq);
        std::string val = (eq == std::string::npos) ? "" : url_decode(kv.substr(eq + 1));
        if      (key == "port")   { try { out->port = std::stoi(val); } catch (...) { /* keep default */ } }
        else if (key == "user")   { out->user = val; }
        else if (key == "passwd") { out->passwd = val; }
        else if (key == "device") { out->device = val; }
        i = amp + 1;
    }

    return !out->host.empty() && out->port > 0;
}

// -----------------------------------------------------------------------
// OpenSSL one-time init. Called lazily from the first Bambu_Create to
// avoid paying the cost in Studio processes that never touch the camera.
// -----------------------------------------------------------------------

std::once_flag g_ssl_init_flag;
SSL_CTX*       g_ssl_ctx = nullptr;

void ssl_init_once()
{
    std::call_once(g_ssl_init_flag, []() {
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
        // Stock plugin accepts any TLS1.2+ handshake from the printer's
        // self-signed cert. We mirror that.
        g_ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (g_ssl_ctx) {
            SSL_CTX_set_min_proto_version(g_ssl_ctx, TLS1_VERSION);
            SSL_CTX_set_verify(g_ssl_ctx, SSL_VERIFY_NONE, nullptr);
        }
    });
}

// -----------------------------------------------------------------------
// Tunnel state. All network IO is synchronous (blocking) on purpose;
// gstbambusrc already runs us on a dedicated streaming thread.
// -----------------------------------------------------------------------

// Reasonable upper bound for a single 1280x720 JPEG frame. The stock
// camera tops out around 60 KB but we give ourselves a whole megabyte
// of headroom in case Bambu ships higher-res firmware later.
constexpr size_t kMaxFrameSize = 1u << 20;

// --------------------------------------------------------------------
// PrinterFileSystem CTRL state.
//
// Studio opens a port-6000 tunnel through us (Bambu_Create/Bambu_Open
// on `bambu:///local/<ip>.?port=6000&user=bblp&passwd=<code>&...`) and
// then calls `Bambu_StartStreamEx(tunnel, CTRL_TYPE=0x3001)`. After
// that point it stops asking for video and instead pushes JSON request
// strings through `Bambu_SendMessage(CTRL_TYPE)`, expecting JSON
// responses through `Bambu_ReadSample`. This is how Studio's Files tab
// (MediaFilePanel + PrinterFileSystem) enumerates, previews, and
// downloads files from the printer.
//
// We run those requests against the printer's FTPS service (port 990,
// same login as the MJPG tunnel) on a dedicated worker thread per
// tunnel. Responses are queued back to Studio through an out-queue;
// `Bambu_ReadSample` pops them one at a time. Each response is a
// single JSON object optionally followed by "\n\n" + binary blob (the
// format PrinterFileSystem::HandleResponse expects).
//
// Lifetime: Bambu_Open is synchronous, Bambu_StartStreamEx spawns the
// worker, Bambu_SendMessage enqueues a request (returns immediately),
// Bambu_ReadSample dequeues a reply (Bambu_would_block if none), and
// Bambu_Close signals the worker to drain and exit.
// --------------------------------------------------------------------

constexpr int kCtrlType = 0x3001;

// Matches PrinterFileSystem::Command enum (FILE_DEL, FILE_DOWNLOAD,
// REQUEST_MEDIA_ABILITY, ...). We don't redeclare the enum: these
// numbers are set in ABI stone on the Studio side.
constexpr int kCmdListInfo            = 0x0001;
constexpr int kCmdSubFile             = 0x0002;
constexpr int kCmdFileDel             = 0x0003;
constexpr int kCmdFileDownload        = 0x0004;
constexpr int kCmdFileUpload          = 0x0005;
constexpr int kCmdRequestMediaAbility = 0x0007;
constexpr int kCmdTaskCancel          = 0x1000;

// PrinterFileSystem result codes we emit back.
constexpr int kResOK           = 0;
constexpr int kResContinue     = 1;
constexpr int kResErrJson      = 2;
constexpr int kResErrPipe      = 3;
constexpr int kResErrCancel    = 4;
constexpr int kResFileNoExist  = 10;
constexpr int kResStorUnavail  = 17;
constexpr int kResApiUnsupport = 18;

struct CtrlRequest {
    int         cmdtype = 0;
    int         sequence = 0;
    std::string body; // full JSON text, including "{...}\n\n<blob>" if present
};

struct CtrlReply {
    // "wire bytes" already laid out in the PrinterFileSystem format:
    // "<json>\n\n<optional blob>". For multi-chunk responses (like
    // FILE_DOWNLOAD's progressive replies) the worker pushes one
    // CtrlReply per chunk, all with the same `sequence`.
    std::string data;
};

struct Tunnel {
    TunnelUrl        url;
    Logger           logger  = noop_logger;
    void*            log_ctx = nullptr;

    // ---- MJPG/TLS state (Scheme::Local) ----
    int              fd      = -1;
    SSL*             ssl     = nullptr;

    // ---- RTSP(S) state (Scheme::Rtsps/Rtsp) ----
    // Pipeline: rtspsrc ! rtph264depay ! h264parse ! appsink.
    // We keep only the pipeline element and the sink handle; the
    // internal elements are owned by the pipeline bin.
    GstElement*      pipeline = nullptr;
    GstAppSink*      appsink  = nullptr;
    // Subtype of the video carried by this tunnel, filled in by
    // Bambu_GetStreamInfo. MJPG for local-scheme tunnels, AVC1 for RTSP.
    int              sub_type = MJPG;

    // Bookkeeping for GetStreamInfo. We don't know the real frame rate
    // until we've observed several frames, so these are "advisory" and
    // Studio uses them only for display.
    int              width      = 1280;
    int              height     = 720;
    int              frame_rate = 15;

    // Reused across ReadSample calls so the Bambu_Sample::buffer pointer
    // stays valid until the NEXT ReadSample is invoked (matches what
    // gstbambusrc does with `g_memdup(sample.buffer, sample.size)`).
    std::vector<uint8_t> frame_buf;

    // Monotonic "decode_time" in the 100-ns units gstbambusrc feeds to
    // gstreamer. We derive it from a steady_clock zeroed at Open() time.
    std::chrono::steady_clock::time_point t0{};
    bool                                  started = false;

    // Cancellation flag set from a different thread by Bambu_Close.
    std::atomic<bool> closing{false};

    // Diagnostic counter; we log a line every Nth frame so the mirror
    // file tells us "stream is alive" without drowning in per-frame spam.
    std::uint64_t frame_count = 0;

    // ---- PrinterFileSystem CTRL state (Scheme::Local + CTRL_TYPE) ----
    // When Studio calls Bambu_StartStreamEx(CTRL_TYPE) on us, we close
    // the MJPG TCP socket we opened in Bambu_Open and switch to CTRL
    // mode: the tunnel now multiplexes JSON request/response messages
    // against FTPS on the same printer.
    bool             ctrl_mode = false;

    std::unique_ptr<obn::ftps::Client> ftp;
    // True if FTPS root == storage mount (P2S / USB-only printers).
    // False means /sdcard and /usb coexist under /.
    bool             root_is_storage = false;
    // One of "sdcard" / "usb" / "" (unknown). Used as the top-level
    // directory for list operations.
    std::string      storage_label;
    // Pre-computed prefix used when talking to the FTPS server:
    // "/sdcard" or "/usb" when both mounts exist, "" when root IS the
    // storage. Files live under <prefix>/<name>; special subtrees like
    // `timelapse/` and `ipcam/` hang off <prefix>/ too.
    std::string      ftp_prefix;

    // CTRL request inbox (Bambu_SendMessage pushes here) and reply
    // outbox (Bambu_ReadSample pops from here). Both guarded by ctrl_mu.
    std::mutex                 ctrl_mu;
    std::condition_variable    ctrl_cv;
    std::deque<CtrlRequest>    ctrl_in;
    std::deque<CtrlReply>      ctrl_out;
    // Sequence numbers the caller asked to cancel. The worker checks
    // this set between multi-chunk responses (e.g. long downloads) and
    // aborts cleanly if membership appears mid-request.
    std::unordered_set<int>    ctrl_cancelled;
    std::atomic<bool>          ctrl_stop{false};
    std::thread                ctrl_worker;

    // Scratch storage for the CtrlReply currently exposed via
    // Bambu_ReadSample's `sample->buffer`. We keep it alive until the
    // NEXT ReadSample call (same contract as frame_buf above).
    std::string                ctrl_current_reply;
};

void tunnel_close(Tunnel* t)
{
    if (!t) return;
    t->closing.store(true, std::memory_order_release);
    if (t->ssl) {
        // Best-effort shutdown; the printer doesn't care about clean
        // close_notify, and we'd rather exit fast than block on a
        // dying TLS connection.
        SSL_shutdown(t->ssl);
        SSL_free(t->ssl);
        t->ssl = nullptr;
    }
    if (t->fd >= 0) {
        ::shutdown(t->fd, SHUT_RDWR);
        ::close(t->fd);
        t->fd = -1;
    }
    if (t->pipeline) {
        // NULL state tears down rtspsrc cleanly; then unref. Note we do
        // NOT unref t->appsink separately: the appsink lives inside the
        // pipeline bin and owning a ref on it would leak until process
        // exit.
        gst_element_set_state(t->pipeline, GST_STATE_NULL);
        t->appsink = nullptr;
        gst_object_unref(t->pipeline);
        t->pipeline = nullptr;
    }
}

// Resolve-and-connect with a total deadline. Returns -1 on error.
int dial(const std::string& host, int port, int timeout_ms)
{
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    char port_s[16];
    std::snprintf(port_s, sizeof(port_s), "%d", port);
    int gai = ::getaddrinfo(host.c_str(), port_s, &hints, &res);
    if (gai != 0 || !res) {
        set_last_error(gai_strerror(gai));
        return -1;
    }

    int fd = -1;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    for (auto* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        // Keep connect() from blocking forever; 5 s matches what the
        // stock plugin uses (observed via strace).
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
        if (std::chrono::steady_clock::now() > deadline) break;
    }
    freeaddrinfo(res);
    if (fd < 0) set_last_error("connect failed");
    return fd;
}

// Writes `len` bytes via SSL, handling short writes. Returns 0 on OK.
int ssl_write_all(SSL* ssl, const void* buf, size_t len)
{
    const auto* p = static_cast<const uint8_t*>(buf);
    size_t sent = 0;
    while (sent < len) {
        int n = SSL_write(ssl, p + sent, static_cast<int>(len - sent));
        if (n <= 0) {
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
            return -1;
        }
        sent += static_cast<size_t>(n);
    }
    return 0;
}

// Reads exactly `len` bytes. Returns 0 on OK, 1 on EOF, -1 on error.
int ssl_read_all(Tunnel* t, void* buf, size_t len)
{
    auto* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < len) {
        if (t->closing.load(std::memory_order_acquire)) return -1;
        int n = SSL_read(t->ssl, p + got, static_cast<int>(len - got));
        if (n <= 0) {
            int err = SSL_get_error(t->ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
            if (err == SSL_ERROR_ZERO_RETURN) return 1;
            return -1;
        }
        got += static_cast<size_t>(n);
    }
    return 0;
}

// -----------------------------------------------------------------------
// GStreamer init (for RTSP(S) tunnels). Lazy so we only pay the cost if
// the user actually plays video on an X1/P2S-class printer.
// -----------------------------------------------------------------------

std::once_flag g_gst_init_flag;

// Reroutes GStreamer's default debug output into obn-bambusource.log so
// we can diagnose rtspsrc handshake failures without having to set
// GST_DEBUG in Studio's environment. We keep the threshold low by
// default (WARNING + ERROR only); OBN_GST_DEBUG=<level> raises it.
void gst_log_handler(GstDebugCategory* cat, GstDebugLevel level,
                     const gchar* file, const gchar* function, gint line,
                     GObject* /*object*/, GstDebugMessage* message,
                     gpointer /*user_data*/)
{
    if (level > gst_debug_category_get_threshold(cat)) return;
    FILE* fp = mirror_log_fp();
    if (!fp) return;
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%F %T", std::localtime(&tt));
    std::fprintf(fp, "%s [gst %s] %s:%d:%s: %s\n",
                 ts, gst_debug_level_get_name(level),
                 file ? file : "?",
                 line, function ? function : "?",
                 gst_debug_message_get(message));
}

void gst_init_once()
{
    std::call_once(g_gst_init_flag, []() {
        gst_init(nullptr, nullptr);
        // Default to WARNING so we only pick up real failures; raise
        // it selectively via OBN_GST_DEBUG=INFO (or 4) if needed.
        GstDebugLevel lvl = GST_LEVEL_WARNING;
        if (const char* env = std::getenv("OBN_GST_DEBUG")) {
            int n = std::atoi(env);
            if (n > 0 && n <= GST_LEVEL_MEMDUMP) {
                lvl = static_cast<GstDebugLevel>(n);
            } else if (std::strcmp(env, "INFO") == 0) {
                lvl = GST_LEVEL_INFO;
            } else if (std::strcmp(env, "DEBUG") == 0) {
                lvl = GST_LEVEL_DEBUG;
            }
        }
        gst_debug_set_default_threshold(lvl);
        gst_debug_remove_log_function(gst_debug_log_default);
        gst_debug_add_log_function(gst_log_handler, nullptr, nullptr);
    });
}

// Formats the rtsp(s) URL the way gst-rtsp-server expects: "<scheme>://
// <host>:<port><path>". User/password are passed through rtspsrc
// properties (user-id / user-pw), NOT embedded in the URI, because
// rtspsrc percent-decodes userinfo and silently mangles access codes
// that contain '+', '/', or '&'.
std::string build_rtsp_uri(const TunnelUrl& u)
{
    const char* scheme = (u.scheme == Scheme::Rtsps) ? "rtsps" : "rtsp";
    return std::string(scheme) + "://" + u.host + ":" +
           std::to_string(u.port) + u.path;
}

// Pad-added callback for rtspsrc. rtspsrc has "sometimes" src pads that
// only exist after SDP negotiation; gst_parse_launch's auto-linking
// handles this in simple cases, but we've seen the link silently fail
// and surface as "streaming stopped, reason not-linked" in Studio. Doing
// the link by hand here removes that ambiguity.
// Holds the pad-added closure arguments. Allocated with `new`, freed
// through g_signal_connect_data's destroy notify so it doesn't leak
// across repeated Bambu_Open calls.
struct PadCtx { GstElement* depay; Tunnel* t; };

// Tunnel passed as user_data so we can log through the same mirror file.
extern "C" void rtsp_pad_added(GstElement* /*src*/, GstPad* new_pad,
                               gpointer user_data)
{
    auto* ctx = static_cast<PadCtx*>(user_data);
    GstElement* depay = ctx->depay;
    Tunnel*     t     = ctx->t;

    gchar* pad_name = gst_pad_get_name(new_pad);
    GstCaps* caps = gst_pad_get_current_caps(new_pad);
    if (!caps) caps = gst_pad_query_caps(new_pad, nullptr);
    gchar* caps_str = caps ? gst_caps_to_string(caps) : g_strdup("(null)");
    log_fmt(t->logger, t->log_ctx,
            "rtsp_pad_added: pad=%s caps=%.400s linked=%d",
            pad_name ? pad_name : "?",
            caps_str ? caps_str : "?",
            (int)GST_PAD_IS_LINKED(new_pad));

    if (GST_PAD_IS_LINKED(new_pad)) {
        if (pad_name) g_free(pad_name);
        if (caps_str) g_free(caps_str);
        if (caps)     gst_caps_unref(caps);
        return;
    }

    GstPad* sink_pad = gst_element_get_static_pad(depay, "sink");
    if (!sink_pad || GST_PAD_IS_LINKED(sink_pad)) {
        log_fmt(t->logger, t->log_ctx,
                "rtsp_pad_added: depay sink unavailable (pad=%p linked=%d)",
                sink_pad, sink_pad ? (int)GST_PAD_IS_LINKED(sink_pad) : -1);
        if (sink_pad) gst_object_unref(sink_pad);
        if (pad_name) g_free(pad_name);
        if (caps_str) g_free(caps_str);
        if (caps)     gst_caps_unref(caps);
        return;
    }

    // Accept the first video/H264 pad we see. We pick it up from
    // either caps["media"]=="video" + caps["encoding-name"]=="H264"
    // (the interesting one) OR from pad name prefix "recv_rtp_src_"
    // (which rtspsrc always uses for its RTP source pads). Being
    // lenient on the criteria matters because some gst versions lie
    // about current_caps at pad_added time.
    gboolean is_video = FALSE;
    if (caps) {
        guint n = gst_caps_get_size(caps);
        for (guint i = 0; i < n && !is_video; ++i) {
            const GstStructure* s = gst_caps_get_structure(caps, i);
            const gchar* media = gst_structure_get_string(s, "media");
            const gchar* enc   = gst_structure_get_string(s, "encoding-name");
            if (media && g_ascii_strcasecmp(media, "video") == 0) is_video = TRUE;
            if (enc   && g_ascii_strcasecmp(enc,   "H264")  == 0) is_video = TRUE;
        }
    }
    gboolean name_looks_rtp = pad_name && g_str_has_prefix(pad_name, "recv_rtp_src_");

    GstPadLinkReturn r = GST_PAD_LINK_NOFORMAT;
    if (is_video || name_looks_rtp) {
        r = gst_pad_link(new_pad, sink_pad);
        log_fmt(t->logger, t->log_ctx,
                "rtsp_pad_added: link pad=%s -> depay.sink = %d",
                pad_name ? pad_name : "?", (int)r);
    } else {
        log_fmt(t->logger, t->log_ctx,
                "rtsp_pad_added: skipping non-video pad=%s",
                pad_name ? pad_name : "?");
    }

    gst_object_unref(sink_pad);
    if (pad_name) g_free(pad_name);
    if (caps_str) g_free(caps_str);
    if (caps)     gst_caps_unref(caps);
}


// Builds the pipeline, wires authentication, and starts PLAYING. Blocks
// until the first preroll (ASYNC_DONE on the bus) or until rtspsrc
// fails -- returns Bambu_success or -1 accordingly.
//
// Pipeline layout (built by hand, not gst_parse_launch, because the
// auto-link from rtspsrc's sometimes-pad into rtph264depay fires a
// race in Studio's long-lived GStreamer context that manifests as
// "streaming stopped, reason not-linked" on the second play):
//
//   rtspsrc [location, user-id, user-pw, protocols=tcp, latency=200,
//            tls-validation-flags=0, do-retransmission=false]
//      --(pad-added, H264)--> rtph264depay
//      --> h264parse --> avdec_h264 --> videoconvert --> videoscale
//      --> capsfilter [video/x-raw,width=1280,height=720]
//      --> jpegenc [quality=80, idct-method=ifast]
//      --> appsink name=sink [emit-signals=false, drop=false,
//                             max-buffers=4, sync=false]
//
// See comments inside for why each property has the value it does.
[[maybe_unused]] int open_rtsp(Tunnel* t)
{
    gst_init_once();

    const std::string uri = build_rtsp_uri(t->url);
    log_fmt(t->logger, t->log_ctx, "open_rtsp: uri=%s user=%s",
            uri.c_str(), t->url.user.c_str());

    // Element factory lookup. We pick the H.264 decoder at runtime
    // because Bambu Studio ships its own libav* in <install>/bin and
    // those symbol-conflict with the system's gst-libav plugin
    // (`module_open failed: libavformat.so.61: undefined symbol
    // av_mastering_display_metadata_alloc_size, version LIBAVUTIL_59`).
    // Preferred order: openh264dec (pure userspace, no ffmpeg ABI tie),
    // then vah264dec (Intel VA-API), then vulkanh264device1dec,
    // then avdec_h264 as last resort (mostly for systems that don't
    // ship Studio's bundled libav*).
    static const char* const kH264Decoders[] = {
        "openh264dec", "vah264dec", "vulkanh264device1dec", "avdec_h264",
    };
    GstElement* pipeline = gst_pipeline_new("obn-rtsp");
    GstElement* src       = gst_element_factory_make("rtspsrc",      "src");
    GstElement* depay     = gst_element_factory_make("rtph264depay", "depay");
    GstElement* parse     = gst_element_factory_make("h264parse",    "parse");
    GstElement* dec       = nullptr;
    const char* dec_name  = nullptr;
    for (const char* name : kH264Decoders) {
        dec = gst_element_factory_make(name, "dec");
        if (dec) { dec_name = name; break; }
    }
    GstElement* conv      = gst_element_factory_make("videoconvert", "conv");
    GstElement* scale     = gst_element_factory_make("videoscale",   "scale");
    GstElement* capsf     = gst_element_factory_make("capsfilter",   "capsf");
    GstElement* enc       = gst_element_factory_make("jpegenc",      "enc");
    GstElement* sink      = gst_element_factory_make("appsink",      "sink");
    if (!pipeline || !src || !depay || !parse || !dec || !conv ||
        !scale || !capsf || !enc || !sink) {
        log_fmt(t->logger, t->log_ctx,
                "open_rtsp: missing GStreamer element(s): "
                "pipeline=%p src=%p depay=%p parse=%p dec=%p conv=%p "
                "scale=%p capsf=%p enc=%p sink=%p",
                pipeline, src, depay, parse, dec, conv, scale, capsf,
                enc, sink);
        if (pipeline) gst_object_unref(pipeline);
        else {
            if (src)   gst_object_unref(src);
            if (depay) gst_object_unref(depay);
            if (parse) gst_object_unref(parse);
            if (dec)   gst_object_unref(dec);
            if (conv)  gst_object_unref(conv);
            if (scale) gst_object_unref(scale);
            if (capsf) gst_object_unref(capsf);
            if (enc)   gst_object_unref(enc);
            if (sink)  gst_object_unref(sink);
        }
        set_last_error("missing gst element");
        return -1;
    }

    // rtspsrc: latency=200 gives the jitterbuffer enough window to
    // reassemble multi-RTP-packet NALs over TCP without adding visible
    // wall-clock lag. tls-validation-flags=0 matches the stock plugin
    // which accepts the printer's self-signed cert.
    g_object_set(src,
                 "location",             uri.c_str(),
                 "user-id",              t->url.user.c_str(),
                 "user-pw",              t->url.passwd.c_str(),
                 "protocols",            4 /* GST_RTSP_LOWER_TRANS_TCP */,
                 "latency",              200,
                 "tls-validation-flags", 0,
                 "do-retransmission",    FALSE,
                 nullptr);

    log_fmt(t->logger, t->log_ctx,
            "open_rtsp: using H.264 decoder '%s'",
            dec_name ? dec_name : "(none!)");

    // h264parse config-interval=-1: insert SPS/PPS before every keyframe
    // so the decoder can always lock on, even if we join mid-stream.
    g_object_set(parse, "config-interval", -1, nullptr);

    // We transcode to 720p MJPG. Studio's camera widget is typically
    // much smaller than 1080p, and MJPG lets Studio use the same
    // lightweight path it has for A1/P1 cameras (one jpegdec per frame,
    // no H.264 decoder bin) -- which is what makes 30 fps actually
    // reach the screen without the "half speed" symptom.
    GstCaps* scaled_caps = gst_caps_from_string(
        "video/x-raw,width=1280,height=720");
    g_object_set(capsf, "caps", scaled_caps, nullptr);
    gst_caps_unref(scaled_caps);

    // jpegenc: quality=90 + islow IDCT keeps visible banding and
    // macroblock artefacts out of high-contrast transitions (e.g.
    // print-bed LED turning on) without costing enough CPU to matter
    // -- on a modern desktop this is ~7-10% of one core for 720p30.
    // ifast drops ~3% CPU but introduces a faint "ringing" on sharp
    // edges; we had complaints about that, so islow it is.
    g_object_set(enc, "quality", 90, "idct-method", 0 /* islow */, nullptr);

    // appsink: no callbacks (we poll from Bambu_ReadSample), backpressure
    // (drop=false max-buffers=4) instead of dropping frames, and
    // sync=false so we dispatch as soon as a frame is ready -- clocking
    // happens on Studio's side of the tunnel.
    g_object_set(sink,
                 "emit-signals", FALSE,
                 "drop",         FALSE,
                 "max-buffers",  4,
                 "sync",         FALSE,
                 nullptr);

    gst_bin_add_many(GST_BIN(pipeline), src, depay, parse, dec, conv,
                     scale, capsf, enc, sink, nullptr);

    // Static portion: depay -> parse -> dec -> conv -> scale -> capsf
    //                 -> enc -> appsink. rtspsrc -> depay is wired in
    //                 the pad-added handler below.
    if (!gst_element_link_many(depay, parse, dec, conv, scale, capsf,
                               enc, sink, nullptr)) {
        log_fmt(t->logger, t->log_ctx,
                "open_rtsp: gst_element_link_many failed");
        gst_object_unref(pipeline);
        set_last_error("link failed");
        return -1;
    }

    // Hook up rtspsrc's dynamic pad. The handler keeps a borrowed
    // reference on `depay` (kept alive by the pipeline bin) and a
    // borrowed pointer to our Tunnel (kept alive as long as the
    // pipeline exists, since we tear both down together in
    // tunnel_close). The closure itself is heap-allocated and freed
    // by glib when the signal handler is disconnected.
    auto* pad_ctx = new PadCtx{depay, t};
    gulong handler_id = g_signal_connect_data(
        src, "pad-added",
        G_CALLBACK(rtsp_pad_added),
        pad_ctx,
        /*destroy_notify=*/[](gpointer data, GClosure*) {
            delete static_cast<PadCtx*>(data);
        },
        GConnectFlags(0));
    log_fmt(t->logger, t->log_ctx,
            "open_rtsp: pad-added connected handler_id=%lu", handler_id);

    t->pipeline = pipeline;
    t->appsink  = GST_APP_SINK(sink);
    // gst_bin_add_many took ownership of `sink` -- but appsink is a
    // borrowed pointer from the pipeline's child list, so we don't
    // retain an extra ref here and we don't unref it in tunnel_close
    // either (the pipeline unref handles it).

    // Kick the pipeline. PLAYING is asynchronous; we wait up to 10 s for
    // ASYNC_DONE so we can surface a clean auth/handshake error instead
    // of the user seeing nothing until the first ReadSample times out.
    log_fmt(t->logger, t->log_ctx, "open_rtsp: set_state -> PLAYING");
    GstStateChangeReturn ret = gst_element_set_state(t->pipeline, GST_STATE_PLAYING);
    log_fmt(t->logger, t->log_ctx, "open_rtsp: set_state returned %d", (int)ret);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        log_fmt(t->logger, t->log_ctx, "open_rtsp: set_state PLAYING failed");
        set_last_error("gst PLAYING failed");
        return -1;
    }

    GstBus* bus = gst_element_get_bus(t->pipeline);
    const GstClockTime kWaitNs = 10ULL * GST_SECOND;
    bool ok = false;
    while (true) {
        GstMessage* msg = gst_bus_timed_pop_filtered(
            bus, kWaitNs,
            static_cast<GstMessageType>(GST_MESSAGE_ASYNC_DONE |
                                        GST_MESSAGE_ERROR |
                                        GST_MESSAGE_EOS));
        if (!msg) {
            log_fmt(t->logger, t->log_ctx, "open_rtsp: timeout waiting ASYNC_DONE");
            set_last_error("rtsp timeout");
            break;
        }
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ASYNC_DONE) {
            ok = true;
            gst_message_unref(msg);
            break;
        }
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError* err  = nullptr;
            gchar*  dbg  = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            log_fmt(t->logger, t->log_ctx,
                    "open_rtsp: gst error: %s (%s)",
                    err  ? err->message  : "?",
                    dbg  ? dbg           : "");
            set_last_error(err ? err->message : "rtsp error");
            if (err) g_error_free(err);
            if (dbg) g_free(dbg);
            gst_message_unref(msg);
            break;
        }
        // EOS pre-play: something is very wrong (printer refused).
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
            log_fmt(t->logger, t->log_ctx, "open_rtsp: EOS before playback");
            set_last_error("rtsp EOS at open");
            gst_message_unref(msg);
            break;
        }
        gst_message_unref(msg);
    }
    gst_object_unref(bus);

    if (!ok) return -1;

    // We transcode to MJPG inside the pipeline above. Studio's
    // consumer side treats this tunnel like an A1/P1 camera.
    t->sub_type  = MJPG;
    t->width     = 1280;
    t->height    = 720;
    t->frame_rate = 30;
    t->t0        = std::chrono::steady_clock::now();
    t->started   = true;
    log_fmt(t->logger, t->log_ctx, "open_rtsp: pipeline PLAYING (mjpg 1280x720)");
    return Bambu_success;
}

// Pulls the next H.264 access unit from the appsink. Returns
// Bambu_would_block if nothing is queued yet (gstbambusrc handles that
// by sleeping 33 ms and polling again). On error returns -1. EOS from
// the server maps to Bambu_stream_end.
int read_rtsp(Tunnel* t, Bambu_Sample* sample)
{
    if (!t->appsink) return -1;

    // Drain any pending bus messages so the mirror log actually names
    // the culprit when something dies mid-stream. Skip state-change /
    // ASYNC_DONE noise.
    if (t->pipeline) {
        GstBus* bus = gst_element_get_bus(t->pipeline);
        while (GstMessage* m = gst_bus_pop_filtered(
                   bus, static_cast<GstMessageType>(
                            GST_MESSAGE_ERROR | GST_MESSAGE_WARNING |
                            GST_MESSAGE_EOS | GST_MESSAGE_STREAM_STATUS))) {
            switch (GST_MESSAGE_TYPE(m)) {
            case GST_MESSAGE_ERROR: {
                GError* e = nullptr; gchar* d = nullptr;
                gst_message_parse_error(m, &e, &d);
                log_fmt(t->logger, t->log_ctx,
                        "read_rtsp: [%s] ERROR %s (%s)",
                        GST_OBJECT_NAME(GST_MESSAGE_SRC(m)),
                        e ? e->message : "?", d ? d : "");
                if (e) g_error_free(e);
                if (d) g_free(d);
                break;
            }
            case GST_MESSAGE_WARNING: {
                GError* e = nullptr; gchar* d = nullptr;
                gst_message_parse_warning(m, &e, &d);
                log_fmt(t->logger, t->log_ctx,
                        "read_rtsp: [%s] WARN %s (%s)",
                        GST_OBJECT_NAME(GST_MESSAGE_SRC(m)),
                        e ? e->message : "?", d ? d : "");
                if (e) g_error_free(e);
                if (d) g_free(d);
                break;
            }
            case GST_MESSAGE_EOS:
                log_fmt(t->logger, t->log_ctx,
                        "read_rtsp: [%s] EOS",
                        GST_OBJECT_NAME(GST_MESSAGE_SRC(m)));
                break;
            default: break;
            }
            gst_message_unref(m);
        }
        gst_object_unref(bus);
    }

    // Non-blocking pull: if we blocked here (e.g. 50 ms) we'd cap our
    // effective frame rate to 1 / timeout regardless of what the
    // camera sends. gstbambusrc already sleeps 33 ms when we return
    // Bambu_would_block, so returning immediately is strictly better.
    //
    // Note: we deliberately never call gst_app_sink_is_eos() here.
    // In gst 1.26 it returns true transiently whenever the queue is
    // drained between frames, which made Bambu_ReadSample claim
    // Bambu_stream_end between every successful sample. gstbambusrc
    // interprets that as "tunnel dead" and tears it down. Real EOS
    // is extremely rare on a live camera (only happens on manual
    // unblock / Bambu_Close), and the next Bambu_Open will re-establish
    // the pipeline anyway -- so returning Bambu_would_block until
    // samples come back is strictly safer.
    GstSample* gsample = gst_app_sink_try_pull_sample(t->appsink, 0);
    if (!gsample) return Bambu_would_block;

    GstBuffer* buf = gst_sample_get_buffer(gsample);
    if (!buf) {
        gst_sample_unref(gsample);
        return Bambu_would_block;
    }
    GstMapInfo info;
    if (!gst_buffer_map(buf, &info, GST_MAP_READ)) {
        gst_sample_unref(gsample);
        set_last_error("gst_buffer_map failed");
        return -1;
    }

    t->frame_buf.assign(info.data, info.data + info.size);
    gst_buffer_unmap(buf, &info);

    // Deliberately ignore GstBuffer's own DTS/PTS here. They are
    // running-time coordinates of *our* rtspsrc pipeline, which
    // includes the 200 ms jitter latency and any backpressure caused
    // by Studio's decoder. Studio runs its own pipeline with its own
    // clock, and gstbambusrc re-baselines to the first DTS we hand it
    // -- so if we forwarded those "pipeline-past" timestamps, Studio's
    // sink would keep trying to replay at that slower cadence,
    // producing the "video plays at half speed" symptom.
    //
    // Instead, stamp each frame with the wall-clock moment we pulled
    // it out of the appsink. That makes gstbambusrc say "this frame is
    // now", Studio's decoder renders it immediately, and the end-to-end
    // frame rate tracks whatever the camera is actually producing.
    auto now = std::chrono::steady_clock::now();
    auto ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(
                   now - t->t0).count();
    unsigned long long dt_100ns =
        static_cast<unsigned long long>(ns / 100);

    // Video dimensions, picked off the first sample's caps if possible.
    if (t->frame_count == 0) {
        GstCaps* caps = gst_sample_get_caps(gsample);
        if (caps) {
            GstStructure* s = gst_caps_get_structure(caps, 0);
            gint w = 0, h = 0;
            if (s && gst_structure_get_int(s, "width", &w) &&
                    gst_structure_get_int(s, "height", &h) &&
                    w > 0 && h > 0) {
                t->width  = w;
                t->height = h;
            }
        }
    }

    gst_sample_unref(gsample);

    if (++t->frame_count == 1 || (t->frame_count % 60) == 0) {
        log_fmt(t->logger, t->log_ctx,
                "read_rtsp: frame #%llu size=%zu",
                static_cast<unsigned long long>(t->frame_count),
                t->frame_buf.size());
    }

    sample->itrack      = 0;
    sample->size        = static_cast<int>(t->frame_buf.size());
    sample->flags       = 0;
    sample->buffer      = t->frame_buf.data();
    sample->decode_time = dt_100ns;
    return Bambu_success;
}

// -----------------------------------------------------------------------
// Build the 80-byte auth packet per OpenBambuAPI/video.md.
// -----------------------------------------------------------------------
void build_auth_packet(const TunnelUrl& url, uint8_t out[80])
{
    std::memset(out, 0, 80);
    auto put_u32_le = [&](size_t off, uint32_t v) {
        out[off + 0] = static_cast<uint8_t>( v        & 0xff);
        out[off + 1] = static_cast<uint8_t>((v >> 8)  & 0xff);
        out[off + 2] = static_cast<uint8_t>((v >> 16) & 0xff);
        out[off + 3] = static_cast<uint8_t>((v >> 24) & 0xff);
    };
    put_u32_le(0,  0x40);       // payload size (always 0x40 for auth)
    put_u32_le(4,  0x3000);     // packet type (auth)
    put_u32_le(8,  0);          // flags
    put_u32_le(12, 0);
    // Username / password into 32-byte fixed-size fields, NUL-padded.
    std::memcpy(out + 16, url.user.data(),
                std::min<size_t>(url.user.size(), 32));
    std::memcpy(out + 48, url.passwd.data(),
                std::min<size_t>(url.passwd.size(), 32));
}

// ---------------------------------------------------------------------
// Minimal ZIP reader.
//
// .3mf files are PKZip archives; to give Studio's MediaFilePanel a
// thumbnail we extract `Metadata/plate_1.png` out of them on the fly.
// We only handle:
//   * Uncompressed (method 0) entries,
//   * DEFLATE (method 8) via zlib's raw `inflate`,
// with a "use the central directory, not the local file headers"
// strategy (local headers can have zero size fields when bit-3 of GPBF
// is set). We read the entire .3mf into memory first - typical 3mf
// sizes are <50 MB for a printed plate, well within tolerable RAM.
// --------------------------------------------------------------------
struct ZipEntry {
    std::string  name;
    std::uint64_t comp_size = 0;
    std::uint64_t uncomp_size = 0;
    std::uint64_t local_offset = 0;
    std::uint16_t method = 0;
};

// Parses the central directory of a ZIP blob. Returns false if the
// EOCD record isn't found in the last 64 KB (standard search window).
bool zip_read_central(const std::vector<std::uint8_t>& zip,
                      std::vector<ZipEntry>*           out)
{
    out->clear();
    if (zip.size() < 22) return false;
    std::size_t max = std::min<std::size_t>(zip.size() - 22, 65535);
    std::size_t eocd = std::string::npos;
    for (std::size_t off = 0; off <= max; ++off) {
        std::size_t i = zip.size() - 22 - off;
        if (zip[i] == 'P' && zip[i+1] == 'K' && zip[i+2] == 5 && zip[i+3] == 6) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) return false;
    auto u16 = [&](std::size_t i) -> std::uint32_t {
        return zip[i] | (static_cast<std::uint32_t>(zip[i+1]) << 8);
    };
    auto u32 = [&](std::size_t i) -> std::uint32_t {
        return zip[i] | (static_cast<std::uint32_t>(zip[i+1]) << 8)
             | (static_cast<std::uint32_t>(zip[i+2]) << 16)
             | (static_cast<std::uint32_t>(zip[i+3]) << 24);
    };
    std::uint32_t cd_size   = u32(eocd + 12);
    std::uint32_t cd_offset = u32(eocd + 16);
    std::uint32_t entries   = u16(eocd + 10);
    if (cd_offset + cd_size > zip.size()) return false;

    std::size_t i = cd_offset;
    for (std::uint32_t n = 0; n < entries; ++n) {
        if (i + 46 > zip.size()) return false;
        if (!(zip[i] == 'P' && zip[i+1] == 'K' && zip[i+2] == 1 && zip[i+3] == 2))
            return false;
        ZipEntry e;
        e.method       = u16(i + 10);
        e.comp_size    = u32(i + 20);
        e.uncomp_size  = u32(i + 24);
        std::uint16_t name_len  = u16(i + 28);
        std::uint16_t extra_len = u16(i + 30);
        std::uint16_t cmt_len   = u16(i + 32);
        e.local_offset = u32(i + 42);
        if (i + 46 + name_len > zip.size()) return false;
        e.name.assign(reinterpret_cast<const char*>(&zip[i + 46]), name_len);
        out->push_back(std::move(e));
        i += 46 + name_len + extra_len + cmt_len;
    }
    return true;
}

// Extracts one entry into `out`. `method=0` is raw copy, `method=8` is
// zlib RAW deflate (no zlib header).
bool zip_extract(const std::vector<std::uint8_t>& zip, const ZipEntry& e,
                 std::vector<std::uint8_t>* out)
{
    out->clear();
    if (e.local_offset + 30 > zip.size()) return false;
    auto u16 = [&](std::size_t i) -> std::uint32_t {
        return zip[i] | (static_cast<std::uint32_t>(zip[i+1]) << 8);
    };
    std::uint16_t name_len  = u16(e.local_offset + 26);
    std::uint16_t extra_len = u16(e.local_offset + 28);
    std::size_t data_off = e.local_offset + 30 + name_len + extra_len;
    if (data_off + e.comp_size > zip.size()) return false;

    if (e.method == 0) {
        out->assign(zip.begin() + data_off,
                    zip.begin() + data_off + e.comp_size);
        return true;
    }
    if (e.method != 8) return false;

    z_stream zs{};
    // -MAX_WBITS = raw deflate (no zlib/gzip header/trailer).
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
    zs.next_in  = const_cast<Bytef*>(&zip[data_off]);
    zs.avail_in = static_cast<uInt>(e.comp_size);
    out->resize(e.uncomp_size ? e.uncomp_size : e.comp_size * 4);
    std::size_t produced = 0;
    while (true) {
        if (produced >= out->size()) out->resize(out->size() * 2);
        zs.next_out  = out->data() + produced;
        zs.avail_out = static_cast<uInt>(out->size() - produced);
        int rc = inflate(&zs, Z_NO_FLUSH);
        produced = out->size() - zs.avail_out;
        if (rc == Z_STREAM_END) break;
        if (rc == Z_BUF_ERROR && zs.avail_in == 0) break;
        if (rc != Z_OK) { inflateEnd(&zs); return false; }
    }
    inflateEnd(&zs);
    out->resize(produced);
    return true;
}

// Looks up one named entry in the central directory. Returns nullptr
// if no match.
const ZipEntry* zip_find(const std::vector<ZipEntry>& dir, const std::string& name)
{
    for (const auto& e : dir) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

// ImageGrid.cpp renders each tile's thumbnail via
//     dc.SetUserScale(content_w / img_w, content_h / img_h)
// i.e. two independent scale factors, so any thumbnail whose aspect
// doesn't already match the tile comes out visibly squashed/stretched.
// The images we serve (3mf plate previews and timelapse sidecars) are
// mostly square, whereas the tiles are 4:3-ish (models) or ~16:9
// (timelapse/video), so the stock UI looks wrong without intervention.
//
// We pre-letterbox at the plugin side: decode the image, paste it on
// a transparent (PNG) canvas with the tile aspect, re-encode as PNG.
// Studio then ends up with hs==vs, the content stays undistorted, and
// the padding is invisible.
//
// Decoder accepts PNG or JPEG (libpng + libjpeg-turbo). Output is
// always PNG so we only need one encoder and we get a clean alpha
// channel for the padding. Caller updates its `mime` accordingly.
//
// Never fails destructively: any decode/encode error just returns the
// original bytes unchanged and the caller keeps its original mime.

struct DecodedRGBA {
    std::vector<std::uint8_t> pixels;
    std::uint32_t             w = 0;
    std::uint32_t             h = 0;
};

bool decode_png_rgba(const std::vector<std::uint8_t>& in, DecodedRGBA* out)
{
    png_image img{};
    img.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_memory(&img, in.data(), in.size()) == 0) {
        png_image_free(&img);
        return false;
    }
    img.format = PNG_FORMAT_RGBA;
    if (img.width == 0 || img.height == 0 ||
        img.width > 8192 || img.height > 8192) {
        png_image_free(&img);
        return false;
    }
    out->w = img.width;
    out->h = img.height;
    out->pixels.assign(PNG_IMAGE_SIZE(img), 0);
    if (png_image_finish_read(&img, nullptr, out->pixels.data(), 0, nullptr) == 0) {
        png_image_free(&img);
        out->pixels.clear();
        return false;
    }
    png_image_free(&img);
    return true;
}

// libjpeg error handler that longjmps out instead of calling exit().
struct JpegErr { jpeg_error_mgr mgr; jmp_buf env; };
void jpeg_error_exit_throw(j_common_ptr cinfo)
{
    auto* err = reinterpret_cast<JpegErr*>(cinfo->err);
    longjmp(err->env, 1);
}

bool decode_jpeg_rgba(const std::vector<std::uint8_t>& in, DecodedRGBA* out)
{
    jpeg_decompress_struct cinfo{};
    JpegErr err{};
    cinfo.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jpeg_error_exit_throw;
    if (setjmp(err.env)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, const_cast<unsigned char*>(in.data()), in.size());
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    cinfo.out_color_space = JCS_RGB;
    if (!jpeg_start_decompress(&cinfo)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    const std::uint32_t w = cinfo.output_width;
    const std::uint32_t h = cinfo.output_height;
    if (w == 0 || h == 0 || w > 8192 || h > 8192 ||
        cinfo.output_components != 3) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    std::vector<std::uint8_t> row(static_cast<std::size_t>(w) * 3);
    out->w = w;
    out->h = h;
    out->pixels.assign(static_cast<std::size_t>(w) * h * 4, 0);
    while (cinfo.output_scanline < h) {
        JSAMPROW rowptr = row.data();
        if (jpeg_read_scanlines(&cinfo, &rowptr, 1) != 1) {
            jpeg_destroy_decompress(&cinfo);
            out->pixels.clear();
            return false;
        }
        std::size_t y = cinfo.output_scanline - 1;
        std::uint8_t* dst = out->pixels.data() + y * w * 4;
        for (std::uint32_t x = 0; x < w; ++x) {
            dst[x * 4 + 0] = row[x * 3 + 0];
            dst[x * 4 + 1] = row[x * 3 + 1];
            dst[x * 4 + 2] = row[x * 3 + 2];
            dst[x * 4 + 3] = 255;
        }
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

bool decode_image_rgba(const std::vector<std::uint8_t>& in, DecodedRGBA* out)
{
    if (in.size() < 4) return false;
    // PNG: 89 50 4E 47 ...
    if (in[0] == 0x89 && in[1] == 0x50 && in[2] == 0x4E && in[3] == 0x47) {
        return decode_png_rgba(in, out);
    }
    // JPEG: FF D8 FF ...
    if (in[0] == 0xFF && in[1] == 0xD8 && in[2] == 0xFF) {
        return decode_jpeg_rgba(in, out);
    }
    return false;
}

bool encode_png_rgba(const DecodedRGBA& src, std::vector<std::uint8_t>* out)
{
    png_image dst{};
    dst.version = PNG_IMAGE_VERSION;
    dst.width   = src.w;
    dst.height  = src.h;
    dst.format  = PNG_FORMAT_RGBA;
    png_alloc_size_t mem_bytes = 0;
    if (png_image_write_to_memory(&dst, nullptr, &mem_bytes, 0,
                                  src.pixels.data(), 0, nullptr) == 0 ||
        mem_bytes == 0) {
        return false;
    }
    out->assign(mem_bytes, 0);
    if (png_image_write_to_memory(&dst, out->data(), &mem_bytes, 0,
                                  src.pixels.data(), 0, nullptr) == 0) {
        out->clear();
        return false;
    }
    out->resize(mem_bytes);
    return true;
}

// Fit modes for reshape_image_to_aspect:
//
//   Pad     - add transparent bars so the source content is fully
//             visible. Good when the source is already "cropped" to
//             something meaningful and losing edges would be bad
//             (e.g. the plate render inside a .3mf already has its
//             own framing around the model).
//   Crop    - center-crop the minor axis so the result fills the
//             tile edge-to-edge. Good when the source has natural
//             margin that can be sacrificed. Loses content on the
//             cropped axis but avoids visible bars.
//   Stretch - bilinearly resample along the minor axis so the
//             result fills the tile edge-to-edge with no padding
//             and no content loss. This is the right choice when
//             the source is already anamorphically compressed by
//             the producer - e.g. P2S timelapse sidecars, which are
//             a 16:9 camera frame squashed into a 480x480 JPEG on
//             the printer's FTPS storage. Re-stretching restores
//             the original geometry.
enum class FitMode { Pad, Crop, Stretch };

// Reshape `in` (PNG or JPEG) to `target_aspect` and re-encode as PNG.
// On any failure - unknown format, decode error, aspect already close
// enough - returns the original bytes and leaves `*mime` alone. On
// success, writes the new PNG bytes and sets `*mime` to "image/png".
std::vector<std::uint8_t>
reshape_image_to_aspect(const std::vector<std::uint8_t>& in,
                        double target_aspect, FitMode mode,
                        std::string* mime)
{
    if (in.empty() || !(target_aspect > 0.0)) return in;

    DecodedRGBA img;
    if (!decode_image_rgba(in, &img)) return in;

    const double src_aspect = static_cast<double>(img.w) /
                              static_cast<double>(img.h);
    // Within ~1% of target: not worth re-encoding.
    if (std::fabs(src_aspect - target_aspect) / target_aspect < 0.01) return in;

    DecodedRGBA canvas;
    if (mode == FitMode::Pad) {
        // Add transparent bars on the minor axis.
        std::uint32_t nw = img.w;
        std::uint32_t nh = img.h;
        std::uint32_t ox = 0;
        std::uint32_t oy = 0;
        if (src_aspect < target_aspect) {
            nw = static_cast<std::uint32_t>(std::llround(img.h * target_aspect));
            if (nw <= img.w) return in;
            ox = (nw - img.w) / 2;
        } else {
            nh = static_cast<std::uint32_t>(std::llround(img.w / target_aspect));
            if (nh <= img.h) return in;
            oy = (nh - img.h) / 2;
        }

        canvas.w = nw;
        canvas.h = nh;
        canvas.pixels.assign(static_cast<std::size_t>(nw) * nh * 4, 0);
        for (std::uint32_t y = 0; y < img.h; ++y) {
            std::memcpy(canvas.pixels.data() +
                        ((static_cast<std::size_t>(oy + y) * nw) + ox) * 4,
                        img.pixels.data() + static_cast<std::size_t>(y) * img.w * 4,
                        static_cast<std::size_t>(img.w) * 4);
        }
    } else if (mode == FitMode::Crop) {
        // Center-crop the minor axis.
        std::uint32_t nw = img.w;
        std::uint32_t nh = img.h;
        std::uint32_t sx = 0;
        std::uint32_t sy = 0;
        if (src_aspect < target_aspect) {
            // Too tall: crop top/bottom.
            nh = static_cast<std::uint32_t>(std::llround(img.w / target_aspect));
            if (nh == 0 || nh >= img.h) return in;
            sy = (img.h - nh) / 2;
        } else {
            // Too wide: crop sides.
            nw = static_cast<std::uint32_t>(std::llround(img.h * target_aspect));
            if (nw == 0 || nw >= img.w) return in;
            sx = (img.w - nw) / 2;
        }

        canvas.w = nw;
        canvas.h = nh;
        canvas.pixels.assign(static_cast<std::size_t>(nw) * nh * 4, 0);
        for (std::uint32_t y = 0; y < nh; ++y) {
            std::memcpy(canvas.pixels.data() +
                        static_cast<std::size_t>(y) * nw * 4,
                        img.pixels.data() +
                        ((static_cast<std::size_t>(sy + y) * img.w) + sx) * 4,
                        static_cast<std::size_t>(nw) * 4);
        }
    } else {
        // Stretch: bilinearly resample the minor axis so the result
        // hits target_aspect with no padding or cropping. For P2S
        // timelapse sidecars this undoes the 16:9 -> 1:1 squashing
        // the printer firmware applies before writing the JPEG to
        // /timelapse/thumbnail/*.jpg.
        std::uint32_t nw = img.w;
        std::uint32_t nh = img.h;
        if (src_aspect < target_aspect) {
            nw = static_cast<std::uint32_t>(std::llround(img.h * target_aspect));
        } else {
            nh = static_cast<std::uint32_t>(std::llround(img.w / target_aspect));
        }
        if (nw == 0 || nh == 0) return in;

        canvas.w = nw;
        canvas.h = nh;
        canvas.pixels.assign(static_cast<std::size_t>(nw) * nh * 4, 0);

        const double x_ratio = nw > 1
            ? static_cast<double>(img.w - 1) / (nw - 1) : 0.0;
        const double y_ratio = nh > 1
            ? static_cast<double>(img.h - 1) / (nh - 1) : 0.0;
        for (std::uint32_t y = 0; y < nh; ++y) {
            const double sy = y * y_ratio;
            const std::uint32_t y0 = static_cast<std::uint32_t>(sy);
            const std::uint32_t y1 = std::min(y0 + 1, img.h - 1);
            const double fy = sy - y0;
            for (std::uint32_t x = 0; x < nw; ++x) {
                const double sx = x * x_ratio;
                const std::uint32_t x0 = static_cast<std::uint32_t>(sx);
                const std::uint32_t x1 = std::min(x0 + 1, img.w - 1);
                const double fx = sx - x0;
                const auto* p00 = img.pixels.data() + (static_cast<std::size_t>(y0) * img.w + x0) * 4;
                const auto* p01 = img.pixels.data() + (static_cast<std::size_t>(y0) * img.w + x1) * 4;
                const auto* p10 = img.pixels.data() + (static_cast<std::size_t>(y1) * img.w + x0) * 4;
                const auto* p11 = img.pixels.data() + (static_cast<std::size_t>(y1) * img.w + x1) * 4;
                auto* dst = canvas.pixels.data() + (static_cast<std::size_t>(y) * nw + x) * 4;
                for (int c = 0; c < 4; ++c) {
                    const double top    = p00[c] + (p01[c] - p00[c]) * fx;
                    const double bottom = p10[c] + (p11[c] - p10[c]) * fx;
                    const double v      = top + (bottom - top) * fy;
                    dst[c] = static_cast<std::uint8_t>(std::lround(
                        std::max(0.0, std::min(255.0, v))));
                }
            }
        }
    }

    std::vector<std::uint8_t> out;
    if (!encode_png_rgba(canvas, &out)) return in;
    if (mime) *mime = "image/png";
    return out;
}

// Target aspects for the tile types ImageGrid renders. Derived from
// ImageGrid.cpp::UpdateFileSystem / renderContent2:
//
//   F_MODEL:     content 266 x (264 - 64) = 266 x 200      -> 1.33:1 (4:3)
//   F_TIMELAPSE: content 384 x 216                          -> 1.78:1 (16:9)
//   F_VIDEO:     same as timelapse
//
// (Group mode uses 480x270 which is also 16:9, so one constant covers
// both list and group views.)
constexpr double kModelTileAspect     = 266.0 / 200.0;
constexpr double kTimelapseTileAspect = 384.0 / 216.0;

// --------------------------------------------------------------------
// CTRL helpers: JSON assembly, FTPS connect, path handling.
// --------------------------------------------------------------------

// Builds the on-wire reply bytes Studio expects. `reply_json` is the
// `{cmdtype, sequence, result, reply}` envelope as a Value. Optional
// blob is appended after "\n\n".
std::string make_wire_reply(const obn::json::Value& env,
                            const std::uint8_t*     blob,
                            std::size_t             blob_len)
{
    std::string s = env.dump();
    if (blob_len > 0) {
        s += "\n\n";
        s.append(reinterpret_cast<const char*>(blob), blob_len);
    }
    return s;
}

obn::json::Value make_reply_envelope(int cmdtype, int sequence, int result,
                                     obn::json::Value reply)
{
    obn::json::Object o;
    o["cmdtype"]  = obn::json::Value(static_cast<double>(cmdtype));
    o["sequence"] = obn::json::Value(static_cast<double>(sequence));
    o["result"]   = obn::json::Value(static_cast<double>(result));
    o["reply"]    = std::move(reply);
    return obn::json::Value(std::move(o));
}

// Parses the inbound "<json>\n\n<blob>" wire format Studio sends.
bool parse_ctrl_request(const std::string& wire, int* cmdtype, int* sequence,
                        obn::json::Value* req)
{
    // Body is either just JSON, or JSON + "\n\n" + blob. Blobs on the
    // request side only appear for FILE_UPLOAD (not implemented).
    std::size_t j_end = wire.find("\n\n");
    std::string j = (j_end == std::string::npos) ? wire : wire.substr(0, j_end);
    std::string perr;
    auto v = obn::json::parse(j, &perr);
    if (!v) return false;
    auto ct = v->find("cmdtype");
    auto sq = v->find("sequence");
    if (!ct.is_number() || !sq.is_number()) return false;
    *cmdtype  = static_cast<int>(ct.as_number());
    *sequence = static_cast<int>(sq.as_number());
    *req      = v->find("req");
    return true;
}

// Connects a fresh FTPS client on demand (the first CTRL request).
// Kept on the tunnel so multiple requests reuse the control channel.
std::string ensure_ftp(Tunnel* t)
{
    if (t->ftp) return {};
    t->ftp = std::make_unique<obn::ftps::Client>();
    obn::ftps::ConnectConfig cfg;
    cfg.host     = t->url.host;
    cfg.port     = 990;
    cfg.username = t->url.user.empty() ? "bblp" : t->url.user;
    cfg.password = t->url.passwd;
    log_fmt(t->logger, t->log_ctx,
            "ctrl: FTPS connect host=%s user=%s", cfg.host.c_str(), cfg.username.c_str());
    std::string err = t->ftp->connect(cfg);
    if (!err.empty()) {
        log_fmt(t->logger, t->log_ctx,
                "ctrl: FTPS connect failed: %s", err.c_str());
        t->ftp.reset();
        return err;
    }

    // Probe storage roots to figure out our prefix. A Bambu P1/X1 with
    // an SD card exposes `/sdcard`; newer P2S/A-series with a USB stick
    // exposes `/usb`. Some firmwares expose both; some P2S units expose
    // neither because the root IS the storage mount.
    if (t->ftp->cwd("/sdcard").empty()) {
        t->storage_label  = "sdcard";
        t->ftp_prefix     = "/sdcard";
        t->root_is_storage = false;
    } else if (t->ftp->cwd("/usb").empty()) {
        t->storage_label  = "usb";
        t->ftp_prefix     = "/usb";
        t->root_is_storage = false;
    } else if (t->ftp->cwd("/").empty()) {
        // P2S-style: root IS the storage. Label it "sdcard" because
        // Studio's MediaFilePanel doesn't care but its storage dropdown
        // displays whatever string we hand it.
        t->storage_label  = "sdcard";
        t->ftp_prefix     = "";
        t->root_is_storage = true;
    } else {
        log_fmt(t->logger, t->log_ctx,
                "ctrl: no accessible storage mount (neither /sdcard, /usb, nor /)");
        return "no storage mount";
    }
    log_fmt(t->logger, t->log_ctx,
            "ctrl: storage=%s prefix='%s' root_is_storage=%d",
            t->storage_label.c_str(), t->ftp_prefix.c_str(),
            t->root_is_storage ? 1 : 0);
    return {};
}

// Subtree on the printer where Studio's `type` argument routes.
// Studio asks for { "timelapse" | "video" | "model" }:
//   * timelapse -> <prefix>/timelapse/ (mp4 + matching .jpg thumb)
//   * video     -> <prefix>/ipcam/     (.mp4 manual recordings; may be empty)
//   * model     -> <prefix>/           (.gcode.3mf at the top level)
// For P2S where prefix is "", we drop the leading slash off of
// subpaths so FTPS doesn't resolve to the drive's root twice.
std::string resolve_subtree(const Tunnel* t, const std::string& type)
{
    if (type == "timelapse") return t->ftp_prefix + "/timelapse";
    if (type == "video")     return t->ftp_prefix + "/ipcam";
    // "model" and anything else we don't recognise map to the root.
    return t->ftp_prefix.empty() ? "/" : t->ftp_prefix;
}

// Chooses what extensions a listing should surface to Studio for a
// given file-type argument. Studio filters client-side too (by file
// extension in FileObject::format), but filtering here means we don't
// send 40 KB of irrelevant names over the CTRL channel.
bool keep_for_type(const std::string& type, const std::string& name)
{
    auto ends_with = [&](const char* suf) {
        std::size_t n = std::strlen(suf);
        return name.size() >= n &&
               std::equal(name.end() - n, name.end(), suf,
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    };
    if (type == "timelapse") return ends_with(".mp4") || ends_with(".avi");
    if (type == "video")     return ends_with(".mp4");
    if (type == "model")     return ends_with(".3mf") || ends_with(".gcode")
                                 || ends_with(".gcode.3mf");
    return true;
}

// Time formatter Studio expects in LIST_INFO entries: "YYYY-MM-DD HH:MM:SS".
std::string format_time_studio(std::uint64_t epoch)
{
    if (epoch == 0) return "";
    std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%F %T", &tm);
    return buf;
}

// --------------------------------------------------------------------
// CTRL command handlers. Each one is synchronous and invoked from the
// worker thread context; they push `CtrlReply`s onto `t->ctrl_out`
// under `t->ctrl_mu`.
// --------------------------------------------------------------------

void push_reply(Tunnel* t, CtrlReply r)
{
    std::lock_guard<std::mutex> lk(t->ctrl_mu);
    t->ctrl_out.emplace_back(std::move(r));
    t->ctrl_cv.notify_all();
}

bool is_cancelled(Tunnel* t, int sequence)
{
    std::lock_guard<std::mutex> lk(t->ctrl_mu);
    return t->ctrl_cancelled.count(sequence) > 0;
}

void handle_media_ability(Tunnel* t, int sequence,
                          const obn::json::Value& /*req*/)
{
    std::string err = ensure_ftp(t);
    obn::json::Object reply;
    obn::json::Array  storage;
    if (err.empty()) {
        storage.emplace_back(t->storage_label);
    }
    reply["storage"] = obn::json::Value(std::move(storage));
    // Studio's MediaFilePanel is satisfied with just storage; some
    // firmwares also advertise `ability.file_list` etc. - we can add
    // them later once something in Studio starts consulting them.
    auto env = make_reply_envelope(kCmdRequestMediaAbility, sequence,
                                   err.empty() ? kResOK : kResStorUnavail,
                                   obn::json::Value(std::move(reply)));
    push_reply(t, {make_wire_reply(env, nullptr, 0)});
}

void handle_list_info(Tunnel* t, int sequence, const obn::json::Value& req)
{
    std::string type = req.find("type").as_string();
    // Storage routing: Studio's dropdown uses whatever labels we sent
    // in REQUEST_MEDIA_ABILITY, so we can safely ignore req.storage.
    std::string err = ensure_ftp(t);
    if (!err.empty()) {
        auto env = make_reply_envelope(kCmdListInfo, sequence,
                                       kResStorUnavail,
                                       obn::json::Value(obn::json::Object{}));
        push_reply(t, {make_wire_reply(env, nullptr, 0)});
        return;
    }
    std::string path = resolve_subtree(t, type);
    std::vector<obn::ftps::Entry> entries;
    err = t->ftp->list_entries(path, &entries);
    if (!err.empty()) {
        // Directory missing on this printer (e.g. no timelapses yet).
        // Return an empty list rather than an error - Studio treats
        // that as "nothing to show".
        log_fmt(t->logger, t->log_ctx,
                "ctrl: LIST_INFO type=%s path=%s: %s (returning empty)",
                type.c_str(), path.c_str(), err.c_str());
        obn::json::Object reply;
        reply["file_lists"] = obn::json::Value(obn::json::Array{});
        auto env = make_reply_envelope(kCmdListInfo, sequence, kResOK,
                                       obn::json::Value(std::move(reply)));
        push_reply(t, {make_wire_reply(env, nullptr, 0)});
        return;
    }

    obn::json::Array files;
    for (const auto& e : entries) {
        if (e.is_dir) continue;
        if (!keep_for_type(type, e.name)) continue;
        obn::json::Object f;
        f["name"] = obn::json::Value(e.name);
        // `path` on the Studio side is the FTPS absolute path we'll
        // receive back in FILE_DOWNLOAD / SUB_FILE. Keep it fully
        // qualified for that reason.
        //
        // On printers where the FTPS root IS the storage volume
        // (P2S/USB), resolve_subtree("model") returns "/", so a naive
        // concat would give "//name". vsftpd tolerates that, but
        // Studio does its own path string compares against the
        // request paths it sent, so it's cleaner to emit just a
        // single leading slash.
        std::string full = (path == "/") ? ("/" + e.name)
                                         : (path + "/" + e.name);
        f["path"] = obn::json::Value(full);
        f["size"] = obn::json::Value(static_cast<double>(e.size));
        if (e.mtime != 0) {
            f["time"] = obn::json::Value(static_cast<double>(e.mtime));
            f["date"] = obn::json::Value(format_time_studio(e.mtime));
        }
        files.emplace_back(obn::json::Value(std::move(f)));
    }
    obn::json::Object reply;
    reply["file_lists"] = obn::json::Value(std::move(files));
    auto env = make_reply_envelope(kCmdListInfo, sequence, kResOK,
                                   obn::json::Value(std::move(reply)));
    push_reply(t, {make_wire_reply(env, nullptr, 0)});
}

// Downloads a file over FTPS into memory (for SUB_FILE thumbnails or
// small 3mf previews). Caller decides if it's worth doing based on
// size - we cap at 64 MB to avoid OOM on a pathological input.
std::string download_blob(Tunnel* t, const std::string& full_path,
                          std::vector<std::uint8_t>* out)
{
    out->clear();
    constexpr std::size_t kMax = 64u * 1024u * 1024u;
    return t->ftp->retr(full_path, [out](const void* data, std::size_t len) {
        if (out->size() + len > kMax) return false;
        out->insert(out->end(),
                    static_cast<const std::uint8_t*>(data),
                    static_cast<const std::uint8_t*>(data) + len);
        return true;
    });
}

// SUB_FILE covers several unrelated Studio operations; the only
// discriminator is the shape of the `req` JSON:
//
//   * req.files: ["name1", "name2"] -- legacy thumbnail fetch. Each
//     entry is served as a separate reply chunk, result=CONTINUE
//     until the last, which gets result=SUCCESS.
//
//   * req.paths: ["path#thumbnail", "path#_rels/.rels", ...] -- modern
//     thumbnail / metadata fetch. The part after '#' names either
//     "thumbnail" (meaning the sidecar jpg for timelapses, or
//     Metadata/plate_1.png inside the .3mf) or a literal entry path
//     inside a zip archive.
//
//   * req.zip=true -- caller wants the whole archive in one blob
//     instead of per-file chunks. Used by the fetch-model path.
void handle_sub_file(Tunnel* t, int sequence, const obn::json::Value& req)
{
    std::string err = ensure_ftp(t);
    if (!err.empty()) {
        auto env = make_reply_envelope(kCmdSubFile, sequence, kResStorUnavail,
                                       obn::json::Value(obn::json::Object{}));
        push_reply(t, {make_wire_reply(env, nullptr, 0)});
        return;
    }

    // Keep the owning Values alive for the entire handler - as_array()
    // returns a reference into the Value, so we can't chain it off a
    // temporary.
    obn::json::Value paths_v = req.find("paths");
    obn::json::Value files_v = req.find("files");
    const auto& paths = paths_v.as_array();
    const auto& files = files_v.as_array();
    bool zip_mode = req.find("zip").as_bool(false);

    {
        std::string first_path;
        if (!paths.empty())      first_path = paths[0].as_string();
        else if (!files.empty()) first_path = files[0].as_string();
        log_fmt(t->logger, t->log_ctx,
                "ctrl: SUB_FILE seq=%d zip=%d paths=%zu files=%zu first=%s",
                sequence, zip_mode ? 1 : 0, paths.size(), files.size(),
                first_path.c_str());
    }

    // Helper that pushes one blob-bearing reply.
    auto push_chunk = [&](int result, const obn::json::Object& extra,
                          const std::uint8_t* blob, std::size_t blen) {
        obn::json::Object r = extra;
        auto env = make_reply_envelope(kCmdSubFile, sequence, result,
                                       obn::json::Value(std::move(r)));
        // Dump envelope JSON for every chunk so we can see exactly
        // what Studio parses. Blob bytes are omitted for brevity.
        if (const char* dbg = std::getenv("OBN_DEBUG_SUBFILE")) {
            if (dbg && dbg[0] == '1') {
                log_fmt(t->logger, t->log_ctx,
                        "ctrl: SUB_FILE reply seq=%d blen=%zu env=%s",
                        sequence, blen, env.dump().c_str());
            }
        }
        push_reply(t, {make_wire_reply(env, blob, blen)});
    };

    if (!paths.empty() && zip_mode) {
        // zip=true means Studio wants to receive a real ZIP archive it
        // will parse itself (FetchModel -> load_gcode_3mf_from_stream).
        // The `paths` list enumerates the individual entries it'll
        // later read out of that zip (_rels/.rels, 3D/3dmodel.model,
        // Metadata/*, plate_thumbnail_*), all under one base file. It
        // is NOT asking us to synthesize a new archive - handing back
        // the source .3mf bytes unchanged is the simplest thing that
        // satisfies Studio's parser.
        //
        // Studio's FetchModel callback just keeps concatenating chunk
        // bytes into one string, so we can stream the archive in 256-
        // KB slices directly off FTPS without pre-buffering the whole
        // thing in memory.
        std::vector<std::string> bases;
        bases.reserve(paths.size());
        for (std::size_t i = 0; i < paths.size(); ++i) {
            std::string full = paths[i].as_string();
            auto hash = full.find('#');
            if (hash != std::string::npos) full = full.substr(0, hash);
            if (std::find(bases.begin(), bases.end(), full) == bases.end())
                bases.push_back(full);
        }
        // Buffer the whole archive in memory and split it into chunks
        // only after the FTPS transfer finishes. This lets us mark the
        // LAST non-empty chunk of each base file with `continue=false`
        // reliably even when the file size happens to be an exact
        // multiple of our chunk boundary - which UpdateFocusThumbnail2
        // interprets as FILE_SIZE_ERR and drops the whole thumbnail.
        // Typical .3mf sizes are a few MB; buffering isn't an issue.
        constexpr std::size_t kChunkSize = 256 * 1024;
        for (std::size_t bi = 0; bi < bases.size(); ++bi) {
            const std::string& full = bases[bi];
            if (is_cancelled(t, sequence)) {
                push_chunk(kResErrCancel, {}, nullptr, 0);
                return;
            }
            std::uint64_t total_size = 0;
            t->ftp->size(full, &total_size);
            std::vector<std::uint8_t> archive;
            archive.reserve(static_cast<std::size_t>(total_size ? total_size : kChunkSize));
            bool cancelled = false;
            std::string err = t->ftp->retr(full, [&](const void* data, std::size_t len) {
                if (is_cancelled(t, sequence)) { cancelled = true; return false; }
                const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
                archive.insert(archive.end(), p, p + len);
                return true;
            });
            if (cancelled) {
                push_chunk(kResErrCancel, {}, nullptr, 0);
                return;
            }
            if (!err.empty()) {
                log_fmt(t->logger, t->log_ctx,
                        "ctrl: SUB_FILE(zip) %s failed: %s", full.c_str(), err.c_str());
                auto env = make_reply_envelope(kCmdSubFile, sequence, kResFileNoExist,
                                               obn::json::Value(obn::json::Object{}));
                push_reply(t, {make_wire_reply(env, nullptr, 0)});
                return;
            }

            const bool is_last_file = (bi + 1 == bases.size());
            const std::size_t total  = archive.size();
            log_fmt(t->logger, t->log_ctx,
                    "ctrl: SUB_FILE(zip) seq=%d %s bytes=%zu chunks=%zu last_file=%d",
                    sequence, full.c_str(), total,
                    (total + kChunkSize - 1) / kChunkSize, is_last_file ? 1 : 0);

            // Debug helper: dump assembled archive to /tmp so we can
            // verify it parses cleanly with an external unzipper.
            if (const char* dump = std::getenv("OBN_DUMP_SUBFILE_ZIP")) {
                if (dump && dump[0] == '1' && total > 0) {
                    std::string name = full;
                    for (auto& c : name) if (c == '/') c = '_';
                    std::string path = "/tmp/obn-" + name;
                    if (FILE* fp = std::fopen(path.c_str(), "wb")) {
                        std::fwrite(archive.data(), 1, total, fp);
                        std::fclose(fp);
                        log_fmt(t->logger, t->log_ctx,
                                "ctrl: SUB_FILE(zip) dumped %s (%zu bytes)",
                                path.c_str(), total);
                    }
                }
            }

            if (total == 0) {
                // Truly empty file - Studio's translator would emit
                // FILE_SIZE_ERR regardless of continue/size flags. Send
                // a terminal response to unstick the sequence.
                obn::json::Object extra;
                extra["path"]     = obn::json::Value(full);
                extra["mimetype"] = obn::json::Value("application/zip");
                extra["size"]     = obn::json::Value(static_cast<double>(0));
                extra["offset"]   = obn::json::Value(static_cast<double>(0));
                extra["total"]    = obn::json::Value(static_cast<double>(0));
                extra["continue"] = obn::json::Value(false);
                int result = is_last_file ? kResOK : kResContinue;
                push_chunk(result, extra, nullptr, 0);
                continue;
            }

            std::size_t offset = 0;
            while (offset < total) {
                std::size_t chunk = std::min(kChunkSize, total - offset);
                bool last_chunk = (offset + chunk == total);
                obn::json::Object extra;
                extra["path"]     = obn::json::Value(full);
                extra["mimetype"] = obn::json::Value("application/zip");
                extra["size"]     = obn::json::Value(static_cast<double>(chunk));
                extra["offset"]   = obn::json::Value(static_cast<double>(offset));
                extra["total"]    = obn::json::Value(static_cast<double>(total));
                // Studio's ModelMetadata translator accumulates slices
                // into iter->local_path as long as `continue` is true;
                // on the first `continue=false` it assembles the full
                // buffer and runs load_gcode_3mf_from_stream on it.
                // So we MUST set continue=true for every non-terminal
                // slice of every base file, including cross-file
                // boundaries when the SUB_FILE request covers several.
                extra["continue"] = obn::json::Value(!last_chunk);
                bool is_final = last_chunk && is_last_file;
                int result = is_final ? kResOK : kResContinue;
                push_chunk(result, extra, archive.data() + offset, chunk);
                offset += chunk;
            }
        }
        return;
    }

    if (!paths.empty()) {
        // Gather candidates first so we know when to send SUCCESS vs CONTINUE.
        std::size_t total = paths.size();
        for (std::size_t i = 0; i < total; ++i) {
            if (is_cancelled(t, sequence)) {
                push_chunk(kResErrCancel, {}, nullptr, 0);
                return;
            }
            std::string full = paths[i].as_string();
            std::string sub_entry;
            auto hash = full.find('#');
            if (hash != std::string::npos) {
                sub_entry = full.substr(hash + 1);
                full      = full.substr(0, hash);
            }

            std::vector<std::uint8_t> blob;
            std::string mime = "image/jpeg";
            std::string thumb_path;

            if (sub_entry == "thumbnail") {
                // Timelapse/video path: printer stores sidecar jpegs in
                // <dir>/thumbnail/<basename>.jpg (full, ~480x480) and
                // <basename>_mini.jpg (~240x194). Only the full one is
                // guaranteed to exist on older firmware; minis showed
                // up later. Full is still only ~15-20 KB so we always
                // fetch the full version and letterbox it to 16:9 for
                // the timelapse tile.
                //
                // Fall back to <dir>/<basename>.jpg and then to the 3mf
                // archive path (plate_*.png) so this keeps working on
                // printers that stash sidecars differently.
                auto ends_with = [&](const std::string& s, const char* suf) {
                    std::size_t ls = std::strlen(suf);
                    return s.size() >= ls &&
                           s.compare(s.size() - ls, ls, suf) == 0;
                };

                // Split full path into dir / base / stem.
                std::string dir_part, base_part, stem;
                auto slash = full.rfind('/');
                if (slash != std::string::npos) {
                    dir_part  = full.substr(0, slash);
                    base_part = full.substr(slash + 1);
                } else {
                    base_part = full;
                }
                stem = base_part;
                auto dot = stem.rfind('.');
                if (dot != std::string::npos) stem = stem.substr(0, dot);

                std::vector<std::string> candidates;
                if (ends_with(base_part, ".mp4")) {
                    // Official firmware location for timelapse/video
                    // thumbnails.
                    candidates.push_back(dir_part + "/thumbnail/" + stem + ".jpg");
                    candidates.push_back(dir_part + "/thumbnail/" + stem + "_mini.jpg");
                }
                // Legacy / generic sidecar next to the source file.
                candidates.push_back(dir_part.empty()
                                     ? ("/" + stem + ".jpg")
                                     : (dir_part + "/" + stem + ".jpg"));

                for (const auto& c : candidates) {
                    blob.clear();
                    if (download_blob(t, c, &blob).empty() && !blob.empty()) {
                        // Echo back the original request path so
                        // Studio's callback in PrinterFileSystem.cpp
                        // can match the reply to the right File: it
                        // does `path.substr(0, path.find('#'))` and
                        // compares to `f.path`. If we returned the
                        // real sidecar location ("/timelapse/thumbnail
                        // /foo.jpg") that match would fail silently
                        // and the tile would stay empty even though
                        // the bytes arrived fine.
                        thumb_path = full + "#thumbnail";
                        mime = "image/jpeg";
                        // P2S firmware stores timelapse sidecars as 480x480
                        // JPEGs - a 16:9 camera frame anamorphically squashed
                        // into a square. Stretch horizontally to restore the
                        // original geometry before handing it to Studio.
                        blob = reshape_image_to_aspect(blob, kTimelapseTileAspect,
                                                       FitMode::Stretch, &mime);
                        break;
                    }
                }

                if (blob.empty()) {
                    if (ends_with(full, ".3mf") || ends_with(full, ".gcode.3mf")) {
                        // .3mf path: read whole archive, extract plate_1.png.
                        std::vector<std::uint8_t> zipbuf;
                        if (download_blob(t, full, &zipbuf).empty()) {
                            std::vector<ZipEntry> dir;
                            if (zip_read_central(zipbuf, &dir)) {
                                const ZipEntry* e = zip_find(dir, "Metadata/plate_1.png");
                                if (!e) e = zip_find(dir, "Metadata/plate_no_light_1.png");
                                if (e && zip_extract(zipbuf, *e, &blob)) {
                                    mime = "image/png";
                                    thumb_path = full + "#" + e->name;
                                    blob = reshape_image_to_aspect(blob, kModelTileAspect,
                                                                   FitMode::Pad, &mime);
                                }
                            }
                        }
                    }
                    // Anything still without a thumbnail: reply size=0
                    // and let Studio fall back to its default icon.
                    if (blob.empty() && thumb_path.empty()) thumb_path = full;
                }
            } else if (!sub_entry.empty()) {
                // Arbitrary entry inside a .3mf ZIP (Metadata/plate_1.png,
                // Metadata/model_settings.config, etc.)
                std::vector<std::uint8_t> zipbuf;
                if (download_blob(t, full, &zipbuf).empty()) {
                    std::vector<ZipEntry> dir;
                    if (zip_read_central(zipbuf, &dir)) {
                        const ZipEntry* e = zip_find(dir, sub_entry);
                        if (e) zip_extract(zipbuf, *e, &blob);
                    }
                }
                // Studio feeds `mimetype` straight into wxImage() for
                // thumbnail entries, so it MUST be "image/<ext>" for PNG/
                // JPEG/etc. A generic octet-stream makes wxImage fail
                // silently and the tile stays blank.
                auto dot = sub_entry.rfind('.');
                if (dot != std::string::npos) {
                    std::string ext = sub_entry.substr(dot + 1);
                    for (auto& c : ext) c = std::tolower(static_cast<unsigned char>(c));
                    if (ext == "png" || ext == "jpg" || ext == "jpeg" ||
                        ext == "gif" || ext == "bmp" || ext == "webp") {
                        mime = "image/" + ext;
                    } else {
                        mime = "application/octet-stream";
                    }
                    // Letterbox the F_MODEL plate previews: Studio stretches
                    // them independently on X and Y to fill the tile, so
                    // native 512x512 plate_*.png comes out squashed.
                    if ((ext == "png" || ext == "jpg" || ext == "jpeg") &&
                        !blob.empty()) {
                        blob = reshape_image_to_aspect(blob, kModelTileAspect,
                                                       FitMode::Pad, &mime);
                    }
                } else {
                    mime = "application/octet-stream";
                }
                thumb_path = full + "#" + sub_entry;
            } else {
                // Raw file fetch, no zip extraction. Studio uses this for
                // small auxiliary files; we cap at 64 MB (download_blob).
                download_blob(t, full, &blob);
                mime = "application/octet-stream";
                thumb_path = full;
            }

            obn::json::Object extra;
            extra["path"]     = obn::json::Value(thumb_path);
            extra["mimetype"] = obn::json::Value(mime);
            extra["size"]     = obn::json::Value(static_cast<double>(blob.size()));
            extra["offset"]   = obn::json::Value(static_cast<double>(0));
            extra["total"]    = obn::json::Value(static_cast<double>(blob.size()));
            int result = (i + 1 == total) ? kResOK : kResContinue;
            if (blob.empty()) {
                // Studio uses `thumbnail` (base64 or raw) as the mimetype-
                // driven fallback when there's no blob; omit it and just
                // signal "no data" for this index.
                extra["size"] = obn::json::Value(static_cast<double>(0));
                push_chunk(result, extra, nullptr, 0);
            } else {
                push_chunk(result, extra, blob.data(), blob.size());
            }
        }
        return;
    }

    if (!files.empty()) {
        // Legacy per-name thumbnail: we map every entry to its sidecar
        // .jpg under the timelapse subtree and reply in order.
        std::string subtree = resolve_subtree(t, "timelapse");
        std::size_t total = files.size();
        for (std::size_t i = 0; i < total; ++i) {
            if (is_cancelled(t, sequence)) {
                push_chunk(kResErrCancel, {}, nullptr, 0);
                return;
            }
            std::string name = files[i].as_string();
            std::string stem = name;
            auto dot = stem.rfind('.');
            if (dot != std::string::npos) stem = stem.substr(0, dot);
            std::string sidecar = subtree + "/" + stem + ".jpg";
            std::vector<std::uint8_t> blob;
            download_blob(t, sidecar, &blob);
            obn::json::Object extra;
            extra["name"]     = obn::json::Value(name);
            extra["mimetype"] = obn::json::Value("image/jpeg");
            extra["size"]     = obn::json::Value(static_cast<double>(blob.size()));
            int result = (i + 1 == total) ? kResOK : kResContinue;
            push_chunk(result, extra, blob.data(), blob.size());
        }
        return;
    }

    push_chunk(kResApiUnsupport, {}, nullptr, 0);
}

void handle_file_download(Tunnel* t, int sequence, const obn::json::Value& req)
{
    std::string err = ensure_ftp(t);
    if (!err.empty()) {
        auto env = make_reply_envelope(kCmdFileDownload, sequence, kResStorUnavail,
                                       obn::json::Value(obn::json::Object{}));
        push_reply(t, {make_wire_reply(env, nullptr, 0)});
        return;
    }
    std::string path = req.find("path").as_string();
    if (path.empty()) path = req.find("file").as_string();
    if (path.empty()) {
        auto env = make_reply_envelope(kCmdFileDownload, sequence, kResFileNoExist,
                                       obn::json::Value(obn::json::Object{}));
        push_reply(t, {make_wire_reply(env, nullptr, 0)});
        return;
    }

    // Need total size up front so Studio's progress bar can render;
    // SIZE is cheap on vsftpd.
    std::uint64_t total = 0;
    t->ftp->size(path, &total);

    // Studio's download callback unconditionally reads `resp["file_md5"]`
    // once `offset + size == total`. If the field is missing nlohmann
    // throws type_error, the recv thread aborts, and the UI surfaces
    // "error 2". We accumulate MD5 incrementally via EVP so we can hand
    // it back on the final chunk without a second pass over the file.
    std::unique_ptr<EVP_MD_CTX, void(*)(EVP_MD_CTX*)> md_ctx(
        EVP_MD_CTX_new(),
        [](EVP_MD_CTX* c){ if (c) EVP_MD_CTX_free(c); });
    if (!md_ctx || EVP_DigestInit_ex(md_ctx.get(), EVP_md5(), nullptr) != 1) {
        auto env = make_reply_envelope(kCmdFileDownload, sequence, kResFileNoExist,
                                       obn::json::Value(obn::json::Object{}));
        push_reply(t, {make_wire_reply(env, nullptr, 0)});
        return;
    }

    // Stream from FTPS through `retr`, flushing reasonably-sized
    // chunks to Studio. Studio's HandleResponse concatenates chunks by
    // `offset`, so the chunk size just affects UI update cadence. 256
    // KB gives ~smooth 1-Hz percent updates on a 20 MB timelapse.
    constexpr std::size_t kChunkSize = 256 * 1024;
    std::vector<std::uint8_t> buf;
    buf.reserve(kChunkSize);
    std::uint64_t sent = 0;

    auto finalize_md5 = [&]() -> std::string {
        unsigned char digest[EVP_MAX_MD_SIZE] = {0};
        unsigned      digest_len = 0;
        EVP_DigestFinal_ex(md_ctx.get(), digest, &digest_len);
        static const char kHex[] = "0123456789abcdef";
        std::string hex;
        hex.resize(digest_len * 2);
        for (unsigned i = 0; i < digest_len; ++i) {
            hex[2*i    ] = kHex[(digest[i] >> 4) & 0xF];
            hex[2*i + 1] = kHex[ digest[i]       & 0xF];
        }
        return hex;
    };

    auto flush_chunk = [&](bool last) {
        obn::json::Object extra;
        extra["path"]   = obn::json::Value(path);
        extra["offset"] = obn::json::Value(static_cast<double>(sent));
        extra["total"]  = obn::json::Value(static_cast<double>(total));
        extra["size"]   = obn::json::Value(static_cast<double>(buf.size()));
        if (last) extra["file_md5"] = obn::json::Value(finalize_md5());
        int result = last ? kResOK : kResContinue;
        auto env = make_reply_envelope(kCmdFileDownload, sequence, result,
                                       obn::json::Value(std::move(extra)));
        push_reply(t, {make_wire_reply(env, buf.data(), buf.size())});
        sent += buf.size();
        buf.clear();
    };

    bool cancelled = false;
    err = t->ftp->retr(path, [&](const void* data, std::size_t len) {
        if (is_cancelled(t, sequence)) { cancelled = true; return false; }
        // Hash the whole stream (Studio MD5s the bytes it wrote, which
        // match what FTPS handed us modulo the chunk-size boundary).
        EVP_DigestUpdate(md_ctx.get(), data, len);
        const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
        while (len > 0) {
            std::size_t take = std::min(len, kChunkSize - buf.size());
            buf.insert(buf.end(), p, p + take);
            p   += take;
            len -= take;
            if (buf.size() == kChunkSize) flush_chunk(/*last=*/false);
        }
        return true;
    });

    if (cancelled) {
        auto env = make_reply_envelope(kCmdFileDownload, sequence, kResErrCancel,
                                       obn::json::Value(obn::json::Object{}));
        push_reply(t, {make_wire_reply(env, nullptr, 0)});
        return;
    }
    if (!err.empty()) {
        log_fmt(t->logger, t->log_ctx,
                "ctrl: FILE_DOWNLOAD %s failed: %s", path.c_str(), err.c_str());
        auto env = make_reply_envelope(kCmdFileDownload, sequence, kResFileNoExist,
                                       obn::json::Value(obn::json::Object{}));
        push_reply(t, {make_wire_reply(env, nullptr, 0)});
        return;
    }
    flush_chunk(/*last=*/true);
}

void handle_file_del(Tunnel* t, int sequence, const obn::json::Value& req)
{
    std::string err = ensure_ftp(t);
    if (!err.empty()) {
        auto env = make_reply_envelope(kCmdFileDel, sequence, kResStorUnavail,
                                       obn::json::Value(obn::json::Object{}));
        push_reply(t, {make_wire_reply(env, nullptr, 0)});
        return;
    }
    // Studio hands us either { "delete": [name1,...] } (legacy,
    // filenames only - we need to prefix with current subtree, but we
    // don't know which) or { "paths": [fqpath1, ...] } (modern, fully
    // qualified). We only implement the modern form: `paths`.
    obn::json::Value paths_v = req.find("paths");
    const auto& paths = paths_v.as_array();
    obn::json::Array ok_names;
    int result = kResOK;
    for (const auto& v : paths) {
        std::string p = v.as_string();
        if (p.empty()) continue;
        std::string e = t->ftp->dele(p);
        if (!e.empty()) {
            log_fmt(t->logger, t->log_ctx,
                    "ctrl: DELE %s failed: %s", p.c_str(), e.c_str());
            result = kResFileNoExist;
        } else {
            ok_names.emplace_back(obn::json::Value(p));
        }
    }
    obn::json::Object reply;
    reply["paths"] = obn::json::Value(std::move(ok_names));
    auto env = make_reply_envelope(kCmdFileDel, sequence, result,
                                   obn::json::Value(std::move(reply)));
    push_reply(t, {make_wire_reply(env, nullptr, 0)});
}

void handle_task_cancel(Tunnel* t, int sequence, const obn::json::Value& req)
{
    obn::json::Value tasks_v = req.find("tasks");
    const auto& tasks = tasks_v.as_array();
    obn::json::Array cancelled;
    std::string cancelled_log;
    {
        std::lock_guard<std::mutex> lk(t->ctrl_mu);
        for (const auto& v : tasks) {
            int seq = static_cast<int>(v.as_number());
            t->ctrl_cancelled.insert(seq);
            cancelled.emplace_back(obn::json::Value(static_cast<double>(seq)));
            if (!cancelled_log.empty()) cancelled_log += ",";
            cancelled_log += std::to_string(seq);
        }
    }
    log_fmt(t->logger, t->log_ctx,
            "ctrl: TASK_CANCEL seq=%d cancels=[%s]",
            sequence, cancelled_log.c_str());
    obn::json::Object reply;
    reply["tasks"] = obn::json::Value(std::move(cancelled));
    auto env = make_reply_envelope(kCmdTaskCancel, sequence, kResOK,
                                   obn::json::Value(std::move(reply)));
    push_reply(t, {make_wire_reply(env, nullptr, 0)});
}

// --------------------------------------------------------------------
// Worker thread entry point.
// --------------------------------------------------------------------
void ctrl_worker_main(Tunnel* t)
{
    log_fmt(t->logger, t->log_ctx, "ctrl: worker started");
    while (!t->ctrl_stop.load(std::memory_order_acquire)) {
        CtrlRequest req;
        {
            std::unique_lock<std::mutex> lk(t->ctrl_mu);
            t->ctrl_cv.wait_for(lk, std::chrono::milliseconds(200), [&] {
                return t->ctrl_stop.load(std::memory_order_acquire) ||
                       !t->ctrl_in.empty();
            });
            if (t->ctrl_stop.load(std::memory_order_acquire)) break;
            if (t->ctrl_in.empty()) continue;
            req = std::move(t->ctrl_in.front());
            t->ctrl_in.pop_front();
        }
        int cmdtype = 0, sequence = 0;
        obn::json::Value body;
        if (!parse_ctrl_request(req.body, &cmdtype, &sequence, &body)) {
            log_fmt(t->logger, t->log_ctx, "ctrl: failed to parse request, ignoring");
            continue;
        }
        log_fmt(t->logger, t->log_ctx,
                "ctrl: dispatch cmd=0x%04x seq=%d", cmdtype, sequence);
        switch (cmdtype) {
        case kCmdRequestMediaAbility: handle_media_ability(t, sequence, body); break;
        case kCmdListInfo:            handle_list_info(t, sequence, body); break;
        case kCmdSubFile:             handle_sub_file(t, sequence, body); break;
        case kCmdFileDownload:        handle_file_download(t, sequence, body); break;
        case kCmdFileDel:             handle_file_del(t, sequence, body); break;
        case kCmdTaskCancel:          handle_task_cancel(t, sequence, body); break;
        default: {
            // FILE_UPLOAD and anything else we haven't implemented:
            // reply API_UNSUPPORT instead of hanging Studio on a
            // missing callback.
            auto env = make_reply_envelope(cmdtype, sequence, kResApiUnsupport,
                                           obn::json::Value(obn::json::Object{}));
            push_reply(t, {make_wire_reply(env, nullptr, 0)});
            break;
        }
        }
    }
    if (t->ftp) {
        t->ftp->quit();
        t->ftp.reset();
    }
    log_fmt(t->logger, t->log_ctx, "ctrl: worker exited");
}

// Kicks off the worker. Tears down any MJPG socket we opened in
// Bambu_Open, because CTRL mode doesn't use it.
int start_ctrl_mode(Tunnel* t)
{
    if (t->ctrl_mode) return Bambu_success;
    // Close the MJPG socket - CTRL multiplexes over FTPS instead.
    if (t->ssl) {
        SSL_shutdown(t->ssl);
        SSL_free(t->ssl);
        t->ssl = nullptr;
    }
    if (t->fd >= 0) {
        ::shutdown(t->fd, SHUT_RDWR);
        ::close(t->fd);
        t->fd = -1;
    }
    t->ctrl_mode = true;
    t->ctrl_stop.store(false);
    t->ctrl_worker = std::thread(ctrl_worker_main, t);
    log_fmt(t->logger, t->log_ctx, "ctrl: switched to CTRL mode");
    return Bambu_success;
}

// Shuts the worker down. Idempotent.
void stop_ctrl_mode(Tunnel* t)
{
    if (!t->ctrl_mode) return;
    {
        std::lock_guard<std::mutex> lk(t->ctrl_mu);
        t->ctrl_stop.store(true, std::memory_order_release);
        t->ctrl_cv.notify_all();
    }
    if (t->ctrl_worker.joinable()) t->ctrl_worker.join();
    t->ctrl_mode = false;
}

} // namespace

// =======================================================================
// Exported BambuLib API
// =======================================================================

OBN_EXPORT int Bambu_Init()
{
    ssl_init_once();
    return Bambu_success;
}

OBN_EXPORT void Bambu_Deinit()
{
    // No-op: we intentionally leak the global SSL_CTX until process exit.
    // Tearing it down while other tunnels might still be alive on a
    // different GstElement is not worth the race risk.
}

OBN_EXPORT int Bambu_Create(Bambu_Tunnel* tunnel, char const* path)
{
    if (!tunnel || !path) return -1;
    ssl_init_once();
    auto* t = new Tunnel();
    // Hide the password from the mirror log but keep the host/port/user
    // portion so we know what the caller actually asked for.
    log_fmt(t->logger, t->log_ctx, "Bambu_Create: url=%.160s%s", path,
            std::strlen(path) > 160 ? "..." : "");
    if (!parse_url(path, &t->url)) {
        log_fmt(t->logger, t->log_ctx, "Bambu_Create: bad URL");
        delete t;
        set_last_error("bad URL");
        return -1;
    }
    const char* scheme_name = (t->url.scheme == Scheme::Rtsps) ? "rtsps"
                            : (t->url.scheme == Scheme::Rtsp)  ? "rtsp"
                            :                                    "local";
    log_fmt(t->logger, t->log_ctx,
            "Bambu_Create: parsed scheme=%s host=%s port=%d path=%s "
            "user=%s passwd=%s",
            scheme_name, t->url.host.c_str(), t->url.port,
            t->url.path.c_str(), t->url.user.c_str(),
            t->url.passwd.empty() ? "(empty!)" : "***");
    *tunnel = t;
    return Bambu_success;
}

OBN_EXPORT void Bambu_SetLogger(Bambu_Tunnel tunnel, Logger logger, void* context)
{
    auto* t = static_cast<Tunnel*>(tunnel);
    if (!t) return;
    t->logger  = logger ? logger : noop_logger;
    t->log_ctx = context;
}

OBN_EXPORT int Bambu_Open(Bambu_Tunnel tunnel)
{
    auto* t = static_cast<Tunnel*>(tunnel);
    if (!t) return -1;

    // RTSP(S) is so different from MJPG that it gets its own code path
    // (pipeline build + gst bus wait); MJPG stays as manual TLS + auth
    // packet below.
    if (t->url.scheme == Scheme::Rtsps || t->url.scheme == Scheme::Rtsp) {
#if OBN_ENABLE_WORKAROUNDS
        return open_rtsp(t);
#else
        // RTSPS transcoding is a workaround: the stock BambuSource
        // hands raw H.264 back to Studio's gstbambusrc, we turn it
        // into MJPEG first. With the workaround disabled we bail
        // cleanly so the user notices instead of getting an opaque
        // "camera not connected" spinner.
        set_last_error("RTSPS liveview disabled (OBN_ENABLE_WORKAROUNDS=OFF)");
        log_fmt(t->logger, t->log_ctx,
                "Bambu_Open: RTSPS(%s) refused: workarounds disabled",
                t->url.host.c_str());
        return -1;
#endif
    }

    log_fmt(t->logger, t->log_ctx, "Bambu_Open: dialing %s:%d",
            t->url.host.c_str(), t->url.port);

    t->fd = dial(t->url.host, t->url.port, /*timeout_ms=*/5000);
    if (t->fd < 0) {
        log_fmt(t->logger, t->log_ctx, "Bambu_Open: connect failed: %s",
                g_last_error.c_str());
        return -1;
    }
    log_fmt(t->logger, t->log_ctx, "Bambu_Open: TCP connected, fd=%d", t->fd);

    if (!g_ssl_ctx) {
        set_last_error("SSL_CTX not ready");
        tunnel_close(t);
        return -1;
    }
    t->ssl = SSL_new(g_ssl_ctx);
    if (!t->ssl) {
        set_last_error("SSL_new failed");
        tunnel_close(t);
        return -1;
    }
    SSL_set_fd(t->ssl, t->fd);
    // SNI: some self-signed printer certs are issued for the device IP;
    // set it anyway so they can still inspect it server-side.
    SSL_set_tlsext_host_name(t->ssl, t->url.host.c_str());
    int rc = SSL_connect(t->ssl);
    if (rc != 1) {
        char errbuf[256];
        ERR_error_string_n(ERR_get_error(), errbuf, sizeof(errbuf));
        log_fmt(t->logger, t->log_ctx,
                "Bambu_Open: TLS handshake failed: %s", errbuf);
        tunnel_close(t);
        return -1;
    }

    log_fmt(t->logger, t->log_ctx,
            "Bambu_Open: TLS established (cipher=%s)",
            SSL_get_cipher(t->ssl));

    // Auth packet.
    uint8_t auth[80];
    build_auth_packet(t->url, auth);
    if (ssl_write_all(t->ssl, auth, sizeof(auth)) != 0) {
        log_fmt(t->logger, t->log_ctx, "Bambu_Open: auth write failed");
        tunnel_close(t);
        return -1;
    }

    t->t0      = std::chrono::steady_clock::now();
    t->started = true;
    log_fmt(t->logger, t->log_ctx,
            "Bambu_Open: sent %zu-byte auth packet (user=%s pw_len=%zu)",
            sizeof(auth), t->url.user.c_str(), t->url.passwd.size());
    return Bambu_success;
}

OBN_EXPORT int Bambu_StartStream(Bambu_Tunnel tunnel, bool /*video*/)
{
    // Both protocols start streaming implicitly:
    //   * MJPG: printer begins pushing frames right after auth.
    //   * RTSP: Bambu_Open already set the pipeline to PLAYING.
    auto* t = static_cast<Tunnel*>(tunnel);
    if (!t) return -1;
    if (t->url.scheme == Scheme::Local && !t->ssl) return -1;
    if ((t->url.scheme == Scheme::Rtsps ||
         t->url.scheme == Scheme::Rtsp) && !t->pipeline) return -1;
    return Bambu_success;
}

OBN_EXPORT int Bambu_StartStreamEx(Bambu_Tunnel tunnel, int type)
{
    auto* t = static_cast<Tunnel*>(tunnel);
    if (!t) return -1;
    // CTRL_TYPE (0x3001) means "open the PrinterFileSystem channel
    // instead of video". Studio's MediaFilePanel calls this right
    // after Bambu_Open to multiplex JSON requests over the same
    // tunnel. We tear down the MJPG TCP connection (which Bambu_Open
    // already brought up) and spin up the FTPS-backed worker.
    if (type == kCtrlType) {
#if OBN_ENABLE_WORKAROUNDS
        return start_ctrl_mode(t);
#else
        // Stock plugin speaks a proprietary CTRL wire we don't
        // implement. Without the workaround the MediaFilePanel stays
        // empty / "Browsing file in storage is not supported".
        set_last_error("PrinterFileSystem bridge disabled (OBN_ENABLE_WORKAROUNDS=OFF)");
        log_fmt(t->logger, t->log_ctx,
                "Bambu_StartStreamEx(CTRL): refused, workarounds disabled");
        return -1;
#endif
    }
    return Bambu_StartStream(tunnel, true);
}

OBN_EXPORT int Bambu_GetStreamCount(Bambu_Tunnel tunnel)
{
    auto* t = static_cast<Tunnel*>(tunnel);
    if (!t) return 0;
    if (t->url.scheme == Scheme::Local && !t->ssl)      return 0;
    if ((t->url.scheme == Scheme::Rtsps ||
         t->url.scheme == Scheme::Rtsp) && !t->pipeline) return 0;
    return 1; // one video track, either MJPG or H.264
}

OBN_EXPORT int Bambu_GetStreamInfo(Bambu_Tunnel tunnel, int index,
                                   Bambu_StreamInfo* info)
{
    auto* t = static_cast<Tunnel*>(tunnel);
    if (!t || !info || index != 0) return -1;
    std::memset(info, 0, sizeof(*info));
    info->type                    = VIDE;
    info->sub_type                = t->sub_type;
    info->format.video.width      = t->width;
    info->format.video.height     = t->height;
    info->format.video.frame_rate = t->frame_rate;
    // For H.264, gstbambusrc feeds the buffer into a decoder bin which
    // sniffs the byte stream; video_avc_byte_stream mirrors what the
    // proprietary plugin advertises in RTSP mode.
    info->format_type             = (t->sub_type == MJPG)
                                        ? video_jpeg
                                        : video_avc_byte_stream;
    info->format_size             = 0;
    info->max_frame_size          = static_cast<int>(kMaxFrameSize);
    info->format_buffer           = nullptr;
    return Bambu_success;
}

OBN_EXPORT unsigned long Bambu_GetDuration(Bambu_Tunnel /*tunnel*/)
{
    return 0; // live stream, no duration
}

OBN_EXPORT int Bambu_Seek(Bambu_Tunnel /*tunnel*/, unsigned long /*time*/)
{
    return Bambu_success; // meaningless for a live stream
}

OBN_EXPORT int Bambu_ReadSample(Bambu_Tunnel tunnel, Bambu_Sample* sample)
{
    auto* t = static_cast<Tunnel*>(tunnel);
    if (!t || !sample) return -1;

    // CTRL channel: drain one reply from the outbox. PrinterFileSystem
    // on Studio's side will loop calling us until would_block or until
    // it sees the terminal reply it's waiting on.
    if (t->ctrl_mode) {
        std::unique_lock<std::mutex> lk(t->ctrl_mu);
        if (t->ctrl_out.empty()) return Bambu_would_block;
        CtrlReply r = std::move(t->ctrl_out.front());
        t->ctrl_out.pop_front();
        lk.unlock();
        // Stash on the tunnel so the pointer stays valid until next
        // ReadSample (matches the contract with gstbambusrc). PrinterFileSystem
        // treats decode_time as irrelevant; we set it to 0.
        t->ctrl_current_reply = std::move(r.data);
        sample->itrack      = 0;
        sample->size        = static_cast<int>(t->ctrl_current_reply.size());
        sample->flags       = 0;
        sample->buffer      = reinterpret_cast<const unsigned char*>(
                                   t->ctrl_current_reply.data());
        sample->decode_time = 0;
        return Bambu_success;
    }

    // RTSP: pull from appsink. MJPG path continues below.
    if (t->url.scheme == Scheme::Rtsps || t->url.scheme == Scheme::Rtsp) {
        return read_rtsp(t, sample);
    }

    if (!t->ssl) return -1;

    // Read 16-byte frame header.
    uint8_t hdr[16];
    int rc = ssl_read_all(t, hdr, sizeof(hdr));
    if (rc < 0) {
        set_last_error("header read failed");
        return -1;
    }
    if (rc > 0) return Bambu_stream_end;

    auto u32 = [&](size_t off) -> uint32_t {
        return  (static_cast<uint32_t>(hdr[off + 0]))
              | (static_cast<uint32_t>(hdr[off + 1]) << 8)
              | (static_cast<uint32_t>(hdr[off + 2]) << 16)
              | (static_cast<uint32_t>(hdr[off + 3]) << 24);
    };
    uint32_t payload_size = u32(0);
    uint32_t itrack       = u32(4);
    uint32_t flags        = u32(8);

    if (payload_size == 0 || payload_size > kMaxFrameSize) {
        log_fmt(t->logger, t->log_ctx,
                "Bambu_ReadSample: bogus payload size %u", payload_size);
        set_last_error("bogus payload size");
        return -1;
    }

    t->frame_buf.resize(payload_size);
    rc = ssl_read_all(t, t->frame_buf.data(), payload_size);
    if (rc < 0) {
        set_last_error("payload read failed");
        return -1;
    }
    if (rc > 0) return Bambu_stream_end;

    // Sanity check: MJPG frames start with 0xFF 0xD8 and end with
    // 0xFF 0xD9. If the magic is wrong we probably lost sync; bailing
    // out lets gstbambusrc tear the pipeline down and reconnect.
    if (payload_size < 4 ||
        t->frame_buf[0] != 0xFF || t->frame_buf[1] != 0xD8 ||
        t->frame_buf[payload_size - 2] != 0xFF ||
        t->frame_buf[payload_size - 1] != 0xD9) {
        log_fmt(t->logger, t->log_ctx,
                "Bambu_ReadSample: JPEG magic mismatch size=%u", payload_size);
        set_last_error("JPEG magic mismatch");
        return -1;
    }

    auto now = std::chrono::steady_clock::now();
    auto ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(now - t->t0).count();

    if (++t->frame_count == 1 || (t->frame_count % 60) == 0) {
        log_fmt(t->logger, t->log_ctx,
                "Bambu_ReadSample: frame #%llu size=%u itrack=%u flags=%u",
                static_cast<unsigned long long>(t->frame_count),
                payload_size, itrack, flags);
    }

    sample->itrack      = static_cast<int>(itrack);
    sample->size        = static_cast<int>(payload_size);
    sample->flags       = static_cast<int>(flags);
    sample->buffer      = t->frame_buf.data();
    // gstbambusrc multiplies decode_time by 100 to get ns, so we divide.
    sample->decode_time = static_cast<unsigned long long>(ns / 100);
    return Bambu_success;
}

OBN_EXPORT int Bambu_SendMessage(Bambu_Tunnel tunnel, int ctrl,
                                 char const* data, int len)
{
    auto* t = static_cast<Tunnel*>(tunnel);
    if (!t || !data || len <= 0) return -1;
    // Only CTRL_TYPE is known to us. Any other ctrl channel is a
    // no-op (returns success so the caller doesn't interpret it as
    // pipe-dead); stock firmware uses the same channel number.
    if (ctrl != kCtrlType) return Bambu_success;
    if (!t->ctrl_mode) {
        // Auto-switch into CTRL if Studio skipped StartStreamEx. This
        // shouldn't happen, but it's cheap insurance.
        start_ctrl_mode(t);
    }
    CtrlRequest req;
    req.body.assign(data, static_cast<std::size_t>(len));
    {
        std::lock_guard<std::mutex> lk(t->ctrl_mu);
        t->ctrl_in.emplace_back(std::move(req));
        t->ctrl_cv.notify_all();
    }
    return Bambu_success;
}

OBN_EXPORT int Bambu_RecvMessage(Bambu_Tunnel /*tunnel*/, int* /*ctrl*/,
                                 char* /*data*/, int* /*len*/)
{
    // Studio uses Bambu_ReadSample for CTRL replies, not RecvMessage.
    // Keep this as a polite "no data".
    return Bambu_would_block;
}

OBN_EXPORT void Bambu_Close(Bambu_Tunnel tunnel)
{
    auto* t = static_cast<Tunnel*>(tunnel);
    if (!t) return;
    // CTRL worker must be joined before the rest of the tunnel is
    // dismantled (it holds references to SSL / ftp that tunnel_close
    // would invalidate).
    stop_ctrl_mode(t);
    tunnel_close(t);
}

OBN_EXPORT void Bambu_Destroy(Bambu_Tunnel tunnel)
{
    auto* t = static_cast<Tunnel*>(tunnel);
    if (!t) return;
    stop_ctrl_mode(t);
    tunnel_close(t);
    delete t;
}

OBN_EXPORT char const* Bambu_GetLastErrorMsg()
{
    // Stock plugin returns a static string; we return a thread-local
    // pointer that remains valid until the next set_last_error on
    // the same thread. That matches how Studio actually uses it
    // (printed immediately, not stored).
    return g_last_error.c_str();
}

OBN_EXPORT void Bambu_FreeLogMsg(tchar const* msg)
{
    // We allocated with strdup() in log_fmt(), so use free().
    // gstbambusrc calls this once per log line; other callers (the
    // Studio-side `_log` helper in GUI_App / wxMediaCtrl3) do the same.
    if (msg) std::free(const_cast<char*>(msg));
}

// Legacy probe: the older stub exported this so callers could tell at a
// glance that they loaded our build. Keep it around (no-op).
OBN_EXPORT int bambu_source_is_stub()
{
    return 0; // now a real implementation
}
