#pragma once

#include "ovl/extract/IResourceExtractor.hpp"

namespace ovl {

// Data-only loader types with a declarative layout (Layout.hpp, registered in
// extractors/src/DataExtractor_*.cpp) -> one <ovl>.data.json per pair:
// {"<tag>": [ {"symbol": ..., fields...}, ... ], ...}. Every field of every
// struct is written; pointers are followed (strings, arrays, sub-structs),
// symbol references become "name:tag". `ovl_layout_check` proves the layouts
// cover every byte and pointer of every object in the game.
class DataExtractor : public IResourceExtractor {
public:
    std::string_view name() const override { return "data"; }
    ExtractResult extract(const OvlParser& parser, const ExtractContext& ctx) override;
};

}  // namespace ovl
