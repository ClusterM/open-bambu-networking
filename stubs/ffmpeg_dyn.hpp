// Resolve libavcodec / libavutil / libswscale via the host process's
// own already-loaded copies (RTLD_DEFAULT), without dlopen of system
// libav and without a hard NEEDED entry on libBambuSource.so.
//
// Why this is the *only* loading strategy now:
//
// Bambu Studio's AppImage carries libavutil.so.59 + libavcodec.so.61 +
// libswscale.so.8 in its bin/ directory and DT_NEEDEDs them off the
// main bambu-studio binary, so by the time our plugin runs they are
// already mapped into the process. Their minor versions match each
// other (one consistent FFmpeg build inside the AppImage), which is
// the only ABI guarantee that matters for cross-libav* call traffic.
//
// Earlier versions of this loader tried to dlopen system libav as a
// fallback. That kept failing with
//     /lib/.../libavformat.so.61: undefined symbol:
//     av_mastering_display_metadata_alloc_size, version LIBAVUTIL_59
// because system libavformat was built against a newer libavutil
// minor than the AppImage already had loaded -- and glibc's link
// model deduplicates by SONAME across dlmopen() namespaces, so even
// with LM_ID_NEWLM the AppImage's older libavutil won. The fix that
// is actually robust is to *not* mix: use the libav set that is
// already in process (matching minors guaranteed) and never touch
// system libavformat at all -- the RTSP protocol layer is now
// implemented in stubs/rtsp_client.cpp.
//
// Headers are still included normally: AVFrame / AVCodecContext /
// AVERROR / AV_LOG_WARNING / AV_PIX_FMT_YUVJ420P are header-only
// types/macros and do not pull in any runtime library.

#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace obn::ffmpeg_dyn {

// One pointer per libav function the H.264 decoder + scaler call.
// decltype(&::name) gives us exact prototype matching (return type +
// parameter types) without retyping signatures by hand, so a libav
// header bump won't silently drift.
//
// libavformat is intentionally absent: rtsp_client.cpp owns RTSP /
// RTSPS framing and there is no longer any path through libavformat.
//
// The MJPEG *encoder* surface (avcodec_find_encoder /
// avcodec_send_frame / avcodec_receive_packet) is also absent on
// purpose: Bambu Studio's bundled libavcodec ships decoder-only
// (no encoders are registered), so avcodec_find_encoder(MJPEG)
// returns nullptr in-process. The pipeline encodes frames through
// stb_image_write (see image_io::encode_jpeg) instead, which is
// header-only and does not depend on libav.
struct AvApi {
    // libavcodec (decoder side only)
    decltype(&::avcodec_find_decoder)          avcodec_find_decoder;
    decltype(&::avcodec_alloc_context3)        avcodec_alloc_context3;
    decltype(&::avcodec_free_context)          avcodec_free_context;
    decltype(&::avcodec_open2)                 avcodec_open2;
    decltype(&::avcodec_send_packet)           avcodec_send_packet;
    decltype(&::avcodec_receive_frame)         avcodec_receive_frame;
    decltype(&::av_packet_alloc)               av_packet_alloc;
    decltype(&::av_packet_free)                av_packet_free;

    // libavutil
    decltype(&::av_frame_alloc)                av_frame_alloc;
    decltype(&::av_frame_free)                 av_frame_free;
    decltype(&::av_strerror)                   av_strerror;
    decltype(&::av_log_set_level)              av_log_set_level;

    // libswscale
    decltype(&::sws_getContext)                sws_getContext;
    decltype(&::sws_freeContext)               sws_freeContext;
    decltype(&::sws_scale)                     sws_scale;
};

// Resolves the AvApi by walking the host process's already-loaded
// libraries via dlsym(RTLD_DEFAULT, ...). Lazy under std::call_once.
//
// Returns true the first time every required symbol resolves. On
// failure (typically because the host is not Bambu Studio and has
// not loaded libav for any other reason) returns false; callers
// route that to "RTSPS unsupported on this Studio build" and leave
// the MJPG / file-browser paths working.
bool ensure_loaded();

// Pointer to the populated AvApi after a successful ensure_loaded();
// nullptr otherwise. Stable for the lifetime of the process.
const AvApi* api();

// One-line description of why ensure_loaded() returned false the
// most recent time. Stable across calls; safe to call before
// ensure_loaded() (returns "ffmpeg_dyn: not yet attempted").
const char* last_load_error();

} // namespace obn::ffmpeg_dyn
