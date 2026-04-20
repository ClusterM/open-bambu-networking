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
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

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
int open_rtsp(Tunnel* t)
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
        return open_rtsp(t);
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

OBN_EXPORT int Bambu_StartStreamEx(Bambu_Tunnel tunnel, int /*type*/)
{
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

OBN_EXPORT int Bambu_SendMessage(Bambu_Tunnel /*tunnel*/, int /*ctrl*/,
                                 char const* /*data*/, int /*len*/)
{
    // Only used by PrinterFileSystem (not for video). Unsupported here;
    // returning an error is safer than a spurious success.
    return -1;
}

OBN_EXPORT int Bambu_RecvMessage(Bambu_Tunnel /*tunnel*/, int* /*ctrl*/,
                                 char* /*data*/, int* /*len*/)
{
    return -1;
}

OBN_EXPORT void Bambu_Close(Bambu_Tunnel tunnel)
{
    auto* t = static_cast<Tunnel*>(tunnel);
    if (!t) return;
    tunnel_close(t);
}

OBN_EXPORT void Bambu_Destroy(Bambu_Tunnel tunnel)
{
    auto* t = static_cast<Tunnel*>(tunnel);
    if (!t) return;
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
