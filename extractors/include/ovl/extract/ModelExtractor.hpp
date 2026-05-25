#pragma once

#include "ovl/extract/IResourceExtractor.hpp"

namespace ovl {

// Extracts MorphMesh (mms) and StaticShape (shs) loaders to Wavefront OBJ.
// One .obj per linkedfile, named after the symbol.
//   - mms: base mesh from morph[0] (animation data ignored).
//   - shs: rigid mesh, format pos(3f)+normal(3f)+u32+uv(2f), u32 indices.
class ModelExtractor : public IResourceExtractor {
public:
    std::string_view name() const override { return "model"; }
    ExtractResult extract(const OvlParser& parser, const ExtractContext& ctx) override;
};

}  // namespace ovl
