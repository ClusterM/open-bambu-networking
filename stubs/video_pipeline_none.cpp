// IVideoPipeline backend: "none".
//
// Selected via -DOBN_VIDEO_BACKEND=none. No RTSPS support; the C-API
// reports a clear error to Studio when an X1/P1S/P2S URL is opened
// instead of segfaulting somewhere downstream because nobody linked
// a transcoder. The MJPG path (port 6000, A1/A1 mini/P1/P1P) is
// implemented entirely in BambuSource.cpp and stays functional.
//
// This file exists so the build still produces a valid
// libBambuSource.so for users who explicitly want a small camera-less
// build (e.g. to ship just the file-browser bridge).

#include "video_pipeline.hpp"

namespace obn::video {

std::unique_ptr<IVideoPipeline> make_video_pipeline(
    obn::source::Logger /*logger*/, void* /*log_ctx*/)
{
    return nullptr;
}

const char* backend_name() { return "none"; }

} // namespace obn::video
