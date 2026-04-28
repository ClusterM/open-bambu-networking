// macOS BambuPlayer: the Objective-C class wxMediaCtrl2.mm in
// OrcaSlicer / Bambu Studio looks up via dlsym(libBambuSource.dylib,
// "OBJC_CLASS_$_BambuPlayer"). When the symbol is missing Studio
// drops m_error to -2 and leaves the camera widget in
// MEDIASTATE_LOADING forever ("infinite spinner"). This file is the
// missing class.
//
// We intentionally implement only the methods the Studio caller
// exercises (initWithImageView, setLogger, open, play, stop, close,
// videoSize) -- initWithDisplayLayer is declared for header
// compatibility but routes to the same internal init since
// AVSampleBufferDisplayLayer is a CALayer subclass and we render via
// .contents either way.
//
// Render path: both MJPG (port 6000) and RTSPS (port 322 transcoded
// to MJPEG by IVideoPipeline) end up handing us JPEG bytes. We turn
// each into a CGImage and assign it to the host CALayer's `contents`
// from the main queue, the same low-overhead path AppKit uses for
// stock NSImageView. AVSampleBufferDisplayLayer + CMSampleBuffer
// would be marginally cheaper for H.264 but only matters once the
// FFmpeg backend grows a hardware-decode bypass (VideoToolbox).
//
// Threading: a producer thread owned by IVideoPipeline / MjpgReader
// does the network IO + transcode. We pull from it on a poll thread
// (start once in -play, joined in -stop/-close) so the producer
// pipeline never blocks the AppKit main queue. CALayer.contents
// updates are dispatched to dispatch_get_main_queue.

#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "mjpg_reader.hpp"
#include "source_log.hpp"
#include "video_pipeline.hpp"

#if defined(_WIN32)
#    define OBN_EXPORT extern "C" __declspec(dllexport)
#else
#    define OBN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// -----------------------------------------------------------------------
// URL parser shared with BambuSource.cpp's tunnel layer. Defined here
// (file-local) instead of pulling the BambuSource.cpp version into a
// header because the URL types are slightly different on the C-ABI
// path (it has a Tunnel struct around them); a redundant 80-line
// parser is cheaper than another internal-only header churn.
// -----------------------------------------------------------------------

namespace {

enum class Scheme {
    Local, // MJPG over TCP/TLS on <port> (default 6000)
    Rtsps, // RTSPS on <port> (default 322)
    Rtsp,  // plain RTSP on <port> (default 554)
};

struct ParsedUrl {
    Scheme      scheme = Scheme::Local;
    std::string host;
    int         port = 6000;
    std::string user = "bblp";
    std::string passwd;
    std::string device;
    std::string path = "/streaming/live/1";
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

bool parse_url(const std::string& url, ParsedUrl* out)
{
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
        out->scheme = Scheme::Local;
        out->port   = 6000;
        rest = url;
    }

    auto q_pos = rest.find('?');
    std::string host_part = (q_pos == std::string::npos) ? rest : rest.substr(0, q_pos);
    std::string query     = (q_pos == std::string::npos) ? ""   : rest.substr(q_pos + 1);

    if (out->scheme == Scheme::Rtsps || out->scheme == Scheme::Rtsp) {
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
            out->path = host_part.substr(slash);
            host_part = host_part.substr(0, slash);
        }
    } else {
        while (!host_part.empty() &&
               (host_part.back() == '/' || host_part.back() == '.'))
            host_part.pop_back();
    }

    auto colon = host_part.find(':');
    if (colon != std::string::npos) {
        out->host = host_part.substr(0, colon);
        try {
            out->port = std::stoi(host_part.substr(colon + 1));
        } catch (...) { return false; }
    } else {
        out->host = host_part;
    }

    size_t i = 0;
    while (i < query.size()) {
        auto amp = query.find('&', i);
        if (amp == std::string::npos) amp = query.size();
        auto kv = query.substr(i, amp - i);
        auto eq = kv.find('=');
        std::string key = (eq == std::string::npos) ? kv : kv.substr(0, eq);
        std::string val = (eq == std::string::npos) ? "" : url_decode(kv.substr(eq + 1));
        if      (key == "port")   { try { out->port = std::stoi(val); } catch (...) {} }
        else if (key == "user")   { out->user = val; }
        else if (key == "passwd") { out->passwd = val; }
        else if (key == "device") { out->device = val; }
        i = amp + 1;
    }

    return !out->host.empty() && out->port > 0;
}

// Studio's logger callback is `void (void const* ctx, int level, char const* msg)`;
// our internal source_log expects `void (void* ctx, int level, char const* msg)`.
// A cast at the boundary is the only friction.
using StudioLogger = void (*)(void const*, int, char const*);

// Trampoline that converts an obn::source::log_at call into the
// signature wxMediaCtrl2.mm's `bambu_log` expects.
struct LoggerBridge {
    StudioLogger fn  = nullptr;
    void const*  ctx = nullptr;
};

void logger_trampoline(void* ctx, int level, char const* msg)
{
    auto* b = static_cast<LoggerBridge*>(ctx);
    if (b && b->fn) b->fn(b->ctx, level, msg);
}

CGImageRef cg_image_from_jpeg(const uint8_t* bytes, size_t len)
{
    if (!bytes || !len) return nullptr;
    CFDataRef data = CFDataCreate(nullptr, bytes, static_cast<CFIndex>(len));
    if (!data) return nullptr;
    CGImageSourceRef src = CGImageSourceCreateWithData(data, nullptr);
    CFRelease(data);
    if (!src) return nullptr;
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    return img;
}

} // namespace

// CMakeLists.txt sets -fvisibility=hidden globally, which on clang
// also hides Objective-C class metadata symbols. Tag the class with
// default visibility so dlsym(libBambuSource.dylib,
// "OBJC_CLASS_$_BambuPlayer") in OrcaSlicer's wxMediaCtrl2.mm
// resolves -- otherwise we are right back at the m_error = -2
// "missing player class" symptom this whole file is fixing.
__attribute__((visibility("default")))
@interface BambuPlayer : NSObject
@end

@implementation BambuPlayer
{
    // Where we render. At least one of `_renderLayer` or
    // `_displayLayer` is non-nil after init; we always render via
    // CALayer.contents for first-pass simplicity, both AVSBDL and
    // plain CALayer accept that.
    CALayer*                    _renderLayer;
    AVSampleBufferDisplayLayer* _displayLayer;
    NSView*                     _hostView;

    // Logger relay for Studio's bambu_log callback.
    LoggerBridge                _logBridge;

    // Last URL Studio asked us to open. -play uses it to pick a
    // backend; -close clears it.
    ParsedUrl                   _url;
    BOOL                        _hasUrl;

    // Either MJPG reader (port 6000) or video pipeline (RTSPS) is
    // populated, never both.
    std::unique_ptr<obn::mjpg::Reader>           _mjpgReader;
    std::unique_ptr<obn::video::IVideoPipeline>  _videoPipe;

    // Polling thread that pulls frames out of the producer and
    // dispatches CALayer.contents updates onto the main queue.
    std::thread                 _pollThread;
    std::atomic<bool>           _stopFlag;

    // Last decoded frame size, surfaced through -videoSize.
    std::atomic<int>            _lastWidth;
    std::atomic<int>            _lastHeight;
}

+ (void)initialize
{
    // Studio calls the meta-class +initialize once before instances
    // are made. Nothing to do here for now: SSL / FFmpeg / Logger
    // initialisation all happen lazily inside their respective
    // first-use call sites.
}

- (instancetype)initWithImageView:(NSView*)view
{
    self = [super init];
    if (!self) return nil;
    // In C++17 std::atomic's default ctor leaves the value
    // indeterminate. Studio can ask -videoSize at any point, so
    // initialise these explicitly before any reader could touch them.
    _stopFlag.store(false, std::memory_order_release);
    _lastWidth.store(0,  std::memory_order_release);
    _lastHeight.store(0, std::memory_order_release);
    _hasUrl    = NO;
    _hostView = view;

    // Studio's wxMediaCtrl2 ctor sets `view.layer = [[CALayer alloc] init]`
    // and `view.wantsLayer = YES`, but we can't assume the host order.
    // Be defensive.
    if (view.layer == nil) {
        view.layer = [CALayer layer];
        view.wantsLayer = YES;
    }

    // Add a dedicated child layer for the video frame so we don't
    // stomp on the host layer's `contents` in case Studio reuses it
    // for anything else later. The child autoresizes with the host
    // and centers the image proportionally via `contentsGravity`.
    _renderLayer = [CALayer layer];
    _renderLayer.frame             = view.layer.bounds;
    _renderLayer.autoresizingMask  = kCALayerWidthSizable | kCALayerHeightSizable;
    _renderLayer.contentsGravity   = kCAGravityResizeAspect;
    _renderLayer.backgroundColor   = CGColorGetConstantColor(kCGColorBlack);
    [view.layer addSublayer:_renderLayer];

    return self;
}

- (instancetype)initWithDisplayLayer:(AVSampleBufferDisplayLayer*)layer
{
    self = [super init];
    if (!self) return nil;
    _stopFlag.store(false, std::memory_order_release);
    _lastWidth.store(0,  std::memory_order_release);
    _lastHeight.store(0, std::memory_order_release);
    _hasUrl       = NO;
    _displayLayer = layer;
    // AVSampleBufferDisplayLayer is a CALayer subclass; we'll set its
    // .contents directly the same as the image-view path. Studio
    // never exercises this codepath (wxMediaCtrl2.mm only calls
    // -initWithImageView:), but the header declares it so dlsym
    // probes don't trip on a missing selector.
    _renderLayer = layer;
    return self;
}

- (void)setLogger:(StudioLogger)logger withContext:(void const*)context
{
    _logBridge.fn  = logger;
    _logBridge.ctx = context;
}

- (NSSize)videoSize
{
    return NSMakeSize(_lastWidth.load(std::memory_order_acquire),
                      _lastHeight.load(std::memory_order_acquire));
}

- (int)open:(char const*)url
{
    if (!url) return -1;
    [self close];

    if (!parse_url(std::string(url), &_url)) {
        obn::source::log_fmt(logger_trampoline, &_logBridge,
                             "BambuPlayer: bad URL");
        return -1;
    }
    _hasUrl = YES;

    const char* scheme_name = (_url.scheme == Scheme::Rtsps) ? "rtsps"
                            : (_url.scheme == Scheme::Rtsp)  ? "rtsp"
                                                              : "local";
    obn::source::log_fmt(logger_trampoline, &_logBridge,
                         "BambuPlayer: open scheme=%s host=%s port=%d user=%s",
                         scheme_name, _url.host.c_str(), _url.port,
                         _url.user.c_str());
    return 0;
}

- (int)play
{
    if (!_hasUrl) {
        obn::source::log_fmt(logger_trampoline, &_logBridge,
                             "BambuPlayer: play before open");
        return -1;
    }
    if (_pollThread.joinable()) {
        // Studio occasionally calls -play multiple times in a row;
        // ignore duplicates instead of leaking threads.
        return 0;
    }

    _stopFlag.store(false, std::memory_order_release);

    if (_url.scheme == Scheme::Local) {
        obn::mjpg::Config cfg;
        cfg.host   = _url.host;
        cfg.port   = _url.port;
        cfg.user   = _url.user;
        cfg.passwd = _url.passwd;
        _mjpgReader = obn::mjpg::make_reader(logger_trampoline, &_logBridge);
        if (_mjpgReader->start(cfg) != 0) {
            obn::source::log_fmt(logger_trampoline, &_logBridge,
                                 "BambuPlayer: mjpg start failed: %s",
                                 obn::source::get_last_error());
            _mjpgReader.reset();
            return -1;
        }
    } else {
        _videoPipe = obn::video::make_video_pipeline(logger_trampoline, &_logBridge);
        if (!_videoPipe) {
            obn::source::log_fmt(logger_trampoline, &_logBridge,
                                 "BambuPlayer: no video backend compiled in");
            return -1;
        }
        obn::video::StartConfig cfg;
        cfg.scheme = (_url.scheme == Scheme::Rtsps)
                         ? obn::video::Scheme::Rtsps
                         : obn::video::Scheme::Rtsp;
        cfg.host   = _url.host;
        cfg.port   = _url.port;
        cfg.user   = _url.user;
        cfg.passwd = _url.passwd;
        cfg.path   = _url.path;
        cfg.target_width  = 1280;
        cfg.target_height = 720;
        cfg.target_fps    = 30;
        if (_videoPipe->start(cfg) != 0) {
            obn::source::log_fmt(logger_trampoline, &_logBridge,
                                 "BambuPlayer: rtsp start failed: %s",
                                 obn::source::get_last_error());
            _videoPipe.reset();
            return -1;
        }
    }

    // Capture self via __unsafe_unretained so the worker thread does
    // not bump our retain count. -dealloc joins this thread before
    // letting ARC destruct ivars, so the raw pointer stays valid for
    // the entire lifetime of the lambda.
    //
    // The strong-capture alternative (`[self]` in the lambda) is
    // worse here: Studio's wxMediaCtrl2 destructor invokes
    // `[player dealloc]` directly (legacy MRC pattern), and the
    // refcount accounting does not match what ARC expects. A strong
    // capture would land us in a release-during-dealloc reentry that
    // is at best brittle, at worst a re-dealloc.
    __unsafe_unretained BambuPlayer* unsafeSelf = self;
    _pollThread = std::thread([unsafeSelf]() { [unsafeSelf pollLoop]; });
    return 0;
}

- (void)pollLoop
{
    using clock = std::chrono::steady_clock;
    while (!_stopFlag.load(std::memory_order_acquire)) {
        const std::uint8_t* jpeg = nullptr;
        std::size_t         len  = 0;
        bool                stream_end = false;
        bool                fatal      = false;

        if (_mjpgReader) {
            obn::mjpg::Frame f;
            switch (_mjpgReader->try_pull(&f)) {
            case obn::mjpg::Pull_Ok:
                jpeg = f.jpeg;
                len  = f.size;
                break;
            case obn::mjpg::Pull_StreamEnd:
                stream_end = true;
                break;
            case obn::mjpg::Pull_Error:
                fatal = true;
                break;
            case obn::mjpg::Pull_WouldBlock:
            default:
                break;
            }
        } else if (_videoPipe) {
            obn::video::Frame f;
            switch (_videoPipe->try_pull(&f)) {
            case obn::video::Pull_Ok:
                jpeg = f.jpeg;
                len  = f.size;
                if (f.width  > 0) _lastWidth.store(f.width,  std::memory_order_release);
                if (f.height > 0) _lastHeight.store(f.height, std::memory_order_release);
                break;
            case obn::video::Pull_StreamEnd:
                stream_end = true;
                break;
            case obn::video::Pull_Error:
                fatal = true;
                break;
            case obn::video::Pull_WouldBlock:
            default:
                break;
            }
        }

        if (fatal || stream_end) {
            obn::source::log_fmt(logger_trampoline, &_logBridge,
                                 "BambuPlayer: stream %s",
                                 fatal ? "error" : "ended");
            // Studio's bambu_log treats negative levels as
            // "stopped" -- emit one synthetic line so wxMediaCtrl2
            // transitions into wxMEDIASTATE_STOPPED instead of
            // sitting in LOADING forever.
            if (_logBridge.fn) {
                _logBridge.fn(_logBridge.ctx, -1, "stream end");
            }
            break;
        }

        if (jpeg && len > 0) {
            CGImageRef img = cg_image_from_jpeg(jpeg, len);
            if (img) {
                int w = static_cast<int>(CGImageGetWidth(img));
                int h = static_cast<int>(CGImageGetHeight(img));
                if (w > 0) _lastWidth.store(w, std::memory_order_release);
                if (h > 0) _lastHeight.store(h, std::memory_order_release);
                // Hop to the main queue for the contents update.
                // CGImageRelease happens after the assignment because
                // CALayer takes its own reference on assignment.
                __weak BambuPlayer* weakSelf = self;
                dispatch_async(dispatch_get_main_queue(), ^{
                    BambuPlayer* strongSelf = weakSelf;
                    if (strongSelf && strongSelf->_renderLayer) {
                        strongSelf->_renderLayer.contents = (__bridge id)img;
                    }
                    CGImageRelease(img);
                });
            }
        } else {
            // No frame yet: short sleep so we don't spin a CPU core.
            // 16 ms keeps us under one 60 Hz frame of latency.
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }
}

- (void)stop
{
    [self close];
}

- (void)close
{
    _stopFlag.store(true, std::memory_order_release);
    // Stop producers first so the poll thread sees stream-end and
    // exits promptly; then join the poll thread; then drop the
    // pipeline. The order matters because both _mjpgReader and
    // _videoPipe own threads of their own that we join inside
    // their stop()s.
    if (_mjpgReader) _mjpgReader->stop();
    if (_videoPipe)  _videoPipe->stop();
    if (_pollThread.joinable()) _pollThread.join();
    _mjpgReader.reset();
    _videoPipe.reset();
    _hasUrl = NO;
}

- (void)dealloc
{
    // Studio's wxMediaCtrl2 destructor explicitly calls -dealloc
    // (legacy MRC pattern; see 3rd_party/OrcaSlicer/src/slic3r/GUI/
    // wxMediaCtrl2.mm). Under ARC we cannot invoke [super dealloc]
    // ourselves -- it is generated automatically -- but cleanup of
    // C++ ivars and worker threads still has to happen here so the
    // process doesn't leave a dangling thread reading from torn-down
    // sockets.
    _stopFlag.store(true, std::memory_order_release);
    if (_mjpgReader) _mjpgReader->stop();
    if (_videoPipe)  _videoPipe->stop();
    if (_pollThread.joinable()) _pollThread.join();
}

@end

// Force linker to keep the BambuPlayer class symbol exported. Without
// this, -fvisibility=hidden in CMakeLists.txt would strip
// OBJC_CLASS_$_BambuPlayer from the dylib's symbol table and
// dlsym(lib, "OBJC_CLASS_$_BambuPlayer") in wxMediaCtrl2.mm would
// return NULL again -- the original bug we are here to fix.
//
// __attribute__((visibility("default"))) on the @implementation also
// works on clang, but referencing the class once from an exported
// extern "C" symbol is portable across compiler versions and serves
// as a self-test (-Werror would catch a typo in the class name).
OBN_EXPORT void* obn_bambuplayer_keep_symbol(void)
{
    return (__bridge void*)[BambuPlayer class];
}
