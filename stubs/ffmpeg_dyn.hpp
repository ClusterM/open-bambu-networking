// Lazy dlopen of the libav* family used by video_pipeline_ffmpeg.cpp.
//
// libBambuSource intentionally does **not** carry a hard NEEDED entry
// for libavformat/libavcodec/libavutil/libswscale. Instead, the first
// RTSPS pipeline that gets started calls ensure_loaded(), which:
//
//   1. dlopen()s each of the four libraries, walking a list of soname
//      versions the package has shipped over the last decade. If a
//      version is not present the search falls through to the next
//      candidate and finally to the unversioned filename. The list
//      mirrors Debian/Ubuntu/Fedora/Homebrew releases so a single
//      libBambuSource binary works on FFmpeg 4.x, 5.x, 6.x, 7.x.
//
//   2. dlsym()s every function video_pipeline_ffmpeg.cpp uses into
//      an AvApi table. If any symbol is missing -- typically because
//      the runtime libav is older than the headers we built against,
//      or because the four shared objects come from incompatible
//      releases -- the load fails as a whole and api() returns
//      nullptr. Callers map that to "RTSPS unavailable, leave the
//      MJPG path and FTPS file browser untouched".
//
// The motivation is that Bambu Studio bundles its own libav* under
// `<install>/bin` and pins LD_LIBRARY_PATH to that bundle. A hard
// NEEDED dependency on libavformat.so.61 would crash the dlopen of
// libBambuSource itself when that bundle ships .60 (or vice versa),
// taking the entire plugin -- including features that have nothing
// to do with the camera -- offline. Going through dlopen lets the
// dynamic loader report a clean "library not found" / "symbol
// version mismatch" string we can surface in obn-bambusource.log
// while keeping the rest of the plugin alive.
//
// Headers are still included normally: types/enums/macros (AVFrame,
// AVCodecContext, AVERROR, AV_LOG_WARNING, AV_PIX_FMT_YUVJ420P, ...)
// are header-only and don't require any runtime library.

#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace obn::ffmpeg_dyn {

// One pointer per libav function video_pipeline_ffmpeg.cpp calls.
// decltype(&::name) gives us exact prototype matching (return type +
// parameter types) without retyping signatures by hand, so a libav
// header bump won't silently drift.
struct AvApi {
    // libavformat
    decltype(&::avformat_alloc_context)        avformat_alloc_context;
    decltype(&::avformat_network_init)         avformat_network_init;
    decltype(&::avformat_open_input)           avformat_open_input;
    decltype(&::avformat_close_input)          avformat_close_input;
    decltype(&::avformat_find_stream_info)     avformat_find_stream_info;
    decltype(&::av_find_best_stream)           av_find_best_stream;
    decltype(&::av_read_frame)                 av_read_frame;

    // libavcodec
    decltype(&::avcodec_find_decoder)          avcodec_find_decoder;
    decltype(&::avcodec_find_encoder)          avcodec_find_encoder;
    decltype(&::avcodec_alloc_context3)        avcodec_alloc_context3;
    decltype(&::avcodec_free_context)          avcodec_free_context;
    decltype(&::avcodec_parameters_to_context) avcodec_parameters_to_context;
    decltype(&::avcodec_open2)                 avcodec_open2;
    decltype(&::avcodec_send_packet)           avcodec_send_packet;
    decltype(&::avcodec_receive_frame)         avcodec_receive_frame;
    decltype(&::avcodec_send_frame)            avcodec_send_frame;
    decltype(&::avcodec_receive_packet)        avcodec_receive_packet;
    decltype(&::av_packet_alloc)               av_packet_alloc;
    decltype(&::av_packet_free)                av_packet_free;
    decltype(&::av_packet_unref)               av_packet_unref;

    // libavutil
    decltype(&::av_frame_alloc)                av_frame_alloc;
    decltype(&::av_frame_free)                 av_frame_free;
    decltype(&::av_frame_get_buffer)           av_frame_get_buffer;
    decltype(&::av_frame_make_writable)        av_frame_make_writable;
    decltype(&::av_strerror)                   av_strerror;
    decltype(&::av_dict_set)                   av_dict_set;
    decltype(&::av_dict_free)                  av_dict_free;
    decltype(&::av_log_set_level)              av_log_set_level;
    decltype(&::av_log_set_callback)           av_log_set_callback;

    // libswscale
    decltype(&::sws_getContext)                sws_getContext;
    decltype(&::sws_freeContext)               sws_freeContext;
    decltype(&::sws_scale)                     sws_scale;
};

// Resolves the four libav shared objects via dlopen+dlsym, racing
// once across multiple soname candidates per library. Returns true on
// the first successful load; subsequent calls are cheap (std::once).
// On failure returns false and last_load_error() reports a single-line
// human-readable explanation suitable for obn-bambusource.log.
bool ensure_loaded();

// Returns a pointer to the populated AvApi after a successful
// ensure_loaded(); returns nullptr otherwise. The pointer is stable
// for the lifetime of the process -- libav handles are intentionally
// kept open until exit, dlclose'ing them is unnecessary churn given
// how rarely the camera tunnel is torn down.
const AvApi* api();

// One-line description of why ensure_loaded() returned false the
// most recent time. Stable across calls; safe to call before
// ensure_loaded() (returns "ffmpeg dynamic loader: not yet attempted").
const char* last_load_error();

} // namespace obn::ffmpeg_dyn
