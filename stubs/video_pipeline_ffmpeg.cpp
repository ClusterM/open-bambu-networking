// IVideoPipeline backed by FFmpeg / libav*.
//
// Pulls H.264 over RTSP(S), decodes it with libavcodec, downscales to
// 720p with libswscale, and re-encodes each frame as a baseline JPEG
// using libavcodec's MJPEG encoder. The C-ABI in BambuSource.cpp then
// hands those JPEG bytes to gstbambusrc / wxMediaCtrl2 unchanged, so
// the rest of Studio sees an A1/P1-style camera regardless of which
// printer is on the other end.
//
// The pipeline runs on a dedicated producer thread because libav's
// av_read_frame is blocking and we need try_pull() to return promptly
// from the C-ABI's polling loop. Frames hit a single-slot mailbox: a
// new frame overwrites the previous one if Studio hasn't picked it up
// yet (drop-on-stall, matches the gstbambusrc gst-pipeline shape that
// used `leaky=downstream max-buffers=2 drop=true`).
//
// Cancellation: stop() sets `stop_flag_`, which the AVIOInterruptCB
// honours so any in-flight av_read_frame returns immediately, then
// joins the worker and tears down the AV* contexts. It is cheap to
// destroy a pipeline that never produced a frame.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "video_pipeline.hpp"

namespace {

using obn::source::log_at;
using obn::source::log_fmt;
using obn::source::Logger;
using obn::source::LL_DEBUG;
using obn::source::LL_TRACE;
using obn::source::set_last_error;

// Demote libav's own log spam to mirror_log_fp at WARNING (== AV_LOG_WARNING)
// so the user's stderr stays clean. Real failures still come through
// at AV_LOG_ERROR level.
void av_log_to_mirror(void* /*avcl*/, int level,
                      const char* fmt, va_list vl)
{
    if (level > AV_LOG_WARNING) return;
    FILE* fp = obn::source::mirror_log_fp();
    if (!fp) return;
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), fmt, vl);
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%F %T", std::localtime(&tt));
    // libav messages already end with \n -- strip the duplicate.
    std::size_t n = std::strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    const char* tag = (level <= AV_LOG_ERROR)   ? "ERROR"
                    : (level <= AV_LOG_WARNING) ? "WARN"
                                                : "INFO";
    std::fprintf(fp, "%s [av %s] %s\n", ts, tag, buf);
}

void av_init_once()
{
    static std::once_flag once;
    std::call_once(once, []() {
        // libav's auto-init covers both the global state and the
        // network protocols (avformat_network_init is a counter, no
        // teardown required).
        avformat_network_init();
        // Crank back libav's own stderr noise unless OBN_AV_LOG_LEVEL
        // raises it. AV_LOG_WARNING is the same threshold the gst
        // backend uses.
        int lvl = AV_LOG_WARNING;
        if (const char* env = std::getenv("OBN_AV_LOG_LEVEL")) {
            int n = std::atoi(env);
            if (n > 0) lvl = n;
        }
        av_log_set_level(lvl);
        av_log_set_callback(av_log_to_mirror);
    });
}

// Turns "{user}", "{passwd}", "{host}", "{port}", "{path}" into the
// canonical FFmpeg URL. user/password is encoded into the userinfo of
// the URL because libavformat's RTSP demuxer reads credentials from
// there (the alternative -- AVDictionary "user"/"pass" keys -- is
// only honoured by the HTTP demuxer in modern libav releases).
//
// We do not URL-encode anything: Bambu LAN access codes are 8 digits,
// usernames are "bblp", and the host is an IP literal. Anything else
// would be a misuse worth surfacing rather than silently mangling.
std::string build_ffmpeg_uri(const obn::video::StartConfig& cfg)
{
    const char* scheme = (cfg.scheme == obn::video::Scheme::Rtsps) ? "rtsps" : "rtsp";
    std::string uri = scheme;
    uri += "://";
    if (!cfg.user.empty()) {
        uri += cfg.user;
        if (!cfg.passwd.empty()) {
            uri += ":";
            uri += cfg.passwd;
        }
        uri += "@";
    }
    uri += cfg.host;
    uri += ":";
    uri += std::to_string(cfg.port);
    uri += cfg.path;
    return uri;
}

class FfmpegPipeline final : public obn::video::IVideoPipeline {
public:
    FfmpegPipeline(Logger logger, void* log_ctx)
        : logger_(logger ? logger : obn::source::noop_logger), log_ctx_(log_ctx) {}

    ~FfmpegPipeline() override { stop(); }

    int start(const obn::video::StartConfig& cfg) override
    {
        av_init_once();

        if (worker_started_.exchange(true)) {
            // Defensive: a second start() on the same object is a
            // misuse. Tear down first.
            stop();
            worker_started_.store(true);
        }

        const std::string uri = build_ffmpeg_uri(cfg);
        log_fmt(logger_, log_ctx_,
                "ff: start uri=%s://%s:%d%s user=%s",
                (cfg.scheme == obn::video::Scheme::Rtsps) ? "rtsps" : "rtsp",
                cfg.host.c_str(), cfg.port, cfg.path.c_str(),
                cfg.user.c_str());

        target_w_   = cfg.target_width;
        target_h_   = cfg.target_height;
        target_fps_ = cfg.target_fps > 0 ? cfg.target_fps : 30;

        fmt_ctx_ = avformat_alloc_context();
        if (!fmt_ctx_) {
            set_last_error("avformat_alloc_context failed");
            return -1;
        }

        // Honour stop_flag_ from inside av_read_frame / avformat_open_input.
        // The callback is polled by libav at every blocking point, so
        // a stop() that sets the flag wakes us up promptly instead of
        // having to wait for read timeouts.
        fmt_ctx_->interrupt_callback.callback = &FfmpegPipeline::interrupt_cb;
        fmt_ctx_->interrupt_callback.opaque   = this;

        AVDictionary* opts = nullptr;
        // Force TCP: Bambu firmware exposes RTSP-over-TCP only (UDP
        // is filtered) and the default UDP-then-TCP fallback wastes
        // 5 s per session. Same property the gst backend sets on
        // rtspsrc (protocols=4 == TCP).
        av_dict_set(&opts, "rtsp_transport", "tcp", 0);
        // Self-signed printer cert: don't validate.
        av_dict_set(&opts, "tls_verify", "0", 0);
        // 5 s connect timeout, 8 s socket timeout (microseconds).
        // Keeps a dead printer from hanging Bambu_Open forever.
        av_dict_set(&opts, "stimeout",  "5000000", 0);
        av_dict_set(&opts, "rw_timeout", "8000000", 0);
        // We don't care about RTCP receiver-report timing.
        av_dict_set(&opts, "max_delay", "120000", 0); // 120 ms in us
        av_dict_set(&opts, "fflags", "nobuffer+discardcorrupt", 0);
        av_dict_set(&opts, "flags",  "low_delay", 0);

        int rc = avformat_open_input(&fmt_ctx_, uri.c_str(), nullptr, &opts);
        av_dict_free(&opts);
        if (rc < 0) {
            char errbuf[128]{};
            av_strerror(rc, errbuf, sizeof(errbuf));
            log_fmt(logger_, log_ctx_,
                    "ff: avformat_open_input failed: %s (%d)", errbuf, rc);
            set_last_error(errbuf);
            return -1;
        }

        rc = avformat_find_stream_info(fmt_ctx_, nullptr);
        if (rc < 0) {
            char errbuf[128]{};
            av_strerror(rc, errbuf, sizeof(errbuf));
            log_fmt(logger_, log_ctx_,
                    "ff: avformat_find_stream_info failed: %s (%d)",
                    errbuf, rc);
            set_last_error(errbuf);
            return -1;
        }

        video_stream_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO,
                                            -1, -1, nullptr, 0);
        if (video_stream_ < 0) {
            log_fmt(logger_, log_ctx_, "ff: no video stream in SDP");
            set_last_error("no video stream");
            return -1;
        }

        AVStream* st = fmt_ctx_->streams[video_stream_];
        const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) {
            log_fmt(logger_, log_ctx_,
                    "ff: no decoder for codec_id=%d", st->codecpar->codec_id);
            set_last_error("no decoder");
            return -1;
        }
        log_fmt(logger_, log_ctx_,
                "ff: using decoder '%s' (codec_id=%d)",
                dec->name, st->codecpar->codec_id);

        dec_ctx_ = avcodec_alloc_context3(dec);
        if (!dec_ctx_) {
            set_last_error("avcodec_alloc_context3 (dec) failed");
            return -1;
        }
        if (avcodec_parameters_to_context(dec_ctx_, st->codecpar) < 0) {
            set_last_error("avcodec_parameters_to_context failed");
            return -1;
        }
        // Match the gst pipeline's "low_delay" stance: each AU we feed
        // the decoder corresponds to one display frame, no need for
        // multi-threaded reorder buffers.
        dec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        dec_ctx_->thread_count = 1;
        if (avcodec_open2(dec_ctx_, dec, nullptr) < 0) {
            set_last_error("avcodec_open2 (dec) failed");
            return -1;
        }

        // MJPEG encoder. Quality is set via the qscale knob, with the
        // same trade-off the gst backend made (q=80 there ~= qscale=4
        // here on the standard MJPEG library quantiser table).
        const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        if (!enc) {
            log_fmt(logger_, log_ctx_, "ff: no MJPEG encoder available");
            set_last_error("no MJPEG encoder");
            return -1;
        }
        enc_ctx_ = avcodec_alloc_context3(enc);
        if (!enc_ctx_) {
            set_last_error("avcodec_alloc_context3 (enc) failed");
            return -1;
        }
        enc_ctx_->width      = target_w_;
        enc_ctx_->height     = target_h_;
        // YUVJ420P is the historical "JPEG full-range" pix_fmt the
        // MJPEG encoder accepts on every libav version we care about.
        // Equivalent on FFmpeg 6+ would be YUV420P + color_range=JPEG,
        // but YUVJ420P is the lowest-common-denominator that doesn't
        // need a separate range fixup pass.
        enc_ctx_->pix_fmt    = AV_PIX_FMT_YUVJ420P;
        enc_ctx_->color_range = AVCOL_RANGE_JPEG;
        enc_ctx_->time_base  = AVRational{1, target_fps_};
        enc_ctx_->framerate  = AVRational{target_fps_, 1};
        enc_ctx_->flags     |= AV_CODEC_FLAG_QSCALE;
        enc_ctx_->global_quality = FF_QP2LAMBDA * 4;
        if (avcodec_open2(enc_ctx_, enc, nullptr) < 0) {
            set_last_error("avcodec_open2 (enc) failed");
            return -1;
        }

        // Scaler: source pixel format isn't known until the first
        // decoded frame (fmt_ctx_->streams[i]->codecpar->format is the
        // SDP value, which is often AV_PIX_FMT_NONE for H.264). We
        // build sws_ctx_ lazily inside the worker loop on the first
        // frame and reuse it afterwards.

        t0_ = std::chrono::steady_clock::now();
        worker_ = std::thread(&FfmpegPipeline::worker_main, this);
        log_fmt(logger_, log_ctx_,
                "ff: pipeline ready (target=%dx%d@%dfps)",
                target_w_, target_h_, target_fps_);
        return 0;
    }

    obn::video::PullResult try_pull(obn::video::Frame* out) override
    {
        std::unique_lock<std::mutex> lk(mailbox_mu_);
        if (!ready_) {
            // EOF/fatal signalling: when the worker has shut down with
            // an end-of-stream the consumer should see Pull_StreamEnd
            // exactly once, then Pull_Error on subsequent calls so the
            // C-ABI tears the tunnel down cleanly.
            if (worker_done_.load(std::memory_order_acquire)) {
                if (!eos_reported_) {
                    eos_reported_ = true;
                    return obn::video::Pull_StreamEnd;
                }
                return obn::video::Pull_Error;
            }
            return obn::video::Pull_WouldBlock;
        }
        // Hand back the borrowed buffer; reset the mailbox flag so the
        // next ready frame replaces it. The buffer storage is the
        // pipeline's `pulled_buf_` which stays valid until try_pull
        // is called again.
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
        stop_flag_.store(true, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
        if (sws_ctx_)   { sws_freeContext(sws_ctx_); sws_ctx_   = nullptr; }
        if (enc_ctx_)   avcodec_free_context(&enc_ctx_);
        if (dec_ctx_)   avcodec_free_context(&dec_ctx_);
        if (fmt_ctx_)   avformat_close_input(&fmt_ctx_);
    }

private:
    static int interrupt_cb(void* opaque)
    {
        auto* self = static_cast<FfmpegPipeline*>(opaque);
        return self->stop_flag_.load(std::memory_order_acquire) ? 1 : 0;
    }

    void worker_main()
    {
        AVPacket* pkt = av_packet_alloc();
        AVPacket* enc_pkt = av_packet_alloc();
        AVFrame*  frame   = av_frame_alloc();
        AVFrame*  scaled  = av_frame_alloc();
        if (!pkt || !enc_pkt || !frame || !scaled) {
            log_fmt(logger_, log_ctx_, "ff: av_alloc failed in worker");
            if (pkt)     av_packet_free(&pkt);
            if (enc_pkt) av_packet_free(&enc_pkt);
            if (frame)   av_frame_free(&frame);
            if (scaled)  av_frame_free(&scaled);
            worker_done_.store(true, std::memory_order_release);
            return;
        }

        // Pre-allocate scaled YUVJ420P at target dimensions.
        scaled->format = AV_PIX_FMT_YUVJ420P;
        scaled->width  = target_w_;
        scaled->height = target_h_;
        if (av_frame_get_buffer(scaled, 32) < 0) {
            log_fmt(logger_, log_ctx_, "ff: av_frame_get_buffer failed");
            av_packet_free(&pkt);
            av_packet_free(&enc_pkt);
            av_frame_free(&frame);
            av_frame_free(&scaled);
            worker_done_.store(true, std::memory_order_release);
            return;
        }

        std::uint64_t frame_count = 0;
        std::int64_t  enc_pts     = 0;

        while (!stop_flag_.load(std::memory_order_acquire)) {
            int rc = av_read_frame(fmt_ctx_, pkt);
            if (rc < 0) {
                if (rc == AVERROR(EAGAIN)) {
                    // Spurious wake-up from interrupt_cb; keep going.
                    av_packet_unref(pkt);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                if (rc == AVERROR_EXIT) {
                    log_at(LL_DEBUG, logger_, log_ctx_,
                           "ff: av_read_frame interrupted (stop requested)");
                    break;
                }
                char errbuf[128]{};
                av_strerror(rc, errbuf, sizeof(errbuf));
                log_fmt(logger_, log_ctx_,
                        "ff: av_read_frame: %s (%d)", errbuf, rc);
                break;
            }
            if (pkt->stream_index != video_stream_) {
                av_packet_unref(pkt);
                continue;
            }

            // Decode.
            rc = avcodec_send_packet(dec_ctx_, pkt);
            av_packet_unref(pkt);
            if (rc < 0) {
                char errbuf[128]{};
                av_strerror(rc, errbuf, sizeof(errbuf));
                log_at(LL_DEBUG, logger_, log_ctx_,
                       "ff: avcodec_send_packet: %s", errbuf);
                continue;
            }

            while (true) {
                rc = avcodec_receive_frame(dec_ctx_, frame);
                if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
                if (rc < 0) {
                    char errbuf[128]{};
                    av_strerror(rc, errbuf, sizeof(errbuf));
                    log_at(LL_DEBUG, logger_, log_ctx_,
                           "ff: avcodec_receive_frame: %s", errbuf);
                    break;
                }

                // Build / refresh the scaler if this is the first frame
                // or the source dimensions changed (would be unusual on
                // a live camera but cheap to handle).
                if (!sws_ctx_ || src_w_ != frame->width ||
                    src_h_ != frame->height || src_fmt_ != frame->format) {
                    if (sws_ctx_) sws_freeContext(sws_ctx_);
                    src_w_   = frame->width;
                    src_h_   = frame->height;
                    src_fmt_ = frame->format;
                    sws_ctx_ = sws_getContext(
                        src_w_, src_h_, static_cast<AVPixelFormat>(src_fmt_),
                        target_w_, target_h_, AV_PIX_FMT_YUVJ420P,
                        SWS_BILINEAR, nullptr, nullptr, nullptr);
                    if (!sws_ctx_) {
                        log_fmt(logger_, log_ctx_,
                                "ff: sws_getContext failed (%dx%d->%dx%d, fmt=%d)",
                                src_w_, src_h_, target_w_, target_h_, src_fmt_);
                        break;
                    }
                    log_fmt(logger_, log_ctx_,
                            "ff: scaler %dx%d (fmt=%d) -> %dx%d YUVJ420P",
                            src_w_, src_h_, src_fmt_,
                            target_w_, target_h_);
                }

                if (av_frame_make_writable(scaled) < 0) break;
                sws_scale(sws_ctx_,
                          frame->data, frame->linesize, 0, src_h_,
                          scaled->data, scaled->linesize);
                scaled->pts = enc_pts++;

                // Encode to JPEG.
                rc = avcodec_send_frame(enc_ctx_, scaled);
                if (rc < 0) {
                    char errbuf[128]{};
                    av_strerror(rc, errbuf, sizeof(errbuf));
                    log_at(LL_DEBUG, logger_, log_ctx_,
                           "ff: avcodec_send_frame: %s", errbuf);
                    continue;
                }
                while (avcodec_receive_packet(enc_ctx_, enc_pkt) == 0) {
                    auto now = std::chrono::steady_clock::now();
                    auto ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   now - t0_).count();
                    std::uint64_t dt_100ns =
                        static_cast<std::uint64_t>(ns / 100);

                    {
                        std::lock_guard<std::mutex> lk(mailbox_mu_);
                        mailbox_.assign(enc_pkt->data,
                                        enc_pkt->data + enc_pkt->size);
                        mailbox_w_     = target_w_;
                        mailbox_h_     = target_h_;
                        mailbox_dt_    = dt_100ns;
                        mailbox_flags_ = 0;
                        ready_         = true;
                    }
                    av_packet_unref(enc_pkt);

                    if (++frame_count == 1 || (frame_count % 60) == 0) {
                        log_fmt(logger_, log_ctx_,
                                "ff: frame #%llu size=%d",
                                static_cast<unsigned long long>(frame_count),
                                static_cast<int>(mailbox_.size()));
                    }
                }
            }
        }

        // Flush the encoder so any in-flight frame is delivered before
        // we tear down. Best-effort; the caller will also stop polling
        // shortly after stop().
        avcodec_send_frame(enc_ctx_, nullptr);
        while (avcodec_receive_packet(enc_ctx_, enc_pkt) == 0) {
            av_packet_unref(enc_pkt);
        }

        av_packet_free(&pkt);
        av_packet_free(&enc_pkt);
        av_frame_free(&frame);
        av_frame_free(&scaled);
        worker_done_.store(true, std::memory_order_release);
        log_at(LL_DEBUG, logger_, log_ctx_, "ff: worker exited");
    }

    Logger      logger_;
    void*       log_ctx_;

    AVFormatContext* fmt_ctx_ = nullptr;
    AVCodecContext*  dec_ctx_ = nullptr;
    AVCodecContext*  enc_ctx_ = nullptr;
    SwsContext*      sws_ctx_ = nullptr;
    int              video_stream_ = -1;
    int              src_w_   = 0;
    int              src_h_   = 0;
    int              src_fmt_ = -1;
    int              target_w_   = 1280;
    int              target_h_   = 720;
    int              target_fps_ = 30;
    std::chrono::steady_clock::time_point t0_{};

    std::thread        worker_;
    std::atomic<bool>  stop_flag_{false};
    std::atomic<bool>  worker_done_{false};
    std::atomic<bool>  worker_started_{false};

    // Single-slot mailbox. Producer writes the latest JPEG bytes into
    // mailbox_ + mailbox_w_/h_/dt_/flags_ and sets ready_ = true. The
    // consumer swaps it into pulled_buf_ on the next try_pull(). New
    // producer frames overwrite an unread mailbox (drop-on-stall, the
    // C-ABI poll cadence is bounded by gstbambusrc's 33 ms sleep so a
    // half-cooked frame can't queue up).
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
    return std::make_unique<FfmpegPipeline>(logger, log_ctx);
}

const char* backend_name() { return "ffmpeg"; }

} // namespace obn::video
