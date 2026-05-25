#pragma once

#include "ovl/extract/IResourceExtractor.hpp"

namespace ovl {

// Splits texture atlases into individual sprite TGAs using GSI loaders.
// A GSI ("GUI Skin Item") loader points to: a parent texture + a 4×u32
// rectangle (left, top, right, bottom) in pixel space. RCT3 packs UI icons,
// signs, and small sprites into shared atlas textures; this extractor cuts
// each sub-region out into its own file named after the GSI symbol.
class AtlasExtractor : public IResourceExtractor {
public:
    std::string_view name() const override { return "atlas"; }
    ExtractResult extract(const OvlParser& parser, const ExtractContext& ctx) override;
};

}  // namespace ovl
