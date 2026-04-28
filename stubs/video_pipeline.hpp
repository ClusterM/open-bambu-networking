// libBambuSource video pipeline abstraction.
//
// libBambuSource carries two independent video paths:
//
//   1. MJPG over TLS on port 6000 (A1/A1 mini/P1/P1P). The printer
//      pushes raw JPEG frames straight at us inside an authenticated
//      TLS tunnel. The C-API in BambuSource.cpp implements that
//      directly with OpenSSL and does NOT use this interface.
//
//   2. RTSPS on port 322 (X1/P1S/P2S). The printer speaks RTSP/RTP
//      with H.264 inside RTP packets. We need to demux + decode +
//      re-encode to JPEG so the C-API can hand JPEG to gstbambusrc
//      (which ultimately means Studio's media widget treats every
//      printer the same way).
//
// IVideoPipeline owns the entire RTSPS lifecycle: connect, demux,
// decode, transcode to MJPEG, deliver one JPEG frame at a time. The
// interface deliberately speaks JPEG rather than raw YUV so the
// existing BambuSource.cpp -> gstbambusrc -> Studio chain keeps
// working byte-for-byte regardless of how we got the JPEG bytes.
//
// Exactly one implementation lives behind this header:
//
//   * LibavPipeline -- default. Custom RTSP / RTSPS client (no
//                      libavformat) + libavcodec H.264 decode +
//                      libswscale + stb_image_write JPEG encode.
//                      Selected when -DOBN_VIDEO_BACKEND=ffmpeg.
//
// `make_video_pipeline` is provided by that translation unit. A
// "none" build (-DOBN_VIDEO_BACKEND=none) selects video_pipeline_none.cpp
// instead, which returns nullptr from make_video_pipeline so RTSPS
// Open() fails with a clear message; the MJPG path on port 6000
// (A1/P1) and the file browser keep working.
//
// (A GStreamer backend used to live here too -- rtspsrc + decodebin
// + jpegenc -- but it always aborted inside Studio's AppImage with
// a libjpeg ABI clash we could not fix from outside Studio's bundle,
// so it was dropped once the FFmpeg one stabilised.)
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "source_log.hpp"

namespace obn::video {

// Which RTSP variant we are speaking. "rtsps" is TLS-wrapped (port 322
// on Bambu firmware), "rtsp" is the plain-text equivalent (port 554)
// that nobody actually ships but Studio still allows in URL form.
enum class Scheme { Rtsps, Rtsp };

struct StartConfig {
    Scheme       scheme = Scheme::Rtsps;
    std::string  host;
    int          port = 322;
    std::string  user = "bblp";
    std::string  passwd;
    // Always "/streaming/live/1" on Bambu firmware, but parameterised
    // so a future protocol revision (or a unit test against a fake
    // RTSP server) can override it without touching the backend.
    std::string  path = "/streaming/live/1";
    // Frame budget for the transcoder. The H.264 source is 1080p but
    // Studio's camera widget renders much smaller, so we downscale to
    // 720p MJPG by default - same trade-off the GStreamer pipeline
    // has always made.
    int          target_width  = 1280;
    int          target_height = 720;
    int          target_fps    = 30;
};

// One JPEG frame handed back to the caller. `jpeg` is borrowed: it
// points into storage owned by the pipeline and is invalidated by the
// next try_pull() call (same contract gstbambusrc has with us via
// Bambu_Sample::buffer).
struct Frame {
    const std::uint8_t* jpeg     = nullptr;
    std::size_t         size     = 0;
    int                 width    = 0;
    int                 height   = 0;
    // "decode_time" in 100 ns units. We stamp each frame with the
    // wall-clock moment we pulled it (rather than the upstream RTP
    // timestamp) so Studio's own clock-synchronised sink renders
    // immediately instead of trying to replay at the upstream cadence
    // - which used to look like "video plays at half speed" if the
    // RTP latency window was tight.
    std::uint64_t       dt_100ns = 0;
    // 0 normally; set to f_sync (1) on keyframes if the backend has
    // that information cheaply (helps avoid a glitch when Studio
    // reconnects mid-stream).
    int                 flags    = 0;
};

// Return values mirror Bambu_Error so the caller can pass them
// straight back through Bambu_ReadSample.
enum PullResult {
    Pull_Ok          = 0, // Bambu_success
    Pull_StreamEnd   = 1, // Bambu_stream_end
    Pull_WouldBlock  = 2, // Bambu_would_block
    Pull_Error       = -1,
};

class IVideoPipeline {
public:
    virtual ~IVideoPipeline() = default;

    // Build the pipeline, dial the printer, wait for first preroll.
    // Returns 0 on success (-1 otherwise; see source_log::get_last_error).
    // Synchronous -- the calling thread will block during connect /
    // SDP / first keyframe.
    virtual int start(const StartConfig& cfg) = 0;

    // Pull next frame. Non-blocking: returns Pull_WouldBlock when no
    // sample is ready yet (the C-ABI contract sleeps 33 ms between
    // would-block returns, so a tight spin is fine). Pull_Ok fills
    // *out with a borrowed pointer valid until the next try_pull().
    virtual PullResult try_pull(Frame* out) = 0;

    // Idempotent. After stop() the pipeline is dead; call start()
    // again on a fresh instance to reconnect.
    virtual void stop() = 0;
};

// Backend factory. Implemented by exactly one of:
//   stubs/video_pipeline_libav.cpp  (default)
//   stubs/video_pipeline_none.cpp   (returns nullptr, RTSPS unsupported)
//
// `logger`/`log_ctx` are forwarded to the per-tunnel Logger that
// Studio set via Bambu_SetLogger; pass them as-is.
std::unique_ptr<IVideoPipeline> make_video_pipeline(
    obn::source::Logger logger, void* log_ctx);

// Human-readable name of the backend that was compiled in. Used in
// boot logs so it's obvious which decoder is responsible when
// diagnosing camera issues.
const char* backend_name();

} // namespace obn::video
