// Implementation of the image_io facade. See image_io.hpp for the
// rationale behind vendoring stb_image instead of linking libpng /
// libjpeg-turbo.
//
// stb's "implementation" pattern is one massive translation unit per
// header, gated by STB_*_IMPLEMENTATION before #include. We are the
// designated TU; nobody else in libBambuSource includes the stb
// headers, which is why image_io.cpp owns both impls.

#include "image_io.hpp"

// stb_image: PNG + JPEG only. The other formats (HDR, GIF, BMP, PSD,
// PIC, PNM, TGA) are non-features for us -- the file browser deals
// with .png plate previews and .jpg timelapse sidecars exclusively.
// Compiling them out keeps the resulting .o under control.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_GIF
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_TGA
// Only allow decoding from memory; we never feed stb a FILE*, so
// disabling stdio strips libc dependencies we don't need.
#define STBI_NO_STDIO
#include "third_party/stb_image.h"

// stb_image_write: PNG only via stbi_write_png_to_func. We feed the
// output bytes back into a std::vector through a tiny callback so
// there is no intermediate file.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "third_party/stb_image_write.h"

#include <cstring>

namespace obn::image {

bool decode_rgba(const std::vector<std::uint8_t>& in, DecodedRGBA* out)
{
    if (in.size() < 4 || !out) return false;

    // stb requires int dimensions; clamp the input length defensively.
    if (in.size() > static_cast<std::size_t>(INT32_MAX)) return false;

    int w = 0, h = 0, channels_in_file = 0;
    // req_comp = 4 forces stb to expand 1/2/3-channel sources to RGBA
    // and synthesise an alpha=0xff for opaque inputs. That matches the
    // legacy libpng path which also produced PNG_FORMAT_RGBA.
    stbi_uc* px = stbi_load_from_memory(
        in.data(),
        static_cast<int>(in.size()),
        &w, &h, &channels_in_file,
        /*req_comp=*/4);
    if (!px) return false;

    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        stbi_image_free(px);
        return false;
    }

    const std::size_t bytes = static_cast<std::size_t>(w) * h * 4;
    out->w = static_cast<std::uint32_t>(w);
    out->h = static_cast<std::uint32_t>(h);
    out->pixels.assign(px, px + bytes);
    stbi_image_free(px);
    return true;
}

namespace {

// stb_image_write callback: stb hands us a chunk of the output buffer,
// we just append it. Allocations are amortised by the std::vector's
// usual exponential growth; for our typical thumbnail (~20-200 KB) the
// total cost is one or two reallocations.
void write_to_vector(void* ctx, void* data, int size)
{
    if (size <= 0 || !data || !ctx) return;
    auto* v = static_cast<std::vector<std::uint8_t>*>(ctx);
    auto* p = static_cast<const std::uint8_t*>(data);
    v->insert(v->end(), p, p + static_cast<std::size_t>(size));
}

} // namespace

bool encode_png(const DecodedRGBA& src, std::vector<std::uint8_t>* out)
{
    if (!out) return false;
    if (src.w == 0 || src.h == 0 || src.w > 8192 || src.h > 8192) return false;
    const std::size_t expect = static_cast<std::size_t>(src.w) * src.h * 4;
    if (src.pixels.size() != expect) return false;

    out->clear();
    out->reserve(expect / 2); // crude lower bound; PNG of an opaque
                              // photo-like canvas is usually ~15-30 %
                              // of the raw bytes.

    const int rc = stbi_write_png_to_func(
        &write_to_vector, out,
        static_cast<int>(src.w),
        static_cast<int>(src.h),
        /*comp=*/4,
        src.pixels.data(),
        /*stride_in_bytes=*/static_cast<int>(src.w * 4));
    if (rc == 0) {
        out->clear();
        return false;
    }
    return true;
}

bool encode_jpeg(const std::uint8_t* rgb,
                 std::uint32_t w, std::uint32_t h,
                 int quality,
                 std::vector<std::uint8_t>* out)
{
    if (!rgb || !out) return false;
    if (w == 0 || h == 0 || w > 8192 || h > 8192) return false;
    if (quality < 1)   quality = 1;
    if (quality > 100) quality = 100;

    out->clear();
    // Reserve a soft-typical baseline-JPEG size (~10-15 % of raw RGB
    // for photographic content at q=80). Re-allocs are exponential
    // anyway, so this is just to avoid the first few small grow steps.
    const std::size_t raw = static_cast<std::size_t>(w) * h * 3;
    out->reserve(raw / 8);

    // stb_image_write's JPEG encoder takes tightly-packed data; the
    // caller (h264_decoder.cpp) already routes sws_scale into a
    // contiguous w*3-stride buffer for exactly this reason, so
    // there is no per-row copy needed here.
    const int rc = stbi_write_jpg_to_func(
        &write_to_vector, out,
        static_cast<int>(w),
        static_cast<int>(h),
        /*comp=*/3,
        rgb,
        quality);
    if (rc == 0) {
        out->clear();
        return false;
    }
    return true;
}

} // namespace obn::image
