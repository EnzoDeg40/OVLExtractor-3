#pragma once

#include "ovl/extract/IResourceExtractor.hpp"

namespace ovl {

// `txt` (Text) → one <ovl>.txt.json per pair: {"Symbol": "string", …}. Every
// string is null-terminated UTF-16LE at its data pointer (docs §18) and is
// written as UTF-8. The language files gui/<Language>.common.ovl hold the
// 12 388 strings of the interface; a few other OVLs carry a handful.
class TextExtractor : public IResourceExtractor {
public:
    std::string_view name() const override { return "text"; }
    ExtractResult extract(const OvlParser& parser, const ExtractContext& ctx) override;
};

}  // namespace ovl
