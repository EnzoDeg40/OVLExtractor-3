#pragma once

#include "ovl/extract/IResourceExtractor.hpp"

namespace ovl {

// People (guests, staff, VIPs, entertainers — Assets/Characters, docs §20):
// `mms` morph meshes (body/legs/swimsuit parts, low-LOD heads), `ssk` simple
// skins (detailed heads), `prt` people parts, `ppg` part groups and `phd`
// people heads.
class PeopleExtractor : public IResourceExtractor {
public:
    std::string_view name() const override { return "people"; }
    ExtractResult extract(const OvlParser& parser, const ExtractContext& ctx) override;
};

}  // namespace ovl
