#pragma once

#include "ovl/OvlTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ovl {

class OvlParser;

// In-memory readers for the mesh-side loaders (docs §8, §13.3, §13.4). They
// return the data exactly as stored in the OVL — Direct3D conventions: left-
// handed, Y-up, row-vector matrices (translation in elements 12..14) — and
// leave any conversion to the writer (OBJ, glTF).

struct ShapeVertex {
    float         pos[3]    = {0, 0, 0};
    float         normal[3] = {0, 0, 0};
    float         uv[2]     = {0, 0};
    std::uint32_t bone      = 0;   // bsh only: index into Shape::bones
    std::uint32_t color     = 0;   // D3DCOLOR (bsh only)
};

// One sub-mesh: own vertex buffer, sub-mesh-local triangle list, and the
// (ftx, txs) material pair bound to it (order-based, docs §8.5).
struct ShapePart {
    std::vector<ShapeVertex>   verts;
    std::vector<std::uint32_t> indices;
    std::string                ftx_symbol;   // e.g. "BalloonStall:ftx" (may be empty)
    std::string                txs_symbol;   // e.g. "SIOpaque:txs" (may be empty)
};

// bsh bone. `parent` is what rct3dump called BoneNumber: the index of the
// parent bone, or -1 for a root. `local` is the bind transform relative to the
// parent (BonePositions1) and `world` the model-space bind transform
// (BonePositions2 == local × parent.world). Vertex positions are stored in
// model space at the bind pose.
struct ShapeBone {
    std::string  name;
    std::int32_t parent = -1;
    float        local[16] = {};
    float        world[16] = {};
};

struct Shape {
    std::string             symbol;     // without ":shs"/":bsh"
    bool                    skinned = false;
    float                   bbox_min[3] = {0, 0, 0};
    float                   bbox_max[3] = {0, 0, 0};
    std::vector<ShapePart>  parts;
    std::vector<ShapeBone>  bones;      // empty for shs
};

// ban keyframe: time + a translation, or an axis-angle rotation vector
// (magnitude = angle in radians) — both relative to the parent bone.
struct AnimKey { float t = 0, x = 0, y = 0, z = 0; };

struct BoneTrack {
    std::string          name;          // matched case-insensitively to ShapeBone::name
    std::vector<AnimKey> translate;
    std::vector<AnimKey> rotate;
};

struct BoneAnimation {
    std::string            symbol;      // without ":ban"
    float                  total_time = 0;
    std::vector<BoneTrack> bones;
};

// One level of detail of a SceneryItemVisual (svd).
struct VisualLod {
    std::uint32_t            mesh_type = 0;   // 0 = shs (static), 3 = bsh (skinned)
    std::string              name;            // LODName string
    std::string              mesh_symbol;     // "foo:shs" / "foo:bsh" (symbol resolve)
    float                    distance = 0;    // LOD switch distance
    std::vector<std::string> animations;      // "foo:ban" symbols, in order
};

struct SceneryVisual {
    std::string            symbol;   // without ":svd"
    std::uint32_t          flags = 0;
    std::vector<VisualLod> lods;
};

// Symbol of linked file `lf_index` on `side`, with or without its ":tag".
std::string linked_symbol(const OvlParser& parser, OvlSide side,
                          std::size_t lf_index, bool keep_tag = false);

// Find a linked file by full symbol ("name:tag", case-insensitive) on either
// side of the pair.
struct LinkedRef { OvlSide side = OvlSide::Common; std::size_t index = 0; };
std::optional<LinkedRef> find_linked(const OvlParser& parser,
                                     const std::string& symbol_with_tag);

// Read a shs (skinned=false) or bsh (skinned=true) linked file.
bool read_shape(const OvlParser& parser, OvlSide side, std::size_t lf_index,
                bool skinned, Shape& out, std::string& err);

bool read_bone_animation(const OvlParser& parser, OvlSide side,
                         std::size_t lf_index, BoneAnimation& out,
                         std::string& err);

// A `txt` resource: null-terminated UTF-16LE at the datapointer, returned as
// UTF-8 ("" on failure). Display names and descriptions live in
// gui/<Language>.common.ovl.
std::string read_text(const OvlParser& parser, OvlSide side, std::size_t lf_index);

bool read_scenery_visual(const OvlParser& parser, OvlSide side,
                         std::size_t lf_index, SceneryVisual& out,
                         std::string& err);

}  // namespace ovl
