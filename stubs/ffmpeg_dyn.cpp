// Implementation of the lazy libav loader. See ffmpeg_dyn.hpp for the
// rationale.

#include "ffmpeg_dyn.hpp"

#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "source_log.hpp"

namespace obn::ffmpeg_dyn {
namespace {

// Soname candidates per library, newest first. A single libBambuSource
// binary needs to keep working across at least the four libav major
// generations users still see in the wild:
//
//   FFmpeg 7.x  -> *.so.61 / *.so.59 / *.so.8     (Debian trixie, Fedora 40+, Arch)
//   FFmpeg 6.x  -> *.so.60 / *.so.58 / *.so.7     (Ubuntu 24.04, Fedora 39)
//   FFmpeg 5.x  -> *.so.59 / *.so.57 / *.so.6     (Debian 12, Ubuntu 23.04)
//   FFmpeg 4.x  -> *.so.58 / *.so.56 / *.so.5     (Ubuntu 22.04)
//
// The unversioned filename is the last fallback so a hand-built FFmpeg
// without an explicit soname still gets a chance.

#if defined(__APPLE__)
constexpr const char* kAvformatNames[] = {
    "libavformat.61.dylib", "libavformat.60.dylib", "libavformat.59.dylib",
    "libavformat.58.dylib", "libavformat.dylib",
    nullptr,
};
constexpr const char* kAvcodecNames[] = {
    "libavcodec.61.dylib",  "libavcodec.60.dylib",  "libavcodec.59.dylib",
    "libavcodec.58.dylib",  "libavcodec.dylib",
    nullptr,
};
constexpr const char* kAvutilNames[] = {
    "libavutil.59.dylib",   "libavutil.58.dylib",   "libavutil.57.dylib",
    "libavutil.56.dylib",   "libavutil.dylib",
    nullptr,
};
constexpr const char* kSwscaleNames[] = {
    "libswscale.8.dylib",   "libswscale.7.dylib",   "libswscale.6.dylib",
    "libswscale.5.dylib",   "libswscale.dylib",
    nullptr,
};
#else
constexpr const char* kAvformatNames[] = {
    "libavformat.so.61", "libavformat.so.60", "libavformat.so.59",
    "libavformat.so.58", "libavformat.so.57", "libavformat.so",
    nullptr,
};
constexpr const char* kAvcodecNames[] = {
    "libavcodec.so.61",  "libavcodec.so.60",  "libavcodec.so.59",
    "libavcodec.so.58",  "libavcodec.so.57",  "libavcodec.so",
    nullptr,
};
constexpr const char* kAvutilNames[] = {
    "libavutil.so.59",   "libavutil.so.58",   "libavutil.so.57",
    "libavutil.so.56",   "libavutil.so.55",   "libavutil.so",
    nullptr,
};
constexpr const char* kSwscaleNames[] = {
    "libswscale.so.8",   "libswscale.so.7",   "libswscale.so.6",
    "libswscale.so.5",   "libswscale.so.4",   "libswscale.so",
    nullptr,
};
#endif

// Try every name until one opens. RTLD_GLOBAL so subsequent dlopens of
// libav modules that depend on common libavutil symbols (e.g. dlopen of
// libavcodec depending on libavutil's av_get_token) actually resolve
// against the libavutil we just loaded instead of pulling a second copy
// from the system path. Keeps us aligned with what gst-libav does.
struct OpenedLib {
    void*       handle = nullptr;
    const char* name   = nullptr;
};

OpenedLib dlopen_first(const char* const* names)
{
    for (int i = 0; names[i]; ++i) {
        if (void* h = ::dlopen(names[i], RTLD_NOW | RTLD_GLOBAL)) {
            return {h, names[i]};
        }
    }
    return {};
}

// Single point that joins dlerror() (if any) with our own context so the
// final error message reads "libavformat: libavformat.so.61: cannot
// open shared object file: No such file or directory".
std::string dlopen_error_for(const char* const* names, const char* what)
{
    std::string msg = what;
    msg += ": tried";
    for (int i = 0; names[i]; ++i) {
        msg += ' ';
        msg += names[i];
    }
    if (const char* d = ::dlerror()) {
        msg += "; last dlerror: ";
        msg += d;
    }
    return msg;
}

template <typename F>
bool resolve(void* h, F& out, const char* sym, std::string& err)
{
    // Clear any stale dlerror first; dlsym() returning NULL is
    // ambiguous (a real symbol can legitimately be NULL on POSIX),
    // so we go through the dlerror() reset trick.
    (void)::dlerror();
    void* p = ::dlsym(h, sym);
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

AvApi              g_api{};
bool               g_loaded = false;
std::string        g_err    = "ffmpeg dynamic loader: not yet attempted";
std::vector<void*> g_handles; // kept for the lifetime of the process

bool do_load(std::string& err)
{
    OpenedLib hfmt = dlopen_first(kAvformatNames);
    if (!hfmt.handle) {
        err = dlopen_error_for(kAvformatNames, "libavformat");
        return false;
    }
    g_handles.push_back(hfmt.handle);

    OpenedLib hcod = dlopen_first(kAvcodecNames);
    if (!hcod.handle) {
        err = dlopen_error_for(kAvcodecNames, "libavcodec");
        return false;
    }
    g_handles.push_back(hcod.handle);

    OpenedLib hutil = dlopen_first(kAvutilNames);
    if (!hutil.handle) {
        err = dlopen_error_for(kAvutilNames, "libavutil");
        return false;
    }
    g_handles.push_back(hutil.handle);

    OpenedLib hsws = dlopen_first(kSwscaleNames);
    if (!hsws.handle) {
        err = dlopen_error_for(kSwscaleNames, "libswscale");
        return false;
    }
    g_handles.push_back(hsws.handle);

// `lib` rather than `handle` so the parameter name doesn't collide
// with the OpenedLib::handle struct field during preprocessor expansion.
#define R(lib, fn)                                                            \
    do {                                                                      \
        if (!resolve((lib).handle, g_api.fn, #fn, err)) {                     \
            err = std::string((lib).name) + ": " + err;                       \
            return false;                                                     \
        }                                                                     \
    } while (0)

    R(hfmt, avformat_alloc_context);
    R(hfmt, avformat_network_init);
    R(hfmt, avformat_open_input);
    R(hfmt, avformat_close_input);
    R(hfmt, avformat_find_stream_info);
    R(hfmt, av_find_best_stream);
    R(hfmt, av_read_frame);

    R(hcod, avcodec_find_decoder);
    R(hcod, avcodec_find_encoder);
    R(hcod, avcodec_alloc_context3);
    R(hcod, avcodec_free_context);
    R(hcod, avcodec_parameters_to_context);
    R(hcod, avcodec_open2);
    R(hcod, avcodec_send_packet);
    R(hcod, avcodec_receive_frame);
    R(hcod, avcodec_send_frame);
    R(hcod, avcodec_receive_packet);
    R(hcod, av_packet_alloc);
    R(hcod, av_packet_free);
    R(hcod, av_packet_unref);

    R(hutil, av_frame_alloc);
    R(hutil, av_frame_free);
    R(hutil, av_frame_get_buffer);
    R(hutil, av_frame_make_writable);
    R(hutil, av_strerror);
    R(hutil, av_dict_set);
    R(hutil, av_dict_free);
    R(hutil, av_log_set_level);
    R(hutil, av_log_set_callback);

    R(hsws, sws_getContext);
    R(hsws, sws_freeContext);
    R(hsws, sws_scale);

#undef R

    // Log which sonames actually got picked. Printed once per process
    // and only at TRACE level since the typical user does not need it,
    // but it is invaluable when diagnosing version-mismatch failures.
    obn::source::log_at(obn::source::LL_TRACE, nullptr, nullptr,
                        "ffmpeg_dyn: resolved libav* via dlopen");
    obn::source::log_fmt(nullptr, nullptr,
                         "ffmpeg_dyn: avformat=%s avcodec=%s avutil=%s swscale=%s",
                         hfmt.name, hcod.name, hutil.name, hsws.name);
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
