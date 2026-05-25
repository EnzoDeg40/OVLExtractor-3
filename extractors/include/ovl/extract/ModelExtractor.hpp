#pragma once

#include "ovl/extract/IResourceExtractor.hpp"

namespace ovl {

// Extracts MorphMesh (mms) loaders to Wavefront OBJ.
// One .obj per mms linkedfile, named after the symbol. Skips morph animation
// data (we only export base mesh positions).
class ModelExtractor : public IResourceExtractor {
public:
    std::string_view name() const override { return "model"; }
    ExtractResult extract(const OvlParser& parser, const ExtractContext& ctx) override;
};

}  // namespace ovl
