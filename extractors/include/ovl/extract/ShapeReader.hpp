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
    // bsh only: up to 4 influences — indices into Shape::bones and weights
    // (stored as u8, sum 255). Most scenery uses a single bone at weight 1.
    std::uint8_t  bones[4]   = {0, 0, 0, 0};
    float         weights[4] = {0, 0, 0, 0};
    std::uint32_t color      = 0;  // D3DCOLOR
};

// One sub-mesh: own vertex buffer, sub-mesh-local triangle list, and the
// (ftx, txs) material pair bound to it (order-based, docs §8.5).
struct ShapePart {
    std::vector<ShapeVertex>   verts;
    std::vector<std::uint32_t> indices;
    std::string                ftx_symbol;   // e.g. "BalloonStall:ftx" (may be empty)
    std::string                txs_symbol;   // e.g. "SIOpaque:txs" (may be empty)
    std::uint32_t              transparency = 0;   // +0x0C: 0 opaque, 1 alpha test, 2 blend
    // +0x10 textureFlags. Sub-meshes with (flags & 0x9000) are not drawn:
    // shadow casters / collision geometry (VNStaticSurface_Create's mask,
    // rerct3 svd-lod notes). kNonVisualFlags below.
    std::uint32_t              texture_flags = 0;
};

inline constexpr std::uint32_t kNonVisualFlags = 0x9000;

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

// The symbol resolve (on either side of the pair) that patches the pointer
// slot at internal offset `slot`, as its full "name:tag" symbol; "" if none.
std::string resolved_symbol_at(const OvlParser& parser, std::uint32_t slot);

// The string a `char*` field points to. `slot` is the internal offset of the
// field: a pointer value of 0 is the first string of the table (offset 0)
// when the slot is relocated, and null otherwise (docs §10).
std::string string_at_slot(const OvlParser& parser, std::uint32_t ptr, std::uint32_t slot);

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

// ---------------------------------------------------------------- mms (§7)
// A MorphMesh: every vertex is (position index, normal index, U, V) and every
// keyframe of every morph (= animation) stores all positions and normals.
struct MorphVertex {
    std::uint16_t pos_index = 0;
    std::uint16_t normal_index = 0;
    float         uv[2] = {0, 0};
};

struct Morph {
    std::string         name;            // animation, e.g. "IdleStandard", "Walk"
    float               bias[3] = {0, 0, 0};
    float               scale[3] = {0, 0, 0};
    std::vector<float>  times;           // one per keyframe, seconds
    std::uint32_t       data_off = 0;    // internal offset of keyframe 0
};

struct MorphMesh {
    std::string                 symbol;  // without ":mms"
    std::uint32_t               position_count = 0;
    std::uint32_t               normal_count = 0;
    std::uint32_t               type_flag = 0;
    std::vector<MorphVertex>    verts;
    std::vector<std::uint32_t>  indices;
    std::vector<Morph>          morphs;
    // Bytes per keyframe: positions (u32 each) then normals (3 × s8 each),
    // padded to 4.
    std::uint32_t frame_stride() const {
        return (position_count * 4 + normal_count * 3 + 3) & ~3u;
    }
};

bool read_morph_mesh(const OvlParser& parser, OvlSide side, std::size_t lf_index,
                     MorphMesh& out, std::string& err);

// Positions and normals of every vertex (MorphMesh::verts order) at keyframe
// `frame` of morph `morph`, in model space (Direct3D, like read_shape).
bool morph_pose(const OvlParser& parser, const MorphMesh& mesh, std::size_t morph,
                std::size_t frame, std::vector<ShapeVertex>& out, std::string& err);

// Index of the morph named `name` (case-insensitive), or -1.
int find_morph(const MorphMesh& mesh, const std::string& name);

// ---------------------------------------------------------------- ssk (§20)
// A SimpleSkin: rigid skinned mesh, each bone owns a run of vertices stored in
// that bone's space.
struct SimpleSkinBone {
    std::string   name;            // e.g. "Bip01 Head"
    std::uint32_t vertex_count = 0;
};

struct SimpleSkin {
    std::string                 symbol;   // without ":ssk"
    std::vector<SimpleSkinBone> bones;
    std::vector<ShapeVertex>    verts;
    std::vector<std::uint32_t>  indices;
};

bool read_simple_skin(const OvlParser& parser, OvlSide side, std::size_t lf_index,
                      SimpleSkin& out, std::string& err);

}  // namespace ovl
