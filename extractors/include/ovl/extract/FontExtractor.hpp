#pragma once

#include "ovl/extract/IResourceExtractor.hpp"

namespace ovl {

// `fct` (Font) → <name>.font.json + <name>.tga (docs §17). A font is a Win32
// face description (family, weight, pixel size), a sorted code-point table
// and one 36-byte record per glyph (advance, offsets, rectangle in the glyph
// page); the page is the `tex` the fct resolves to, stored bottom-up. The
// JSON lists every glyph the page pre-renders with its rectangle in upright
// pixel coordinates, and the .tga is the page flipped upright to match.
class FontExtractor : public IResourceExtractor {
public:
    std::string_view name() const override { return "font"; }
    ExtractResult extract(const OvlParser& parser, const ExtractContext& ctx) override;
};

}  // namespace ovl
