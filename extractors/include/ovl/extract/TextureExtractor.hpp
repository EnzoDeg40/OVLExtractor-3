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
//   - tex: DXT-compressed pixel data in a trailing section after the parsed
//     OVL structures. The format code at header +0x1C selects the decoder —
//     DXT1 (BC1), DXT3 (BC2, explicit alpha) and DXT5 (BC3, interpolated alpha)
//     are all supported. Multi-texture (btbl/flic) OVLs are not yet handled.
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

    // Decode a single-tex `tex` linkedfile (declared on `declaring_side`) to
    // mip-0 BGRA pixels (row-major, top-down, 4 B/px). Picks the data side
    // (opposite the declaration) and the trailing DXT page automatically.
    // Returns false for multi-tex OVLs — the trailing scan is symbol-blind so
    // it can't tell pages apart (docs §4.6) — and for non-DXT/unreadable data.
    // Used by AtlasExtractor to slice gsi sprites from tex-backed atlases.
    static bool decode_tex_bgra(const OvlParser& parser, OvlSide declaring_side,
                                std::uint32_t& width, std::uint32_t& height,
                                std::vector<std::uint8_t>& bgra);
};

}  // namespace ovl
