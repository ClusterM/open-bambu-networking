// Tiny image decode/encode facade backed by stb_image / stb_image_write.
//
// The thumbnail letterbox path in BambuSource needs to decode a PNG or
// JPEG, paint it on a canvas with the right aspect, and re-encode the
// result as PNG. We used to call libpng + libjpeg-turbo directly, but
// that pulled the system libpng16/libjpeg into libBambuSource's NEEDED
// list and turned out to be a recurring source of trouble inside Bambu
// Studio's process: notably the libjpeg-turbo ABI mismatch ("Wrong JPEG
// library version: library is 80, caller expects 62") that abort()s
// gst-jpeg-encoder.so the moment Studio's GStreamer pipeline tries to
// touch libjpeg via our globally-loaded copy.
//
// stb_image / stb_image_write are single-header public-domain libraries
// we vendor under stubs/third_party/. They have no external NEEDED
// entries -- libBambuSource.so is therefore self-contained for image
// I/O, and Studio's own libpng / libjpeg / GStreamer plugins live their
// version-tagged lives undisturbed by us.

#pragma once

#include <cstdint>
#include <vector>

namespace obn::image {

// Row-major RGBA8 surface with no row padding.
struct DecodedRGBA {
    std::vector<std::uint8_t> pixels;
    std::uint32_t             w = 0;
    std::uint32_t             h = 0;
};

// Sniffs the PNG / JPEG magic in `in` and decodes into `*out`. Returns
// false on truncated input, decode errors, missing magic, or sizes
// outside [1..8192] in either dimension (the latter is a guard against
// pathological 3mf/firmware payloads, not an architectural cap).
//
// Output is always 4-channel RGBA: opaque sources get alpha=0xff, JPEG
// sources are upsampled to RGBA, PNGs are converted as needed. Caller
// is responsible for the buffer lifetime; the function never throws.
bool decode_rgba(const std::vector<std::uint8_t>& in, DecodedRGBA* out);

// Encodes `src` (must be RGBA8 with pixels.size() == w*h*4) as a PNG
// byte stream into `*out`. The output is *always* a fresh PNG; it does
// not preserve the input format -- by design, since the letterbox path
// needs an alpha channel for the transparent bars and PNG is the only
// lossless RGBA format we care about.
//
// Returns false on invalid dimensions or stb_image_write failure
// (typically OOM). The caller in reshape_image_to_aspect treats this
// as a non-fatal soft failure: the original bytes are returned and the
// MIME type is left untouched.
bool encode_png(const DecodedRGBA& src, std::vector<std::uint8_t>* out);

} // namespace obn::image
