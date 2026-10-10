#pragma once

#include "ovl/extract/IResourceExtractor.hpp"
#include "ovl/OvlTypes.hpp"

#include <cstdint>
#include <vector>

namespace ovl {

// Extractor for FlexiTexture (ftx) and Texture (tex) loaders. Both decode to a
// 32-bit BGRA .tga at the largest mip level (see docs §3 and §4):
//   - ftx: 8-bit palette-indexed pixels (palette in the header block, indices
//     in a separate chunk reached via an internal offset). Also writes the raw
//     header block to <name>.ovltex and a <name>.json metadata sidecar.
//   - tex: a page of the pair's bitmap table (`btbl`), reached through the
//     tex → flic → btbl chain and the per-loader extra-data stream (docs
//     §4.1). DXT1 / DXT3 / DXT5 and A8R8G8B8 pages decode; every tex symbol
//     maps to its own page, so multi-texture OVLs (the GUI) work too.
class TextureExtractor : public IResourceExtractor {
public:
    std::string_view name() const override { return "texture"; }
    ExtractResult extract(const OvlParser& parser, const ExtractContext& ctx) override;

    // Extract only the linked file whose symbol (case-insensitive) matches
    // `symbol_lc`. Returns true if a matching texture was processed (or was
    // already extracted and `ctx.overwrite` was false). Used by
    // ModelExtractor's auto-textures path to pull in cross-OVL textures
    // referenced by a shs without dumping every texture in the source OVL.
    static bool extract_symbol(const OvlParser& parser,
                               const std::string& symbol_lc,
                               const ExtractContext& ctx);

    // Decode the FlexiTexture (`ftx`) whose header sits at internal offset
    // `datapointer` to BGRA pixels (docs §3). Follows the in-header pointers
    // to the palette, the 1-byte index plane and — when present — the
    // separate 8-bit alpha plane. `has_alpha` is set when an alpha plane was
    // found; otherwise every pixel gets alpha 255. `recolorable` receives the
    // 3-bit remap-channel mask (1/2/4 = first/second/third colour).
    // `recolor` (3 swatch indices 0..31, others = keep) recolours the remap
    // bands with the game's FlexiColour ramps (docs §3.8).
    static bool decode_ftx_bgra(const OvlParser& parser, std::uint32_t datapointer,
                                std::uint32_t& width, std::uint32_t& height,
                                std::vector<std::uint8_t>& bgra,
                                bool* has_alpha = nullptr,
                                std::uint32_t* recolorable = nullptr,
                                const int* recolor = nullptr);

    // Decode linked file `lf_index` on `side` whatever its texture tag (ftx →
    // decode_ftx_bgra, tex → decode_tex_bgra). False for non-texture loaders.
    static bool decode_linked_bgra(const OvlParser& parser, OvlSide side,
                                   std::size_t lf_index,
                                   std::uint32_t& width, std::uint32_t& height,
                                   std::vector<std::uint8_t>& bgra,
                                   bool* has_alpha = nullptr,
                                   std::uint32_t* recolorable = nullptr,
                                   const int* recolor = nullptr);

    // Decode the `tex` linked file `lf_index` declared on `declaring_side` to
    // mip-0 BGRA pixels (row-major, 4 B/px) by following its chain to the
    // bitmap-table page (docs §4.1). False when the chain is broken or the
    // page format is unsupported. Used by AtlasExtractor to slice gsi sprites.
    static bool decode_tex_bgra(const OvlParser& parser, OvlSide declaring_side,
                                std::size_t lf_index,
                                std::uint32_t& width, std::uint32_t& height,
                                std::vector<std::uint8_t>& bgra);
};

}  // namespace ovl
