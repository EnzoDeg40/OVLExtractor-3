#pragma once

#include "ovl/extract/IResourceExtractor.hpp"

namespace ovl {

// `ter` (TerrainType, 60 B, docs §19) → one <ovl>.terrain.json per pair: each
// ground or cliff surface the land tools paint, with its texture symbol, its
// interface name and icon, its colours and the world size its texture covers.
// The pixels themselves come from `--types texture` (the `tex` it points to).
class TerrainExtractor : public IResourceExtractor {
public:
    std::string_view name() const override { return "terrain"; }
    ExtractResult extract(const OvlParser& parser, const ExtractContext& ctx) override;
};

}  // namespace ovl
