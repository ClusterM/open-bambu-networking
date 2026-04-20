#include "obn/zip_reader.hpp"

#include <algorithm>
#include <cstring>
#include <zlib.h>

namespace obn::zip {

bool read_central(const std::vector<std::uint8_t>& zip,
                  std::vector<Entry>*              out)
{
    out->clear();
    if (zip.size() < 22) return false;
    std::size_t max = std::min<std::size_t>(zip.size() - 22, 65535);
    std::size_t eocd = std::string::npos;
    for (std::size_t off = 0; off <= max; ++off) {
        std::size_t i = zip.size() - 22 - off;
        if (zip[i] == 'P' && zip[i+1] == 'K' && zip[i+2] == 5 && zip[i+3] == 6) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) return false;
    auto u16 = [&](std::size_t i) -> std::uint32_t {
        return zip[i] | (static_cast<std::uint32_t>(zip[i+1]) << 8);
    };
    auto u32 = [&](std::size_t i) -> std::uint32_t {
        return zip[i] | (static_cast<std::uint32_t>(zip[i+1]) << 8)
             | (static_cast<std::uint32_t>(zip[i+2]) << 16)
             | (static_cast<std::uint32_t>(zip[i+3]) << 24);
    };
    std::uint32_t cd_size   = u32(eocd + 12);
    std::uint32_t cd_offset = u32(eocd + 16);
    std::uint32_t entries   = u16(eocd + 10);
    if (cd_offset + cd_size > zip.size()) return false;

    std::size_t i = cd_offset;
    for (std::uint32_t n = 0; n < entries; ++n) {
        if (i + 46 > zip.size()) return false;
        if (!(zip[i] == 'P' && zip[i+1] == 'K' && zip[i+2] == 1 && zip[i+3] == 2))
            return false;
        Entry e;
        e.method       = u16(i + 10);
        e.comp_size    = u32(i + 20);
        e.uncomp_size  = u32(i + 24);
        std::uint16_t name_len  = u16(i + 28);
        std::uint16_t extra_len = u16(i + 30);
        std::uint16_t cmt_len   = u16(i + 32);
        e.local_offset = u32(i + 42);
        if (i + 46 + name_len > zip.size()) return false;
        e.name.assign(reinterpret_cast<const char*>(&zip[i + 46]), name_len);
        out->push_back(std::move(e));
        i += 46 + name_len + extra_len + cmt_len;
    }
    return true;
}

bool extract(const std::vector<std::uint8_t>& zip,
             const Entry&                     e,
             std::vector<std::uint8_t>*       out)
{
    out->clear();
    if (e.local_offset + 30 > zip.size()) return false;
    auto u16 = [&](std::size_t i) -> std::uint32_t {
        return zip[i] | (static_cast<std::uint32_t>(zip[i+1]) << 8);
    };
    std::uint16_t name_len  = u16(e.local_offset + 26);
    std::uint16_t extra_len = u16(e.local_offset + 28);
    std::size_t data_off = e.local_offset + 30 + name_len + extra_len;
    if (data_off + e.comp_size > zip.size()) return false;

    if (e.method == 0) {
        out->assign(zip.begin() + data_off,
                    zip.begin() + data_off + e.comp_size);
        return true;
    }
    if (e.method != 8) return false;

    z_stream zs{};
    // -MAX_WBITS = raw deflate (no zlib/gzip header/trailer).
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
    zs.next_in  = const_cast<Bytef*>(&zip[data_off]);
    zs.avail_in = static_cast<uInt>(e.comp_size);
    out->resize(e.uncomp_size ? e.uncomp_size : e.comp_size * 4);
    std::size_t produced = 0;
    while (true) {
        if (produced >= out->size()) out->resize(out->size() * 2);
        zs.next_out  = out->data() + produced;
        zs.avail_out = static_cast<uInt>(out->size() - produced);
        int rc = inflate(&zs, Z_NO_FLUSH);
        produced = out->size() - zs.avail_out;
        if (rc == Z_STREAM_END) break;
        if (rc == Z_BUF_ERROR && zs.avail_in == 0) break;
        if (rc != Z_OK) { inflateEnd(&zs); return false; }
    }
    inflateEnd(&zs);
    out->resize(produced);
    return true;
}

const Entry* find(const std::vector<Entry>& dir, const std::string& name)
{
    for (const auto& e : dir) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

} // namespace obn::zip
