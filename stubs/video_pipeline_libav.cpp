// IVideoPipeline backed by a custom RTSP client + libavcodec.
//
// This replaces the previous libavformat-based pipeline. The protocol
// (RTSP / RTSPS / TLS / RTP-over-TCP / H.264 NAL reassembly) is handled
// by stubs/rtsp_client.cpp; the H.264 decode + libswscale + MJPEG
// re-encode by stubs/h264_decoder.cpp; and we are the glue that drives
// them on a worker thread and exposes the IVideoPipeline contract
// upstream consumers (BambuSource.cpp on Linux/Windows, BambuPlayer.mm
// on macOS) already speak.
//
// The reason we no longer use libavformat is the libavutil ABI clash
// hidden inside Bambu Studio's AppImage: the bundle ships an older
// libavutil.so.59 than the system libavformat.so.61 in /usr was built
// against (av_mastering_display_metadata_alloc_size missing from the
// older libavutil). Trying to dlopen / dlmopen system libavformat in a
// process where the AppImage's libavutil was already loaded fails the
// symbol-versioning check no matter how isolated the namespace is. So
// we drop libavformat entirely, talk RTSP ourselves, and link only
// against the libav* objects Studio's AppImage already has loaded
// (libavcodec / libavutil / libswscale, all resolved through
// dlsym(RTLD_DEFAULT, ...) -- see stubs/ffmpeg_dyn.cpp).
//
// Threading mirrors what the ffmpeg pipeline did: one producer thread
// drives the rtsp_client read loop and feeds each NAL to the decoder.
// Each successfully encoded JPEG lands in a single-slot mailbox the
// C-ABI consumer drains via try_pull(); a stale frame is overwritten
// rather than queued (drop-on-stall, so a slow Studio sink does not
// pile up backlog).

#include "h264_decoder.hpp"
#include "rtsp_client.hpp"
#include "source_log.hpp"
#include "video_pipeline.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using obn::source::log_at;
using obn::source::log_fmt;
using obn::source::Logger;
using obn::source::set_last_error;
using obn::source::LL_DEBUG;
using obn::source::LL_TRACE;

class LibavPipeline final : public obn::video::IVideoPipeline {
public:
    LibavPipeline(Logger logger, void* log_ctx)
        : logger_(logger ? logger : obn::source::noop_logger),
          log_ctx_(log_ctx) {}

    ~LibavPipeline() override { stop(); }

    int start(const obn::video::StartConfig& cfg) override
    {
        if (started_.exchange(true)) {
            // Defensive: caller misused the API. Reset and retry.
            stop();
            started_.store(true);
        }
        stop_flag_.store(false, std::memory_order_release);

        target_w_   = cfg.target_width  > 0 ? cfg.target_width  : 1280;
        target_h_   = cfg.target_height > 0 ? cfg.target_height : 720;
        target_fps_ = cfg.target_fps    > 0 ? cfg.target_fps    : 30;

        log_fmt(logger_, log_ctx_,
                "libav: start uri=%s://%s:%d%s user=%s target=%dx%d@%dfps",
                (cfg.scheme == obn::video::Scheme::Rtsps) ? "rtsps" : "rtsp",
                cfg.host.c_str(), cfg.port, cfg.path.c_str(), cfg.user.c_str(),
                target_w_, target_h_, target_fps_);

        rtsp_ = std::make_unique<obn::rtsp::Client>(logger_, log_ctx_);
        obn::rtsp::Url u;
        u.host   = cfg.host;
        u.port   = cfg.port;
        u.user   = cfg.user;
        u.passwd = cfg.passwd;
        u.path   = cfg.path;
        u.tls    = (cfg.scheme == obn::video::Scheme::Rtsps);
        if (rtsp_->start(u) != 0) {
            // last_error already populated by rtsp_client.
            cleanup_after_failed_start();
            return -1;
        }

        decoder_ = std::make_unique<obn::video::H264Decoder>(logger_, log_ctx_);
        if (decoder_->open(rtsp_->track(), target_w_, target_h_, target_fps_) != 0) {
            log_fmt(logger_, log_ctx_,
                    "libav: decoder open failed: %s", obn::source::get_last_error());
            cleanup_after_failed_start();
            return -1;
        }

        t0_      = std::chrono::steady_clock::now();
        worker_  = std::thread(&LibavPipeline::worker_main, this);
        log_fmt(logger_, log_ctx_, "libav: pipeline ready");
        return 0;
    }

    obn::video::PullResult try_pull(obn::video::Frame* out) override
    {
        std::unique_lock<std::mutex> lk(mailbox_mu_);
        if (!ready_) {
            if (worker_done_.load(std::memory_order_acquire)) {
                if (!eos_reported_) {
                    eos_reported_ = true;
                    return obn::video::Pull_StreamEnd;
                }
                return obn::video::Pull_Error;
            }
            return obn::video::Pull_WouldBlock;
        }
        pulled_buf_.swap(mailbox_);
        ready_         = false;
        out->jpeg      = pulled_buf_.data();
        out->size      = pulled_buf_.size();
        out->width     = mailbox_w_;
        out->height    = mailbox_h_;
        out->dt_100ns  = mailbox_dt_;
        out->flags     = mailbox_flags_;
        return obn::video::Pull_Ok;
    }

    void stop() override
    {
        if (!stop_flag_.exchange(true)) {
            // Wake any in-flight rtsp_->read_nalu(). The rtsp client
            // shuts the underlying socket down and breaks SSL_read so
            // the worker can join() promptly.
            if (rtsp_) rtsp_->cancel();
        }
        if (worker_.joinable()) worker_.join();
        if (rtsp_)    { rtsp_->stop();    rtsp_.reset(); }
        if (decoder_) { decoder_->close(); decoder_.reset(); }
        started_.store(false);
    }

private:
    void cleanup_after_failed_start()
    {
        if (rtsp_)    { rtsp_->stop();    rtsp_.reset(); }
        if (decoder_) { decoder_->close(); decoder_.reset(); }
        started_.store(false);
    }

    void worker_main()
    {
        std::uint64_t frame_count = 0;
        std::vector<std::uint8_t> jpeg;

        while (!stop_flag_.load(std::memory_order_acquire)) {
            obn::rtsp::Nalu nu;
            int rc = rtsp_->read_nalu(&nu);
            if (rc != 0) {
                log_at(LL_DEBUG, logger_, log_ctx_,
                       "libav: rtsp read_nalu rc=%d (%s)",
                       rc,
                       rc < 0 ? obn::source::get_last_error() : "EOS");
                break;
            }
            if (decoder_->decode(nu.data, &jpeg) != 0) {
                log_at(LL_DEBUG, logger_, log_ctx_,
                       "libav: decoder hit fatal error");
                break;
            }
            if (jpeg.empty()) continue;

            auto now = std::chrono::steady_clock::now();
            auto ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           now - t0_).count();
            std::uint64_t dt_100ns = static_cast<std::uint64_t>(ns / 100);

            {
                std::lock_guard<std::mutex> lk(mailbox_mu_);
                mailbox_       = jpeg; // copy; producer reuses jpeg next iter
                mailbox_w_     = target_w_;
                mailbox_h_     = target_h_;
                mailbox_dt_    = dt_100ns;
                mailbox_flags_ = nu.au_end ? 1 : 0; // f_sync hint on AU end
                ready_         = true;
            }

            ++frame_count;
            if (frame_count == 1 || (frame_count % 60) == 0) {
                log_fmt(logger_, log_ctx_,
                        "libav: frame #%llu size=%zu",
                        static_cast<unsigned long long>(frame_count),
                        jpeg.size());
            }
        }
        worker_done_.store(true, std::memory_order_release);
        log_at(LL_DEBUG, logger_, log_ctx_, "libav: worker exited");
    }

    Logger logger_;
    void*  log_ctx_;

    std::unique_ptr<obn::rtsp::Client>     rtsp_;
    std::unique_ptr<obn::video::H264Decoder> decoder_;

    int target_w_   = 1280;
    int target_h_   = 720;
    int target_fps_ = 30;

    std::chrono::steady_clock::time_point t0_{};

    std::thread       worker_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> worker_done_{false};
    std::atomic<bool> started_{false};

    std::mutex                mailbox_mu_;
    std::vector<std::uint8_t> mailbox_;
    std::vector<std::uint8_t> pulled_buf_;
    int                       mailbox_w_     = 0;
    int                       mailbox_h_     = 0;
    std::uint64_t             mailbox_dt_    = 0;
    int                       mailbox_flags_ = 0;
    bool                      ready_         = false;
    bool                      eos_reported_  = false;
};

} // namespace

namespace obn::video {

std::unique_ptr<IVideoPipeline> make_video_pipeline(
    obn::source::Logger logger, void* log_ctx)
{
    return std::make_unique<LibavPipeline>(logger, log_ctx);
}

// Same name as before so existing log lines like
// "ff: backend=ffmpeg" stay grep-able. The implementation switched
// from libavformat to a custom RTSP client + libavcodec, but from
// the user's perspective it is still "the FFmpeg backend".
const char* backend_name() { return "ffmpeg"; }

} // namespace obn::video
