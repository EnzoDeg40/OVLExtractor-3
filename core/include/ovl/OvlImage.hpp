#pragma once

// Low-level, lossless view of ONE OVL file (one side of a common/unique pair):
// every field of the container in file order, block payloads as raw bytes.
// read_image() + write_image() round-trip a game file byte for byte; the
// OvlBuilder (OvlBuilder.hpp) produces an OvlImage from symbols, loaders and
// data blocks. Layout reference: docs/RCT3_OVL_FORMAT.md §1 and
// docs/LIBOVLNG_WRITER.md §2.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ovl {

using Bytes = std::vector<std::uint8_t>;

struct ImageLoader {
    std::string   loader;  // e.g. "FGDK"
    std::string   name;    // e.g. "Text"
    std::uint32_t type = 0;
    std::string   tag;     // e.g. "txt"
};

struct ImageChunk {
    std::vector<Bytes> blocks;
    std::uint32_t      count2 = 0;       // v4/v5 repeat the block count; kept verbatim
    std::uint32_t      v5sub1_unknown = 0;
};

struct OvlImage {
    std::uint32_t version = 5;           // 1, 4 or 5
    std::uint32_t headerversion = 2;     // 0 for v1, 2 otherwise
    std::uint32_t v5_subversion = 0;     // v5 only; 1 adds header_v5sub1 + per-chunk words
    Bytes         header_v5sub1;         // raw bytes between subversion and reference count

    std::vector<std::string> references;
    std::uint32_t            loader_header_unknown = 0;
    std::vector<ImageLoader> loaders;
    // v5: one {a, b} pair per loader (libOVLng writes {loader index, count}).
    std::vector<std::array<std::uint32_t, 2>> v5_loader_info;

    std::array<ImageChunk, 9> chunks;

    // v5: the extra-data descriptor struct, without its leading u32 size word
    // (payload.size() is written as that word). {count, 14-byte records…, 0}.
    Bytes                      v5_extra_struct;
    std::vector<std::uint32_t> v5_after_blocks;
    std::array<std::uint32_t, 2> v4_unknown{};

    std::vector<std::uint32_t> relocations;  // file offsets of pointer slots
    // Everything after the relocation table: the loaders' extra-data chunks
    // ({u32 size, bytes} each), then the trailer (0 word, FilesAndLines…).
    Bytes post_relocations;
};

OvlImage read_image(const Bytes& file);
OvlImage read_image_file(const std::filesystem::path& path);
Bytes    write_image(const OvlImage& image);
void     write_image_file(const OvlImage& image, const std::filesystem::path& path);

// djb2 over the lower-cased "name:tag" — the hash stored in v4/v5 symbol and
// symbol-reference records (verified on every symbol of the Mac build).
std::uint32_t symbol_hash(const std::string& name_colon_tag);

}  // namespace ovl
