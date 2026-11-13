#pragma once

// Decomposes an OVL pair into what a type writer supplies (block payloads,
// which fields are pointers, strings, symbols, loader references with their
// extra-data chunks, symbol references) and hands it to an OvlBuilder, which
// regenerates the container. With no hooks, clone_pair(im).build() gives the
// original bytes back for 7 403 / 7 476 pairs of the game (ovl_roundtrip
// --rebuild). The hooks turn that into a cloner: rename every string and
// symbol (a new object set that does not collide with the game's), rewrite the
// pair's file references, patch block payloads (texture palettes, pixels) and
// loader extra data (tex pages) before they are laid out again.
//
// The returned builder can take more objects (encode_sid, add_texts…) before
// build().

#include "ovl/OvlBuilder.hpp"
#include "ovl/OvlImage.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ovl {

struct CloneHooks {
    // Every string of the pair: symbol names ("Name:tag"), symbol-reference
    // targets and strings pointed to by data.
    std::function<std::string(const std::string&)> rename;
    // A string pointer at `slot` (original address) after `rename`: lets a
    // caller point one field at another string.
    std::function<std::string(std::uint32_t slot, const std::string&)> string_pointer;
    std::function<std::string(const std::string&)> rename_reference;  // file references
    // A data block, by its original start address in the pair's address space.
    std::function<void(std::uint32_t start, Bytes& data)> patch_block;
    // A loader reference's extra-data chunks, by side and loader index.
    std::function<void(OvlSide side, std::uint32_t loader, std::vector<Bytes>& chunks)> patch_extra;
    // Keep the source symbol / symbol-reference order (default: the game's
    // sorted order, recomputed — needed when names change).
    bool keep_order = false;
};

OvlBuilder clone_pair(const std::array<OvlImage, 2>& im, const CloneHooks& hooks = {});

}  // namespace ovl
