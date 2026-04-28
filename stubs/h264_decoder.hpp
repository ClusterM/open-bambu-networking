// H.264 decoder + JPEG encoder backing libBambuSource's RTSPS pipeline.
//
// Decode side runs on libavcodec / libavutil / libswscale resolved
// through ffmpeg_dyn (dlsym(RTLD_DEFAULT, ...) against whatever Studio's
// bundle already loaded; see ffmpeg_dyn.hpp). Encode side is *not*
// libavcodec: Bambu Studio's bundled libavcodec is decoder-only, so
// avcodec_find_encoder(MJPEG) returns NULL in-process. We encode each
// frame with stb_image_write (header-only, no NEEDED entries), routed
// through obn::image::encode_jpeg.
//
// The decoder feeds on a stream of raw H.264 NAL units (the kind
// rtsp_client.cpp emits): no length prefix, no Annex-B start code.
// We add the start code internally so libavcodec sees a tidy
// `00 00 00 01 NAL` packet on every send.
//
// SPS / PPS handling: rather than build a baseline AVCodecParameters
// extradata blob (which would require an av_malloc pointer in our
// AvApi we do not currently export), we send the SPS and PPS from
// the SDP `sprop-parameter-sets` field as their own packets right
// after avcodec_open2(). The decoder accumulates them into its
// internal state and then has everything it needs by the time the
// first IDR slice arrives. In-band SPS/PPS work the same way.
//
// JPEG output: every successfully decoded frame is libswscale-resampled
// straight into a tightly-packed RGB24 buffer at the requested target
// dimensions, then handed to stb_image_write's baseline JPEG encoder
// at q=80 (override via OBN_JPEG_QUALITY). The C-ABI consumer ships
// those JPEG bytes straight to Studio's gstbambusrc / wxMediaCtrl2
// consumer without further decoding -- the same byte-level contract
// the legacy GStreamer-backed pipeline implemented before this one.
//
// Threading: a single decode() call may loop receive_frame internally
// until the decoder is empty, but only the *latest* JPEG is returned
// (the upstream pipeline's mailbox drops stale frames anyway). All
// state (decoder context, scaler, scratch RGB buffer) is owned by the
// instance and not safe to share across threads.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "rtsp_client.hpp"
#include "source_log.hpp"

namespace obn::video {

class H264Decoder {
public:
    H264Decoder(obn::source::Logger logger, void* log_ctx);
    ~H264Decoder();

    H264Decoder(const H264Decoder&)            = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    // Initialise the decoder + scaler + JPEG encoder. `track` carries
    // the SDP-derived SPS / PPS / payload type; `target_w` / `target_h`
    // are the dimensions of the JPEG we hand back to Studio (the
    // source frame is resampled into them with libswscale).
    //
    // Returns 0 on success; -1 on failure (with set_last_error()).
    int open(const obn::rtsp::H264Track& track,
             int target_w,
             int target_h,
             int target_fps);

    // Feed one raw NAL unit (no start code prefix). Internally:
    //   * prepends Annex-B and ships to avcodec_send_packet,
    //   * loops avcodec_receive_frame draining what is available,
    //   * for the *last* frame produced (if any), runs sws_scale
    //     into a tightly-packed RGB24 buffer and then encodes it as
    //     baseline JPEG via obn::image::encode_jpeg,
    //   * returns the encoded bytes via *jpeg (cleared first).
    //
    // If no frame popped out (i.e. SPS / PPS / SEI / non-IDR before
    // an IDR has been seen, or a still-buffering decoder), *jpeg
    // ends up empty and the call still returns 0.
    //
    // Return codes:
    //    0 - decode loop ran cleanly, *jpeg either populated or empty
    //   -1 - fatal libav error (decoder unrecoverable)
    int decode(const std::vector<std::uint8_t>& nalu,
               std::vector<std::uint8_t>* jpeg);

    // Idempotent. Releases all libav contexts but leaves the instance
    // re-openable with a different track if desired.
    void close();

    // Geometry of the JPEG we emit (== target_w/target_h passed to
    // open). Useful for stamping Bambu_Sample with width/height.
    int target_width()  const noexcept;
    int target_height() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace obn::video
