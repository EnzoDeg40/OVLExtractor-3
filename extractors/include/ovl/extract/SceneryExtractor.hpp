#pragma once

#include "ovl/extract/IResourceExtractor.hpp"

namespace ovl {

// Packages scenery items — stalls, shops, rides' static parts, decorations —
// as self-contained glTF 2.0 binaries (docs §14). For each SceneryItemVisual
// (`svd`) in the OVL pair it follows the LOD table to the shs/bsh meshes,
// their (ftx, txs) materials and the ban animations, and writes:
//   <svd>.glb          most detailed LOD: mesh + embedded PNG textures +
//                      materials (opaque / alpha-mask / blend from txs) +
//                      skeleton and animations when the LOD is a bsh
//   <svd>_<lod>.glb    every other LOD (only with ExtractContext::all_lods)
//   <svd>.json         manifest: LOD names, meshes, switch distances,
//                      animations, materials, unresolved textures
// Meshes no svd references are written on their own as <mesh>.glb.
// Textures are looked up in the same OVL pair first, then through
// ExtractContext::texture_index (needs its assets root set).
// Geometry is converted from Direct3D's left-handed frame to glTF's
// right-handed one by mirroring Z.
class SceneryExtractor : public IResourceExtractor {
public:
    std::string_view name() const override { return "scenery"; }
    ExtractResult extract(const OvlParser& parser, const ExtractContext& ctx) override;
};

}  // namespace ovl
