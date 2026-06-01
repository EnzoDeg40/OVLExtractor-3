#pragma once

#include "ovl/extract/IResourceExtractor.hpp"

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
};

}  // namespace ovl
