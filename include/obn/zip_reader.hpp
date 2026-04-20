#pragma once

// Minimal read-only ZIP parser. Extracted out of stubs/BambuSource.cpp so
// that both libBambuSource.so (file browser / thumbnails path) and
// libbambu_networking.so (subtask-thumbnail cache) can share the same
// implementation.
//
// Scope is intentionally narrow: seek the EOCD, walk the central
// directory, extract STORED (method 0) or raw-DEFLATE (method 8)
// entries. Zip64, encrypted and other fancier flavours aren't handled
// because Bambu's .3mf files never use them.

#include <cstdint>
#include <string>
#include <vector>

namespace obn::zip {

struct Entry {
    std::string   name;
    std::uint64_t comp_size   = 0;
    std::uint64_t uncomp_size = 0;
    std::uint64_t local_offset = 0;
    std::uint16_t method       = 0;
};

// Parses the central directory of a ZIP blob. Returns false if the EOCD
// record isn't found in the last 64 KB (standard search window) or if
// the central directory is truncated.
bool read_central(const std::vector<std::uint8_t>& zip,
                  std::vector<Entry>*              out);

// Extracts a single entry's bytes into `out`. Supports STORED (method 0)
// and raw DEFLATE (method 8). Returns false on any decode error.
bool extract(const std::vector<std::uint8_t>& zip,
             const Entry&                     e,
             std::vector<std::uint8_t>*       out);

// O(n) lookup by exact name match. Returns nullptr if absent.
const Entry* find(const std::vector<Entry>& dir, const std::string& name);

} // namespace obn::zip
