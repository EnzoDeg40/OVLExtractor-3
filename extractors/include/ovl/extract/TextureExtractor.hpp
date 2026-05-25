#pragma once

#include "ovl/extract/IResourceExtractor.hpp"

namespace ovl {

// Best-effort extractor for FlexiTexture (ftx) and Texture (tex) loaders.
// The RCT3 texture format is not publicly documented, so this extractor:
//   1. Reads the texture header (format code, width, height, mipmap pointers)
//   2. Locates the actual pixel data block via offset resolution
//   3. Writes the raw pixel payload to <name>.ovltex
//   4. Writes a sidecar <name>.json with parsed header metadata
//   5. If the format looks like a known DXT variant, also writes a .dds
//      attempt (may need format tweaking by the user)
class TextureExtractor : public IResourceExtractor {
public:
    std::string_view name() const override { return "texture"; }
    ExtractResult extract(const OvlParser& parser, const ExtractContext& ctx) override;
};

}  // namespace ovl
