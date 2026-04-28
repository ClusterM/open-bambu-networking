// IVideoPipeline backed by GStreamer.
//
// This is the legacy implementation that came out of the original
// libBambuSource.so reverse-engineering: rtspsrc -> rtph264depay ->
// h264parse -> <h264-decoder> -> queue -> videoconvert -> videoscale
// -> capsfilter (1280x720) -> jpegenc (q=80, ifast IDCT) -> appsink.
//
// It still works fine but pulls in the entire GStreamer runtime
// (system or bundle) just to do RTSP demux + H.264 decode + MJPEG
// encode -- a job FFmpeg/libav covers in three small calls. The
// FFmpeg backend is the new default; this file stays around behind
// -DOBN_VIDEO_BACKEND=gstreamer for one release in case the FFmpeg
// path regresses on someone's setup, then can be removed.
//
// The code below is the exact same pipeline the original
// stubs/BambuSource.cpp shipped (same caps, same property values,
// same pad-added wiring) -- just behind IVideoPipeline so the C-ABI
// caller is backend-agnostic.

#include "video_pipeline.hpp"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <vector>

namespace {

using obn::source::log_at;
using obn::source::log_fmt;
using obn::source::Logger;
using obn::source::LL_DEBUG;
using obn::source::set_last_error;

// avdec_h264 decodes Bambu's stream into AV_PIX_FMT_YUVJ420P (JPEG
// full-range YUV), whose handling is deprecated in modern ffmpeg.
// gst_videoconvert's internal swscale prints one "deprecated pixel
// format used, make sure you did set range correctly" per frame to
// stderr - at 30 fps that's 1800 lines/minute spamming Studio's
// console. We don't link libav directly (it's pulled in by gst-libav),
// so we reach av_log_set_level through dlsym after gst_init has
// already loaded libavutil into the process. AV_LOG_ERROR (16) is
// loud enough to still surface real decoder failures.
void silence_libav_warnings()
{
    constexpr int kAvLogError = 16;
    using SetLevelFn = void (*)(int);
    auto try_hush = [&](const char* soname) -> bool {
        void* h = dlopen(soname, RTLD_NOW | RTLD_NOLOAD);
        if (!h) return false;
        if (auto fn = reinterpret_cast<SetLevelFn>(dlsym(h, "av_log_set_level"))) {
            fn(kAvLogError);
            dlclose(h);
            return true;
        }
        dlclose(h);
        return false;
    };
    if (try_hush("libavutil.so.59")) return;
    if (try_hush("libavutil.so.58")) return;
    if (try_hush("libavutil.so.57")) return;
    if (try_hush("libavutil.so.56")) return;
    if (try_hush("libavutil.so")) return;
    (void)try_hush("libavutil.so.55");
}

// Reroutes GStreamer's default debug output into obn-bambusource.log
// so we can diagnose rtspsrc handshake failures without having to
// set GST_DEBUG in Studio's environment. Threshold defaults to
// WARNING + ERROR; OBN_GST_DEBUG=<level> raises it.
void gst_log_handler(GstDebugCategory* cat, GstDebugLevel level,
                     const gchar* file, const gchar* /*function*/, gint line,
                     GObject* /*object*/, GstDebugMessage* message,
                     gpointer /*user_data*/)
{
    if (level > gst_debug_category_get_threshold(cat)) return;
    FILE* fp = obn::source::mirror_log_fp();
    if (!fp) return;
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%F %T", std::localtime(&tt));
    std::fprintf(fp, "%s [gst %s] %s:%d: %s\n",
                 ts, gst_debug_level_get_name(level),
                 file ? file : "?",
                 line,
                 gst_debug_message_get(message));
}

void gst_init_once()
{
    static std::once_flag once;
    std::call_once(once, []() {
        gst_init(nullptr, nullptr);
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
        silence_libav_warnings();
    });
}

// Formats the rtsp(s) URL the way gst-rtsp-server expects: "<scheme>://
// <host>:<port><path>". User/password are passed through rtspsrc
// properties (user-id / user-pw), NOT embedded in the URI, because
// rtspsrc percent-decodes userinfo and silently mangles access codes
// that contain '+', '/', or '&'.
std::string build_rtsp_uri(const obn::video::StartConfig& cfg)
{
    const char* scheme = (cfg.scheme == obn::video::Scheme::Rtsps) ? "rtsps" : "rtsp";
    return std::string(scheme) + "://" + cfg.host + ":" +
           std::to_string(cfg.port) + cfg.path;
}

// rtspsrc has "sometimes" src pads that only exist after SDP
// negotiation; gst_parse_launch's auto-linking handles this in simple
// cases, but we've seen the link silently fail and surface as
// "streaming stopped, reason not-linked" in Studio. Doing the link by
// hand here removes that ambiguity.
struct PadCtx {
    GstElement* depay;
    Logger      logger;
    void*       log_ctx;
};

extern "C" void rtsp_pad_added(GstElement* /*src*/, GstPad* new_pad,
                               gpointer user_data)
{
    auto* ctx = static_cast<PadCtx*>(user_data);
    GstElement* depay = ctx->depay;

    gchar* pad_name = gst_pad_get_name(new_pad);
    GstCaps* caps = gst_pad_get_current_caps(new_pad);
    if (!caps) caps = gst_pad_query_caps(new_pad, nullptr);
    gchar* caps_str = caps ? gst_caps_to_string(caps) : g_strdup("(null)");
    log_fmt(ctx->logger, ctx->log_ctx,
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
        log_fmt(ctx->logger, ctx->log_ctx,
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
        log_fmt(ctx->logger, ctx->log_ctx,
                "rtsp_pad_added: link pad=%s -> depay.sink = %d",
                pad_name ? pad_name : "?", (int)r);
    } else {
        log_fmt(ctx->logger, ctx->log_ctx,
                "rtsp_pad_added: skipping non-video pad=%s",
                pad_name ? pad_name : "?");
    }

    gst_object_unref(sink_pad);
    if (pad_name) g_free(pad_name);
    if (caps_str) g_free(caps_str);
    if (caps)     gst_caps_unref(caps);
}

class GstreamerPipeline final : public obn::video::IVideoPipeline {
public:
    GstreamerPipeline(Logger logger, void* log_ctx)
        : logger_(logger ? logger : obn::source::noop_logger), log_ctx_(log_ctx) {}

    ~GstreamerPipeline() override { stop(); }

    int start(const obn::video::StartConfig& cfg) override
    {
        gst_init_once();

        const std::string uri = build_rtsp_uri(cfg);
        log_fmt(logger_, log_ctx_, "gst: start uri=%s user=%s",
                uri.c_str(), cfg.user.c_str());

        // Element factory lookup. We pick the H.264 decoder at runtime
        // because Bambu Studio ships its own libav* in <install>/bin and
        // those symbol-conflict with the system's gst-libav plugin
        // (`module_open failed: libavformat.so.61: undefined symbol
        // av_mastering_display_metadata_alloc_size, version LIBAVUTIL_59`).
        // Preferred order: openh264dec (pure userspace, no ffmpeg ABI tie),
        // then vah264dec (Intel VA-API), then vulkanh264device1dec,
        // then avdec_h264 as last resort.
        static const char* const kH264Decoders[] = {
            "openh264dec", "vah264dec", "vulkanh264device1dec", "avdec_h264",
        };
        GstElement* pipeline  = gst_pipeline_new("obn-rtsp");
        GstElement* src       = gst_element_factory_make("rtspsrc",      "src");
        GstElement* depay     = gst_element_factory_make("rtph264depay", "depay");
        GstElement* parse     = gst_element_factory_make("h264parse",    "parse");
        GstElement* dec       = nullptr;
        const char* dec_name  = nullptr;
        for (const char* name : kH264Decoders) {
            dec = gst_element_factory_make(name, "dec");
            if (dec) { dec_name = name; break; }
        }
        GstElement* dec_queue = gst_element_factory_make("queue",        "decq");
        GstElement* conv      = gst_element_factory_make("videoconvert", "conv");
        GstElement* scale     = gst_element_factory_make("videoscale",   "scale");
        GstElement* capsf     = gst_element_factory_make("capsfilter",   "capsf");
        GstElement* enc       = gst_element_factory_make("jpegenc",      "enc");
        GstElement* sink      = gst_element_factory_make("appsink",      "sink");
        if (!pipeline || !src || !depay || !parse || !dec || !dec_queue ||
            !conv || !scale || !capsf || !enc || !sink) {
            log_fmt(logger_, log_ctx_,
                    "gst: missing GStreamer element(s): "
                    "pipeline=%p src=%p depay=%p parse=%p dec=%p decq=%p "
                    "conv=%p scale=%p capsf=%p enc=%p sink=%p",
                    pipeline, src, depay, parse, dec, dec_queue,
                    conv, scale, capsf, enc, sink);
            if (pipeline) gst_object_unref(pipeline);
            else {
                if (src)       gst_object_unref(src);
                if (depay)     gst_object_unref(depay);
                if (parse)     gst_object_unref(parse);
                if (dec)       gst_object_unref(dec);
                if (dec_queue) gst_object_unref(dec_queue);
                if (conv)      gst_object_unref(conv);
                if (scale)     gst_object_unref(scale);
                if (capsf)     gst_object_unref(capsf);
                if (enc)       gst_object_unref(enc);
                if (sink)      gst_object_unref(sink);
            }
            set_last_error("missing gst element");
            return -1;
        }

        // rtspsrc: latency=120 ms is a compromise between end-to-end
        // delay and jitter tolerance. Three frame-times at 30 fps,
        // still below human-perceived "delayed".
        // drop-on-latency=true makes the jitterbuffer leaky on TCP so
        // late packets are dropped instead of triggering catch-up bursts.
        // tls-validation-flags=0 matches the stock plugin which accepts
        // the printer's self-signed cert.
        g_object_set(src,
                     "location",             uri.c_str(),
                     "user-id",              cfg.user.c_str(),
                     "user-pw",              cfg.passwd.c_str(),
                     "protocols",            4 /* GST_RTSP_LOWER_TRANS_TCP */,
                     "latency",              120,
                     "drop-on-latency",      TRUE,
                     "tls-validation-flags", 0,
                     "do-retransmission",    FALSE,
                     nullptr);

        log_fmt(logger_, log_ctx_,
                "gst: using H.264 decoder '%s'",
                dec_name ? dec_name : "(none!)");

        // h264parse config-interval=-1: insert SPS/PPS before every
        // keyframe so the decoder can always lock on, even if we join
        // mid-stream.
        g_object_set(parse, "config-interval", -1, nullptr);

        // 720p MJPG to keep parity with A1/P1 cameras (one jpegdec per
        // frame on Studio's side, no H.264 decoder bin), so 30 fps
        // actually reaches the screen instead of the "half speed"
        // symptom we see when handing H.264 straight to gstbambusrc.
        char caps_str[64];
        std::snprintf(caps_str, sizeof(caps_str),
                      "video/x-raw,width=%d,height=%d",
                      cfg.target_width, cfg.target_height);
        GstCaps* scaled_caps = gst_caps_from_string(caps_str);
        g_object_set(capsf, "caps", scaled_caps, nullptr);
        gst_caps_unref(scaled_caps);

        // jpegenc: q=80 + ifast IDCT is the sweet spot for live camera
        // (~4-5% of one core for 720p30 vs ~8% at q=90/islow).
        g_object_set(enc, "quality", 80, "idct-method", 1 /* ifast */, nullptr);

        // dec_queue: small leaky queue between H.264 decoder and the
        // conv/scale/jpegenc chain. Without it a Studio-side stall
        // back-pressures rtspsrc and triggers "fast-forward" bursts on
        // recovery; 2 buffers + leaky=downstream means we drop the
        // oldest frame instead.
        g_object_set(dec_queue,
                     "leaky",            2 /* GST_QUEUE_LEAKY_DOWNSTREAM */,
                     "max-size-buffers", 2,
                     "max-size-bytes",   0,
                     "max-size-time",    G_GUINT64_CONSTANT(0),
                     nullptr);

        // appsink: poll-based (we call try_pull_sample ourselves), drop
        // oldest if a new arrives, sync=false because Studio runs its
        // own clock.
        g_object_set(sink,
                     "emit-signals", FALSE,
                     "drop",         TRUE,
                     "max-buffers",  2,
                     "sync",         FALSE,
                     nullptr);

        gst_bin_add_many(GST_BIN(pipeline), src, depay, parse, dec,
                         dec_queue, conv, scale, capsf, enc, sink, nullptr);

        if (!gst_element_link_many(depay, parse, dec, dec_queue, conv,
                                   scale, capsf, enc, sink, nullptr)) {
            log_fmt(logger_, log_ctx_,
                    "gst: gst_element_link_many failed");
            gst_object_unref(pipeline);
            set_last_error("link failed");
            return -1;
        }

        // Hook up rtspsrc's dynamic pad. Closure freed by glib via the
        // destroy-notify when the signal handler is disconnected (i.e.
        // when the pipeline is unref'd).
        auto* pad_ctx = new PadCtx{depay, logger_, log_ctx_};
        gulong handler_id = g_signal_connect_data(
            src, "pad-added",
            G_CALLBACK(rtsp_pad_added),
            pad_ctx,
            /*destroy_notify=*/[](gpointer data, GClosure*) {
                delete static_cast<PadCtx*>(data);
            },
            GConnectFlags(0));
        log_fmt(logger_, log_ctx_,
                "gst: pad-added connected handler_id=%lu", handler_id);

        pipeline_ = pipeline;
        appsink_  = GST_APP_SINK(sink);

        // Kick the pipeline. PLAYING is asynchronous; we wait up to 10 s
        // for ASYNC_DONE so we surface a clean handshake error instead
        // of letting the user wait until the first ReadSample times out.
        log_fmt(logger_, log_ctx_, "gst: set_state -> PLAYING");
        GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
        log_fmt(logger_, log_ctx_, "gst: set_state returned %d", (int)ret);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            log_fmt(logger_, log_ctx_, "gst: set_state PLAYING failed");
            set_last_error("gst PLAYING failed");
            return -1;
        }

        // gst-libav only loads libav* at first PAUSED/PLAYING; retry
        // hush now that the libraries are in-process.
        silence_libav_warnings();

        GstBus* bus = gst_element_get_bus(pipeline_);
        const GstClockTime kWaitNs = 10ULL * GST_SECOND;
        bool ok = false;
        while (true) {
            GstMessage* msg = gst_bus_timed_pop_filtered(
                bus, kWaitNs,
                static_cast<GstMessageType>(GST_MESSAGE_ASYNC_DONE |
                                            GST_MESSAGE_ERROR |
                                            GST_MESSAGE_EOS));
            if (!msg) {
                log_fmt(logger_, log_ctx_, "gst: timeout waiting ASYNC_DONE");
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
                log_fmt(logger_, log_ctx_,
                        "gst: error: %s (%s)",
                        err  ? err->message  : "?",
                        dbg  ? dbg           : "");
                set_last_error(err ? err->message : "rtsp error");
                if (err) g_error_free(err);
                if (dbg) g_free(dbg);
                gst_message_unref(msg);
                break;
            }
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
                log_fmt(logger_, log_ctx_, "gst: EOS before playback");
                set_last_error("rtsp EOS at open");
                gst_message_unref(msg);
                break;
            }
            gst_message_unref(msg);
        }
        gst_object_unref(bus);

        if (!ok) return -1;

        width_  = cfg.target_width;
        height_ = cfg.target_height;
        t0_     = std::chrono::steady_clock::now();
        log_fmt(logger_, log_ctx_,
                "gst: pipeline PLAYING (mjpg %dx%d)", width_, height_);
        return 0;
    }

    obn::video::PullResult try_pull(obn::video::Frame* out) override
    {
        if (!appsink_) return obn::video::Pull_Error;

        // Drain any pending bus messages so the mirror log names the
        // culprit when something dies mid-stream.
        if (pipeline_) {
            GstBus* bus = gst_element_get_bus(pipeline_);
            while (GstMessage* m = gst_bus_pop_filtered(
                       bus, static_cast<GstMessageType>(
                                GST_MESSAGE_ERROR | GST_MESSAGE_WARNING |
                                GST_MESSAGE_EOS | GST_MESSAGE_STREAM_STATUS))) {
                switch (GST_MESSAGE_TYPE(m)) {
                case GST_MESSAGE_ERROR: {
                    GError* e = nullptr; gchar* d = nullptr;
                    gst_message_parse_error(m, &e, &d);
                    log_fmt(logger_, log_ctx_,
                            "gst: [%s] ERROR %s (%s)",
                            GST_OBJECT_NAME(GST_MESSAGE_SRC(m)),
                            e ? e->message : "?", d ? d : "");
                    if (e) g_error_free(e);
                    if (d) g_free(d);
                    break;
                }
                case GST_MESSAGE_WARNING: {
                    GError* e = nullptr; gchar* d = nullptr;
                    gst_message_parse_warning(m, &e, &d);
                    log_fmt(logger_, log_ctx_,
                            "gst: [%s] WARN %s (%s)",
                            GST_OBJECT_NAME(GST_MESSAGE_SRC(m)),
                            e ? e->message : "?", d ? d : "");
                    if (e) g_error_free(e);
                    if (d) g_free(d);
                    break;
                }
                case GST_MESSAGE_EOS:
                    log_fmt(logger_, log_ctx_,
                            "gst: [%s] EOS",
                            GST_OBJECT_NAME(GST_MESSAGE_SRC(m)));
                    break;
                default: break;
                }
                gst_message_unref(m);
            }
            gst_object_unref(bus);
        }

        // Non-blocking pull. We deliberately never call
        // gst_app_sink_is_eos() here -- in gst 1.26 it returns true
        // transiently whenever the queue is drained between frames,
        // which made Bambu_ReadSample claim Bambu_stream_end between
        // every successful sample. Real EOS only happens on
        // Bambu_Close (which closes us first) or on a server-side
        // teardown (the next would_block tells us nothing arrived for
        // a while, and a higher layer will reconnect).
        GstSample* gsample = gst_app_sink_try_pull_sample(appsink_, 0);
        if (!gsample) return obn::video::Pull_WouldBlock;

        GstBuffer* buf = gst_sample_get_buffer(gsample);
        if (!buf) {
            gst_sample_unref(gsample);
            return obn::video::Pull_WouldBlock;
        }
        GstMapInfo info;
        if (!gst_buffer_map(buf, &info, GST_MAP_READ)) {
            gst_sample_unref(gsample);
            set_last_error("gst_buffer_map failed");
            return obn::video::Pull_Error;
        }

        frame_buf_.assign(info.data, info.data + info.size);
        gst_buffer_unmap(buf, &info);

        // Wall-clock timestamp (see Frame::dt_100ns docs for why).
        auto now = std::chrono::steady_clock::now();
        auto ns  = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       now - t0_).count();
        std::uint64_t dt_100ns = static_cast<std::uint64_t>(ns / 100);

        if (frame_count_ == 0) {
            GstCaps* caps = gst_sample_get_caps(gsample);
            if (caps) {
                GstStructure* s = gst_caps_get_structure(caps, 0);
                gint w = 0, h = 0;
                if (s && gst_structure_get_int(s, "width", &w) &&
                        gst_structure_get_int(s, "height", &h) &&
                        w > 0 && h > 0) {
                    width_  = w;
                    height_ = h;
                }
            }
        }

        gst_sample_unref(gsample);

        if (++frame_count_ == 1 || (frame_count_ % 60) == 0) {
            log_fmt(logger_, log_ctx_,
                    "gst: frame #%llu size=%zu",
                    static_cast<unsigned long long>(frame_count_),
                    frame_buf_.size());
        }

        out->jpeg     = frame_buf_.data();
        out->size     = frame_buf_.size();
        out->width    = width_;
        out->height   = height_;
        out->dt_100ns = dt_100ns;
        out->flags    = 0;
        return obn::video::Pull_Ok;
    }

    void stop() override
    {
        if (!pipeline_) return;
        // NULL state tears down rtspsrc cleanly; then unref. Note we
        // do NOT unref appsink_ separately: the appsink lives inside
        // the pipeline bin and owning a ref on it would leak until
        // process exit.
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        appsink_ = nullptr;
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }

private:
    Logger      logger_;
    void*       log_ctx_;
    GstElement* pipeline_ = nullptr;
    GstAppSink* appsink_  = nullptr;
    int         width_       = 1280;
    int         height_      = 720;
    std::uint64_t                         frame_count_ = 0;
    std::chrono::steady_clock::time_point t0_{};
    std::vector<std::uint8_t>             frame_buf_;
};

} // namespace

namespace obn::video {

std::unique_ptr<IVideoPipeline> make_video_pipeline(
    obn::source::Logger logger, void* log_ctx)
{
    return std::make_unique<GstreamerPipeline>(logger, log_ctx);
}

const char* backend_name() { return "gstreamer"; }

} // namespace obn::video
