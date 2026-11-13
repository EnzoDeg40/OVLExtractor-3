#pragma once

// Decode / encode of the scenery chain: ftx (FlexiTexture), shs (StaticShape),
// svd (SceneryItemVisual) and sid (SceneryItem).
//
// Each type keeps its struct as raw bytes with every pointer / symbol-ref
// field zeroed (so unknown scalar fields survive a round trip untouched) and
// the pointed-to data decoded beside it. Layouts: docs/RCT3_OVL_FORMAT.md §3,
// §8, §13.4 and libOVLng (include/flexitexture.h, staticshape.h,
// sceneryvisual.h, sceneryrevised.h). Encoders place data like the game:
// ftx struct in common chunk 4 with its planes in common chunk 0, shs / svd /
// sid structs in unique chunk 2, shs geometry and sid squares in common
// chunk 4 / 2.

#include "ovl/OvlBuilder.hpp"
#include "ovl/PairView.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace ovl {

// Addresses of the pointer / symbol-ref slots a decoder consumed; used to
// check that it accounted for every slot of the struct it read.
using Touched = std::set<std::uint32_t>;

// ---- ftx -------------------------------------------------------------------
struct FtxFrame {
    Bytes raw;      // 28-byte FlexiTextureStruct, pointers zeroed
    Bytes palette;  // 1024 B, 256 x BGRA
    Bytes texture;  // width x height indices
    std::optional<Bytes> alpha;
    bool operator==(const FtxFrame& o) const { return std::tie(raw, palette, texture, alpha) == std::tie(o.raw, o.palette, o.texture, o.alpha); }
};
struct Ftx {
    Bytes                      raw;      // 36-byte FlexiTextureInfoStruct, pointers zeroed
    std::vector<std::uint32_t> offsets;  // frame sequence
    bool                       has_offsets = true;
    std::vector<FtxFrame>      frames;
    std::uint32_t width() const;
    std::uint32_t height() const;
    bool operator==(const Ftx& o) const { return std::tie(raw, offsets, has_offsets, frames) == std::tie(o.raw, o.offsets, o.has_offsets, o.frames); }
};
Ftx  decode_ftx(const PairView& v, std::uint32_t a, Touched* t = nullptr);
void encode_ftx(OvlBuilder& b, const std::string& name, const Ftx& f);

// ---- shs -------------------------------------------------------------------
struct ShsMesh {
    Bytes       raw;  // 40-byte StaticShapeMesh, pointers / refs zeroed
    std::optional<std::string> ftx, txs;  // "name:ftx", "name:txs"
    Bytes       vertices;  // vertex_count x 36 B
    Bytes       indices;   // index words (x3 for sorted transparent meshes)
    bool operator==(const ShsMesh& o) const { return std::tie(raw, ftx, txs, vertices, indices) == std::tie(o.raw, o.ftx, o.txs, o.vertices, o.indices); }
};
struct Shs {
    Bytes                    raw;  // 56-byte StaticShape, pointers zeroed
    std::vector<ShsMesh>     meshes;
    std::vector<Bytes>       effect_matrices;  // 64 B each
    std::vector<std::string> effect_names;
    bool operator==(const Shs& o) const { return std::tie(raw, meshes, effect_matrices, effect_names) == std::tie(o.raw, o.meshes, o.effect_matrices, o.effect_names); }
};
Shs  decode_shs(const PairView& v, std::uint32_t a, Touched* t = nullptr);
void encode_shs(OvlBuilder& b, const std::string& name, const Shs& s);

// ---- svd -------------------------------------------------------------------
struct SvdLod {
    Bytes                      raw;   // 72-byte SceneryItemVisualLOD, pointers / refs zeroed
    std::string                name;  // lod_name (string table)
    std::optional<std::string> shs, bsh, ftx, txs;
    std::vector<std::string>   animations;  // "name:ban"
    bool operator==(const SvdLod& o) const { return std::tie(raw, name, shs, bsh, ftx, txs, animations) == std::tie(o.raw, o.name, o.shs, o.bsh, o.ftx, o.txs, o.animations); }
};
struct Svd {
    Bytes                      raw;  // 0x34 / 0x38 (Soaked) / 0x3C (Wild) bytes
    std::optional<std::string> proxy;  // Soaked / Wild "name:mam"
    std::vector<SvdLod>        lods;
    bool operator==(const Svd& o) const { return std::tie(raw, proxy, lods) == std::tie(o.raw, o.proxy, o.lods); }
};
Svd  decode_svd(const PairView& v, std::uint32_t a, Touched* t = nullptr);
void encode_svd(OvlBuilder& b, const std::string& name, const Svd& s);

// ---- sid -------------------------------------------------------------------
struct SidSquare {
    Bytes                      raw;   // 20-byte SceneryItemData, pointer zeroed
    std::vector<std::uint32_t> bits;  // block_bits words (empty if no pointer)
    bool operator==(const SidSquare& o) const { return std::tie(raw, bits) == std::tie(o.raw, o.bits); }
};
struct Sid {
    Bytes                      raw;  // 0xD4 / 0xE4 / 0xEC bytes, pointers / refs zeroed
    std::vector<SidSquare>     squares;
    Bytes                      tracks;  // track_struct_count x 88 B
    std::optional<std::string> supports, ovl_path;
    std::vector<std::string>   svds;  // "name:svd"
    std::optional<std::string> icon, group_icon, group_name, name;  // gsi / gsi / txt / txt
    std::vector<std::pair<std::string, std::string>> params;
    std::vector<std::string>   anr_names;
    bool operator==(const Sid& o) const { return std::tie(raw, squares, tracks, supports, ovl_path, svds, icon, group_icon, group_name, name, params, anr_names) == std::tie(o.raw, o.squares, o.tracks, o.supports, o.ovl_path, o.svds, o.icon, o.group_icon, o.group_name, o.name, o.params, o.anr_names); }

    // Scalar fields (offsets in SceneryItem_V).
    std::int32_t cost() const;
    void         set_cost(std::int32_t c);
    std::int32_t removal_cost() const;
    void         set_removal_cost(std::int32_t c);
    std::uint32_t type() const;
};
Sid  decode_sid(const PairView& v, std::uint32_t a, Touched* t = nullptr);
void encode_sid(OvlBuilder& b, const std::string& name, const Sid& s);

// Loader types used by the encoders (registered on first use).
std::uint32_t loader_type(OvlBuilder& b, const std::string& tag);

}  // namespace ovl
