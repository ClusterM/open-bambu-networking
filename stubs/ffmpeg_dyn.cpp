#include "ffmpeg_dyn.hpp"

#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "source_log.hpp"

namespace obn::ffmpeg_dyn {
namespace {

AvApi       g_api{};
bool        g_loaded = false;
std::string g_err    = "ffmpeg_dyn: not yet attempted";

// Templated helper: lookup `sym` via dlsym(RTLD_DEFAULT, ...) and write
// the resulting pointer into `out`. The dlerror() reset trick lets us
// distinguish "symbol resolved to a real NULL" (impossible for libav
// exports but the C standard reserves the case) from "symbol not
// found".
template <typename F>
bool resolve(F& out, const char* sym, std::string& err)
{
    (void)::dlerror();
    void* p = ::dlsym(RTLD_DEFAULT, sym);
    if (const char* d = ::dlerror()) {
        err = std::string("dlsym(") + sym + "): " + d;
        return false;
    }
    if (!p) {
        err = std::string("dlsym(") + sym + "): symbol not found";
        return false;
    }
    out = reinterpret_cast<F>(p);
    return true;
}

bool do_load(std::string& err)
{
    // Cheap sentinel: if avcodec_version is not in the link map, libav
    // is simply not loaded into this process and there is nothing for
    // us to bind to. Fail fast with a clear message instead of
    // marching through the whole table and returning the first
    // dlsym error.
    if (!::dlsym(RTLD_DEFAULT, "avcodec_version")) {
        err = "ffmpeg_dyn: libavcodec is not loaded in this process "
              "(this Studio build does not bundle libavcodec, the "
              "RTSPS pipeline cannot run)";
        return false;
    }

#define R(fn)                                                                  \
    do {                                                                       \
        if (!resolve(g_api.fn, #fn, err)) return false;                        \
    } while (0)

    R(avcodec_find_decoder);
    R(avcodec_alloc_context3);
    R(avcodec_free_context);
    R(avcodec_open2);
    R(avcodec_send_packet);
    R(avcodec_receive_frame);
    R(av_packet_alloc);
    R(av_packet_free);

    R(av_frame_alloc);
    R(av_frame_free);
    R(av_strerror);
    R(av_log_set_level);

    R(sws_getContext);
    R(sws_freeContext);
    R(sws_scale);

#undef R
    obn::source::log_fmt(nullptr, nullptr,
                         "ffmpeg_dyn: resolved libav* via RTLD_DEFAULT "
                         "(in-process libavcodec/libavutil/libswscale)");
    return true;
}

} // namespace

bool ensure_loaded()
{
    static std::once_flag once;
    std::call_once(once, []() {
        std::string err;
        if (do_load(err)) {
            g_loaded = true;
            g_err.clear();
        } else {
            g_loaded = false;
            g_err    = err;
        }
    });
    return g_loaded;
}

const AvApi* api()
{
    return g_loaded ? &g_api : nullptr;
}

const char* last_load_error()
{
    return g_err.c_str();
}

} // namespace obn::ffmpeg_dyn
