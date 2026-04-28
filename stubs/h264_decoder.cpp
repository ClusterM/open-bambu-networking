#include "h264_decoder.hpp"

#include "ffmpeg_dyn.hpp"
#include "image_io.hpp"
#include "source_log.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace obn::video {

namespace {

using obn::ffmpeg_dyn::AvApi;
using obn::source::Logger;
using obn::source::log_at;
using obn::source::log_fmt;
using obn::source::set_last_error;
using obn::source::LL_DEBUG;
using obn::source::LL_TRACE;

// Crank libav's stderr noise down once, the first time any decoder
// instance starts. Honours OBN_AV_LOG_LEVEL the same way the legacy
// FFmpeg pipeline did so existing diagnostic recipes still work.
void av_init_once(const AvApi* av)
{
    static std::once_flag once;
    std::call_once(once, [av]() {
        int lvl = AV_LOG_WARNING;
        if (const char* env = std::getenv("OBN_AV_LOG_LEVEL")) {
            int n = std::atoi(env);
            if (n > 0) lvl = n;
        }
        av->av_log_set_level(lvl);
    });
}

// Annex-B start code we prepend to every NAL fed into the decoder.
constexpr std::uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};

// JPEG quality knob; q=80 mirrors what the legacy GStreamer-backed
// pipeline produced (jpegenc default), and turns out to be a fine
// trade-off for live camera at 720p. Override via OBN_JPEG_QUALITY
// (1..100) for diagnostic captures.
int read_jpeg_quality()
{
    int q = 80;
    if (const char* env = std::getenv("OBN_JPEG_QUALITY")) {
        int n = std::atoi(env);
        if (n >= 1 && n <= 100) q = n;
    }
    return q;
}

} // namespace

struct H264Decoder::Impl {
    Logger logger;
    void*  log_ctx;

    const AvApi*    av       = nullptr;
    AVCodecContext* dec_ctx  = nullptr;
    SwsContext*     sws_ctx  = nullptr;

    AVFrame*  scratch_yuv  = nullptr; // decoder output (borrow)
    AVPacket* dec_pkt      = nullptr; // reused for send_packet

    int target_w   = 1280;
    int target_h   = 720;
    int target_fps = 30;
    int jpeg_q     = 80;

    // Cached source format; rebuilds sws_ctx whenever the decoder
    // produces a frame whose geometry / pixfmt differ.
    int src_w   = 0;
    int src_h   = 0;
    int src_fmt = -1;

    // Reusable buffer that backs the AVPacket we feed the decoder.
    // Holds Annex-B prefix + NAL payload + AV_INPUT_BUFFER_PADDING_SIZE
    // bytes of zero padding (libavcodec may overread the buffer end
    // by up to that many bytes during bitstream parsing).
    std::vector<std::uint8_t> dec_buf;

    // Tightly-packed RGB888 surface that sws_scale writes into. We
    // do not use an AVFrame for the destination because:
    //   * sws_scale accepts raw uint8_t*[] + stride[] just fine, and
    //   * stb_image_write's JPEG encoder needs *tightly* packed
    //     pixels (no row stride parameter), which av_frame_get_buffer
    //     does not guarantee (it aligns rows to 32/64 bytes for SIMD).
    // Sized to target_w * target_h * 3 once on geometry change.
    std::vector<std::uint8_t> rgb_buf;

    Impl(Logger l, void* c) : logger(l ? l : obn::source::noop_logger), log_ctx(c) {}

    int alloc_codec_objects(const AVCodec* dec)
    {
        dec_ctx = av->avcodec_alloc_context3(dec);
        scratch_yuv = av->av_frame_alloc();
        dec_pkt = av->av_packet_alloc();
        if (!dec_ctx || !scratch_yuv || !dec_pkt) {
            set_last_error("h264: av_*_alloc failed");
            return -1;
        }
        return 0;
    }

    int prime_decoder_with(const std::vector<std::uint8_t>& nalu)
    {
        if (nalu.empty()) return 0;
        return feed_nalu(nalu);
    }

    int feed_nalu(const std::vector<std::uint8_t>& nalu)
    {
        // Build Annex-B packet: <00 00 00 01> + NAL + padding.
        dec_buf.clear();
        dec_buf.reserve(4 + nalu.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        dec_buf.insert(dec_buf.end(), kStartCode, kStartCode + 4);
        dec_buf.insert(dec_buf.end(), nalu.begin(), nalu.end());
        // Pad with zeros so libavcodec's bitstream readers may
        // safely overread up to AV_INPUT_BUFFER_PADDING_SIZE bytes.
        dec_buf.resize(dec_buf.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);

        // Reuse the heap packet but point it at our buffer; libavcodec
        // copies the data internally during avcodec_send_packet (the
        // packet becomes refcounted inside the decoder), so the
        // backing vector is safe to mutate on the next call.
        dec_pkt->data  = dec_buf.data();
        dec_pkt->size  = static_cast<int>(4 + nalu.size());
        dec_pkt->flags = 0;

        int rc = av->avcodec_send_packet(dec_ctx, dec_pkt);
        // Detach our buffer pointer so av_packet_free below does not
        // try to free it (it was never owned by libav).
        dec_pkt->data = nullptr;
        dec_pkt->size = 0;
        return rc;
    }

    // Returns 0 if we wrote a JPEG, 1 if no frame was ready, -1 on error.
    int drain_and_encode(std::vector<std::uint8_t>* jpeg)
    {
        bool produced = false;
        for (;;) {
            int rc = av->avcodec_receive_frame(dec_ctx, scratch_yuv);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc < 0) {
                char err[128]{};
                av->av_strerror(rc, err, sizeof(err));
                log_at(LL_DEBUG, logger, log_ctx,
                       "h264: receive_frame: %s", err);
                return -1;
            }

            // (Re)build scaler if source geometry / pixfmt changed.
            if (!sws_ctx ||
                src_w   != scratch_yuv->width  ||
                src_h   != scratch_yuv->height ||
                src_fmt != scratch_yuv->format) {
                if (sws_ctx) av->sws_freeContext(sws_ctx);
                src_w   = scratch_yuv->width;
                src_h   = scratch_yuv->height;
                src_fmt = scratch_yuv->format;
                sws_ctx = av->sws_getContext(
                    src_w, src_h, static_cast<AVPixelFormat>(src_fmt),
                    target_w, target_h, AV_PIX_FMT_RGB24,
                    SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!sws_ctx) {
                    log_fmt(logger, log_ctx,
                            "h264: sws_getContext failed (%dx%d fmt=%d -> %dx%d RGB24)",
                            src_w, src_h, src_fmt, target_w, target_h);
                    set_last_error("h264: sws_getContext failed");
                    return -1;
                }
                rgb_buf.assign(static_cast<std::size_t>(target_w) * target_h * 3, 0);
                log_fmt(logger, log_ctx,
                        "h264: scaler %dx%d (fmt=%d) -> %dx%d RGB24",
                        src_w, src_h, src_fmt, target_w, target_h);
            }

            // sws_scale into our tightly-packed RGB buffer. The dst
            // pointer + stride form is the lowest-level API and
            // imposes no alignment requirements on the destination.
            std::uint8_t* dst[4]    = { rgb_buf.data(), nullptr, nullptr, nullptr };
            int           stride[4] = { target_w * 3,   0,       0,       0       };
            av->sws_scale(sws_ctx,
                          scratch_yuv->data, scratch_yuv->linesize,
                          0, src_h,
                          dst, stride);

            // Newest JPEG wins (drop-on-stall mirroring the gstbambusrc
            // shape upstream): keep overwriting *jpeg if the decoder
            // hands us multiple frames in one call.
            if (!obn::image::encode_jpeg(rgb_buf.data(),
                                         static_cast<std::uint32_t>(target_w),
                                         static_cast<std::uint32_t>(target_h),
                                         jpeg_q,
                                         jpeg)) {
                log_at(LL_DEBUG, logger, log_ctx,
                       "h264: stb jpeg encode failed");
                continue;
            }
            produced = true;
        }
        return produced ? 0 : 1;
    }
};

H264Decoder::H264Decoder(obn::source::Logger logger, void* log_ctx)
    : impl_(std::make_unique<Impl>(logger, log_ctx)) {}

H264Decoder::~H264Decoder() { close(); }

int H264Decoder::open(const obn::rtsp::H264Track& track,
                      int target_w, int target_h, int target_fps)
{
    if (!obn::ffmpeg_dyn::ensure_loaded()) {
        char err[256];
        std::snprintf(err, sizeof(err),
                      "h264: ffmpeg_dyn unavailable: %s",
                      obn::ffmpeg_dyn::last_load_error());
        set_last_error(err);
        return -1;
    }
    auto& I = *impl_;
    I.av         = obn::ffmpeg_dyn::api();
    I.target_w   = target_w  > 0 ? target_w  : 1280;
    I.target_h   = target_h  > 0 ? target_h  : 720;
    I.target_fps = target_fps > 0 ? target_fps : 30;
    I.jpeg_q     = read_jpeg_quality();

    av_init_once(I.av);

    const AVCodec* dec = I.av->avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!dec) {
        set_last_error("h264: no H.264 decoder in libavcodec");
        return -1;
    }
    if (I.alloc_codec_objects(dec) != 0) return -1;

    // Decoder context: low_delay + single thread so each AU we feed
    // turns into one display frame on the next receive_frame call.
    // Multi-threaded decoding would queue up frames, which our caller
    // (a single mailbox) has no use for and would just drop.
    I.dec_ctx->flags        |= AV_CODEC_FLAG_LOW_DELAY;
    I.dec_ctx->thread_count  = 1;
    if (I.av->avcodec_open2(I.dec_ctx, dec, nullptr) < 0) {
        set_last_error("h264: avcodec_open2 (decoder) failed");
        return -1;
    }

    // Prime the decoder with the SPS / PPS we already pulled from the
    // SDP. Either may legitimately be empty if the printer chose to
    // send them in-band only; the decoder catches up on the first
    // IDR slice in that case.
    if (!track.sps.empty()) (void)I.feed_nalu(track.sps);
    if (!track.pps.empty()) (void)I.feed_nalu(track.pps);

    log_fmt(I.logger, I.log_ctx,
            "h264: decoder ready (target=%dx%d@%dfps, jpeg_q=%d, sps=%zuB pps=%zuB)",
            I.target_w, I.target_h, I.target_fps, I.jpeg_q,
            track.sps.size(), track.pps.size());
    return 0;
}

int H264Decoder::decode(const std::vector<std::uint8_t>& nalu,
                        std::vector<std::uint8_t>* jpeg)
{
    auto& I = *impl_;
    jpeg->clear();
    if (!I.dec_ctx) {
        set_last_error("h264: decode() before open()");
        return -1;
    }
    if (nalu.empty()) return 0;

    int rc = I.feed_nalu(nalu);
    if (rc < 0 && rc != AVERROR(EAGAIN)) {
        // EAGAIN means "decoder full, drain first" -- safe to fall
        // through and try receive_frame; anything else is a real error.
        char err[128]{};
        I.av->av_strerror(rc, err, sizeof(err));
        log_at(LL_TRACE, I.logger, I.log_ctx,
               "h264: send_packet: %s (continuing)", err);
    }

    int dr = I.drain_and_encode(jpeg);
    if (dr < 0) return -1;
    return 0;
}

void H264Decoder::close()
{
    if (!impl_) return;
    auto& I = *impl_;
    if (!I.av) return;
    if (I.sws_ctx)     { I.av->sws_freeContext(I.sws_ctx);     I.sws_ctx     = nullptr; }
    if (I.dec_pkt)     I.av->av_packet_free(&I.dec_pkt);
    if (I.scratch_yuv) I.av->av_frame_free(&I.scratch_yuv);
    if (I.dec_ctx)     I.av->avcodec_free_context(&I.dec_ctx);
    I.av = nullptr;
}

int H264Decoder::target_width()  const noexcept { return impl_->target_w; }
int H264Decoder::target_height() const noexcept { return impl_->target_h; }

} // namespace obn::video
