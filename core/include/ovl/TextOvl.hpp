#pragma once

// `txt` writer: builds a text OVL pair from scratch with OvlBuilder.
// Layout as in the game's gui/<Language> files (docs/RCT3_OVL_FORMAT.md §18):
// UTF-16LE NUL-terminated strings packed into common chunk-0 blocks closed at
// 16 KB, one symbol + one loader reference per string, all in the common file;
// the first loader carries the {u32 1} extra chunk (and its v5 record), as
// libOVLng's ovlTXTManager does.

#include "ovl/OvlImage.hpp"

#include <array>
#include <string>
#include <vector>

namespace ovl {

struct TextEntry {
    std::string    name;  // symbol name without ":txt"
    std::u16string text;
};

// Entries are written in name order (std::map order, as libOVLng).
std::array<OvlImage, 2> build_text_ovl(std::vector<TextEntry> entries, std::uint32_t version = 5);

// Reads a flat JSON object {"Name": "text", ...} (what `--types text` writes).
std::vector<TextEntry> read_text_json(const std::string& json);

}  // namespace ovl
