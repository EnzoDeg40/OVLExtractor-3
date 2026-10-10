#include "ovl/extract/ShapeReader.hpp"

#include "ovl/BinaryReader.hpp"
#include "ovl/OvlParser.hpp"

#include <algorithm>
#include <cctype>

namespace ovl {

namespace {

// Open a reader positioned at internal offset `off`, or nullopt if `off`
// doesn't resolve to a file position.
std::optional<BinaryReader> open_at(const OvlParser& parser, std::uint32_t off) {
    if (off == 0) return std::nullopt;
    auto p = parser.offset_to_position(off);
    if (!p.found) return std::nullopt;
    BinaryReader r(parser.side(p.currentOVL).ovlname);
    r.seek(p.position);
    return r;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string strip_tag(std::string s) {
    auto cut = s.rfind(':');
    if (cut != std::string::npos) s.resize(cut);
    return s;
}

// The symbol resolve (on either side) that patches the pointer slot at
// internal offset `slot`, as its full "name:tag" symbol; "" if none.
std::string resolve_at(const OvlParser& parser, std::uint32_t slot) {
    for (std::size_t s = 0; s < (parser.has_unique() ? 2u : 1u); ++s) {
        for (const auto& sr : parser.side(static_cast<OvlSide>(s)).symbolresolves)
            if (sr.pointer == slot) return parser.string_from_offset(sr.stringpointer);
    }
    return {};
}

}  // namespace

std::string linked_symbol(const OvlParser& parser, OvlSide side,
                          std::size_t lf_index, bool keep_tag) {
    const auto& lf = parser.side(side).linkedfiles[lf_index];
    std::string s = parser.string_from_offset(lf.symbolresolve.stringpointer);
    return keep_tag ? s : strip_tag(s);
}

std::optional<LinkedRef> find_linked(const OvlParser& parser,
                                     const std::string& symbol_with_tag) {
    const std::string want = lower(symbol_with_tag);
    for (std::size_t s = 0; s < (parser.has_unique() ? 2u : 1u); ++s) {
        auto side = static_cast<OvlSide>(s);
        const auto& d = parser.side(side);
        for (std::size_t i = 0; i < d.linkedfiles.size(); ++i) {
            if (lower(linked_symbol(parser, side, i, true)) == want)
                return LinkedRef{side, i};
        }
    }
    return std::nullopt;
}

// StaticShape1 / BoneShape1 header (docs §8.1, §13.3), at datapointer:
//   +0x00 f32[3] bbox min        +0x0C f32[3] bbox max
//   +0x18 u32 total vertex count +0x1C u32 total index count
//   +0x20 u32 mesh count (×2)    +0x28 ptr  sub-mesh pointer table (0xFFFFFFFF-terminated)
//   bsh only: +0x2C u32 bone count, +0x30 ptr bones {char* name; i32 parent}[],
//             +0x34 ptr local bind matrices, +0x38 ptr model-space bind matrices
// Sub-mesh (StaticShape2/BoneShape2): +0x18 vc, +0x1C ic, +0x20 ptr verts, +0x24 ptr idx.
//   shs vertex 36 B: pos(12) normal(12) color(4) uv(8);   u32 indices
//   bsh vertex 44 B: pos(12) normal(12) bone(4) unk(4) color(4) uv(8);   u16 indices
bool read_shape(const OvlParser& parser, OvlSide side, std::size_t lf_index,
                bool skinned, Shape& out, std::string& err) {
    const auto& d = parser.side(side);
    const auto& lf = d.linkedfiles[lf_index];
    out = Shape{};
    out.symbol = linked_symbol(parser, side, lf_index);
    out.skinned = skinned;

    auto hr = open_at(parser, lf.loaderreference.datapointer);
    if (!hr) { err = "datapointer unresolved"; return false; }
    for (float& f : out.bbox_min) f = hr->read_f32();
    for (float& f : out.bbox_max) f = hr->read_f32();
    std::uint32_t vc = hr->read_u32();
    std::uint32_t ic = hr->read_u32();
    hr->read_u32();
    hr->read_u32();
    std::uint32_t table_ptr = hr->read_u32();
    std::uint32_t bone_count = 0, bones_ptr = 0, local_ptr = 0, world_ptr = 0;
    if (skinned) {
        bone_count = hr->read_u32();
        bones_ptr  = hr->read_u32();
        local_ptr  = hr->read_u32();
        world_ptr  = hr->read_u32();
    }
    if (vc == 0 || ic == 0 || vc > 200000 || ic > 600000) {
        err = "implausible counts v=" + std::to_string(vc) + " i=" + std::to_string(ic);
        return false;
    }

    std::vector<std::uint32_t> sm_ptrs;
    if (auto tr = open_at(parser, table_ptr)) {
        for (int i = 0; i < 64; ++i) {
            std::uint32_t p = tr->read_u32();
            if (p == 0xFFFFFFFFu || p == 0) break;
            sm_ptrs.push_back(p);
        }
    }
    if (sm_ptrs.empty()) { err = "no sub-mesh table"; return false; }

    // Materials: this loader reference's symbol-resolve slice, as consecutive
    // (ftx, txs) pairs in sub-mesh order.
    std::vector<std::string> mats;
    for (const auto& sr : d.symbolresolves)
        if (sr.loadpointer == lf.loaderreference.internal_offset)
            mats.push_back(parser.string_from_offset(sr.stringpointer));

    std::uint32_t sum_vc = 0, sum_ic = 0;
    for (std::size_t s = 0; s < sm_ptrs.size(); ++s) {
        auto sr = open_at(parser, sm_ptrs[s]);
        if (!sr) { err = "sub-mesh pointer unresolved"; return false; }
        sr->seek(sr->tell() + 0x18);
        std::uint32_t svc = sr->read_u32();
        std::uint32_t sic = sr->read_u32();
        std::uint32_t vptr = sr->read_u32();
        std::uint32_t iptr = sr->read_u32();
        sum_vc += svc;
        sum_ic += sic;

        ShapePart part;
        auto vr = open_at(parser, vptr);
        auto ir = open_at(parser, iptr);
        if (!vr || !ir) { err = "sub-mesh buffer unresolved"; return false; }
        part.verts.resize(svc);
        for (auto& v : part.verts) {
            for (float& f : v.pos) f = vr->read_f32();
            for (float& f : v.normal) f = vr->read_f32();
            if (skinned) {
                v.bone = vr->read_u32();
                vr->read_u32();
            }
            v.color = vr->read_u32();
            v.uv[0] = vr->read_f32();
            v.uv[1] = vr->read_f32();
        }
        part.indices.resize(sic);
        for (auto& x : part.indices) x = skinned ? ir->read_u16() : ir->read_u32();
        if (2 * s + 1 < mats.size()) {
            part.ftx_symbol = mats[2 * s];
            part.txs_symbol = mats[2 * s + 1];
        }
        out.parts.push_back(std::move(part));
    }
    if (sum_vc != vc || sum_ic != ic) {
        err = "sub-mesh sum mismatch";
        return false;
    }

    if (skinned) {
        if (bone_count > 256) { err = "implausible bone count"; return false; }
        auto br = open_at(parser, bones_ptr);
        auto lr = open_at(parser, local_ptr);
        auto wr = open_at(parser, world_ptr);
        if (bone_count && (!br || !lr || !wr)) { err = "bone table unresolved"; return false; }
        out.bones.resize(bone_count);
        for (auto& b : out.bones) {
            std::uint32_t np = br->read_u32();
            b.parent = br->read_i32();
            b.name = np ? parser.string_from_offset(np) : std::string();
            for (float& f : b.local) f = lr->read_f32();
            for (float& f : b.world) f = wr->read_f32();
            if (b.parent >= static_cast<std::int32_t>(bone_count)) b.parent = -1;
        }
        for (const auto& p : out.parts)
            for (const auto& v : p.verts)
                if (v.bone >= bone_count) { err = "vertex bone index out of range"; return false; }
    }
    return true;
}

// BoneAnim { u32 BoneCount; BoneAnimBone* Bones; f32 TotalTime; }
// BoneAnimBone { char* Name; u32 TransCount; txyz* Trans; u32 RotCount; txyz* Rot; }
bool read_bone_animation(const OvlParser& parser, OvlSide side,
                         std::size_t lf_index, BoneAnimation& out,
                         std::string& err) {
    const auto& lf = parser.side(side).linkedfiles[lf_index];
    out = BoneAnimation{};
    out.symbol = linked_symbol(parser, side, lf_index);
    auto r = open_at(parser, lf.loaderreference.datapointer);
    if (!r) { err = "datapointer unresolved"; return false; }
    std::uint32_t count = r->read_u32();
    std::uint32_t bones_ptr = r->read_u32();
    out.total_time = r->read_f32();
    if (count == 0 || count > 4096) { err = "implausible bone count"; return false; }
    auto br = open_at(parser, bones_ptr);
    if (!br) { err = "bones pointer unresolved"; return false; }
    auto read_keys = [&](std::uint32_t ptr, std::uint32_t n) {
        std::vector<AnimKey> keys;
        if (n == 0 || n > 1000000) return keys;
        auto kr = open_at(parser, ptr);
        if (!kr) return keys;
        keys.resize(n);
        for (auto& k : keys) {
            k.t = kr->read_f32(); k.x = kr->read_f32();
            k.y = kr->read_f32(); k.z = kr->read_f32();
        }
        return keys;
    };
    out.bones.resize(count);
    for (auto& b : out.bones) {
        std::uint32_t np = br->read_u32();
        std::uint32_t tc = br->read_u32(), tp = br->read_u32();
        std::uint32_t rc = br->read_u32(), rp = br->read_u32();
        b.name = np ? parser.string_from_offset(np) : std::string();
        b.translate = read_keys(tp, tc);
        b.rotate = read_keys(rp, rc);
    }
    return true;
}

std::string read_text(const OvlParser& parser, OvlSide side, std::size_t lf_index) {
    const auto& lf = parser.side(side).linkedfiles[lf_index];
    auto r = open_at(parser, lf.loaderreference.datapointer);
    if (!r) return {};
    std::string out;
    for (int i = 0; i < 4096; ++i) {
        std::uint32_t c = r->read_u16();
        if (c == 0) break;
        if (c >= 0xD800 && c < 0xDC00) {   // surrogate pair
            std::uint32_t lo = r->read_u16();
            c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// SceneryItemVisual (svd), at datapointer:
//   +0x00 u32 flags  +0x04 u32  +0x08 f32 (1.0)  +0x0C f32 (1.0)  +0x10 u32
//   +0x14 u32 LODCount   +0x18 ptr → LODCount × ptr → SceneryItemVisualLOD
// SceneryItemVisualLOD (72 B + inline animation slots):
//   +0x00 u32 MeshType (0 shs, 3 bsh)   +0x04 char* LODName
//   +0x08 ptr StaticShape slot (resolved by symbol)   +0x10 ptr BoneShape slot
//   +0x38 f32 distance   +0x3C u32 AnimationCount   +0x44 ptr → AnimationCount
//   × ptr → ban slot (resolved by symbol)
bool read_scenery_visual(const OvlParser& parser, OvlSide side,
                         std::size_t lf_index, SceneryVisual& out,
                         std::string& err) {
    const auto& lf = parser.side(side).linkedfiles[lf_index];
    out = SceneryVisual{};
    out.symbol = linked_symbol(parser, side, lf_index);
    std::uint32_t base = lf.loaderreference.datapointer;
    auto r = open_at(parser, base);
    if (!r) { err = "datapointer unresolved"; return false; }
    out.flags = r->read_u32();
    r->seek(r->tell() + 0x10);
    std::uint32_t lod_count = r->read_u32();
    std::uint32_t lods_ptr = r->read_u32();
    if (lod_count > 64) { err = "implausible LOD count"; return false; }
    auto lr = open_at(parser, lods_ptr);
    if (lod_count && !lr) { err = "LOD table unresolved"; return false; }
    for (std::uint32_t i = 0; i < lod_count; ++i) {
        std::uint32_t lod_off = lr->read_u32();
        auto l = open_at(parser, lod_off);
        if (!l) { err = "LOD pointer unresolved"; return false; }
        VisualLod lod;
        lod.mesh_type = l->read_u32();
        std::uint32_t name_ptr = l->read_u32();
        lod.name = name_ptr ? parser.string_from_offset(name_ptr) : std::string();
        lod.mesh_symbol = resolve_at(parser, lod_off + 0x08);
        if (lod.mesh_symbol.empty()) lod.mesh_symbol = resolve_at(parser, lod_off + 0x10);
        l->seek(l->tell() + 0x38 - 0x08);
        lod.distance = l->read_f32();
        std::uint32_t anim_count = l->read_u32();
        l->read_u32();
        std::uint32_t anim_arr = l->read_u32();
        if (anim_count > 0 && anim_count < 64) {
            if (auto ar = open_at(parser, anim_arr)) {
                for (std::uint32_t k = 0; k < anim_count; ++k) {
                    std::uint32_t slot = ar->read_u32();
                    std::string sym = resolve_at(parser, slot);
                    if (!sym.empty()) lod.animations.push_back(sym);
                }
            }
        }
        out.lods.push_back(std::move(lod));
    }
    return true;
}

}  // namespace ovl
