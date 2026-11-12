#include "ovl/extract/ShapeReader.hpp"

#include "ovl/BinaryReader.hpp"
#include "ovl/OvlParser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

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

std::string string_at_slot(const OvlParser& parser, std::uint32_t ptr, std::uint32_t slot) {
    if (ptr == 0 && !parser.is_relocation(slot)) return {};
    std::string s = parser.string_from_offset(ptr);
    return s == "STRINGNOTFOUND" ? std::string() : s;
}

std::string resolved_symbol_at(const OvlParser& parser, std::uint32_t slot) {
    return resolve_at(parser, slot);
}

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
//   bsh vertex 44 B: pos(12) normal(12) bones(4×u8) weights(4×u8) color(4) uv(8);   u16 indices
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
        sr->seek(sr->tell() + 0x0C);
        std::uint32_t transparency = sr->read_u32();
        std::uint32_t texture_flags = sr->read_u32();
        sr->read_u32();
        std::uint32_t svc = sr->read_u32();
        std::uint32_t sic = sr->read_u32();
        std::uint32_t vptr = sr->read_u32();
        std::uint32_t iptr = sr->read_u32();
        sum_vc += svc;
        sum_ic += sic;

        ShapePart part;
        part.transparency = transparency;
        part.texture_flags = texture_flags;
        auto vr = open_at(parser, vptr);
        auto ir = open_at(parser, iptr);
        if (!vr || !ir) { err = "sub-mesh buffer unresolved"; return false; }
        part.verts.resize(svc);
        for (auto& v : part.verts) {
            for (float& f : v.pos) f = vr->read_f32();
            for (float& f : v.normal) f = vr->read_f32();
            if (skinned) {
                // D3D BLENDINDICES (4 × u8) then BLENDWEIGHT (4 × u8, sum 255).
                for (auto& b : v.bones) b = vr->read_u8();
                for (auto& w : v.weights) w = vr->read_u8() / 255.0f;
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
        for (std::uint32_t bi = 0; bi < bone_count; ++bi) {
            auto& b = out.bones[bi];
            std::uint32_t np = br->read_u32();
            b.parent = br->read_i32();
            b.name = string_at_slot(parser, np, bones_ptr + 8 * bi);
            for (float& f : b.local) f = lr->read_f32();
            for (float& f : b.world) f = wr->read_f32();
            if (b.parent >= static_cast<std::int32_t>(bone_count)) b.parent = -1;
        }
        for (const auto& p : out.parts)
            for (const auto& v : p.verts)
                for (int k = 0; k < 4; ++k)
                    if (v.weights[k] > 0 && v.bones[k] >= bone_count) {
                        err = "vertex bone index out of range: " + std::to_string(v.bones[k]) +
                              " >= " + std::to_string(bone_count);
                        return false;
                    }
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
    for (std::uint32_t bi = 0; bi < count; ++bi) {
        auto& b = out.bones[bi];
        std::uint32_t np = br->read_u32();
        std::uint32_t tc = br->read_u32(), tp = br->read_u32();
        std::uint32_t rc = br->read_u32(), rp = br->read_u32();
        b.name = string_at_slot(parser, np, bones_ptr + 20 * bi);
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
        lod.name = string_at_slot(parser, name_ptr, lod_off + 4);
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

// ---------------------------------------------------------------- mms (§7)
// Header (40 B at datapointer):
//   +0x00 u32 vertex count        +0x04 u32 index count (u16 indices)
//   +0x08 u32 position count      +0x0C u32 normal count
//   +0x10 u32 type flag           +0x14 u32 morph count
//   +0x18 ptr vertices (12 B: u16 position index, u16 normal index, f32 U, f32 V)
//   +0x1C ptr u16 indices         +0x20 u32 (0)
//   +0x24 ptr morphs (64 B: f32[3] bias, pad, f32[3] scale, pad, char* name,
//         u32 keyframes, f32* times, ptr keyframe data, ptr attachment, 12 B)
// A keyframe is position_count packed u32 (x = bits 21..31, y = 11..20,
// z = 0..10, each `bias + n × scale`) then normal_count × 3 s8, padded to 4.
bool read_morph_mesh(const OvlParser& parser, OvlSide side, std::size_t lf_index,
                     MorphMesh& out, std::string& err) {
    const auto& lf = parser.side(side).linkedfiles[lf_index];
    out = MorphMesh{};
    out.symbol = linked_symbol(parser, side, lf_index, false);
    auto r = open_at(parser, lf.loaderreference.datapointer);
    if (!r) { err = "mms header unresolved"; return false; }
    const std::uint32_t vcount = r->read_u32();
    const std::uint32_t icount = r->read_u32();
    out.position_count = r->read_u32();
    out.normal_count   = r->read_u32();
    out.type_flag      = r->read_u32();
    const std::uint32_t mcount = r->read_u32();
    const std::uint32_t voff = r->read_u32();
    const std::uint32_t ioff = r->read_u32();
    (void)r->read_u32();
    const std::uint32_t moff = r->read_u32();
    if (vcount == 0 || vcount > 65536 || icount > 600000 || mcount > 4096 ||
        out.position_count == 0 || out.position_count > vcount ||
        out.normal_count > vcount) {
        err = "implausible mms header";
        return false;
    }
    auto rv = open_at(parser, voff);
    if (!rv) { err = "mms vertices unresolved"; return false; }
    out.verts.resize(vcount);
    for (auto& v : out.verts) {
        v.pos_index = rv->read_u16();
        v.normal_index = rv->read_u16();
        v.uv[0] = rv->read_f32();
        v.uv[1] = rv->read_f32();
        if (v.pos_index >= out.position_count ||
            (out.normal_count && v.normal_index >= out.normal_count)) {
            err = "mms vertex index out of range";
            return false;
        }
    }
    auto ri = open_at(parser, ioff);
    if (!ri) { err = "mms indices unresolved"; return false; }
    out.indices.resize(icount);
    for (auto& i : out.indices) {
        i = ri->read_u16();
        if (i >= vcount) { err = "mms index out of range"; return false; }
    }
    for (std::uint32_t m = 0; m < mcount; ++m) {
        const std::uint32_t rec = moff + m * 64;
        auto rm = open_at(parser, rec);
        if (!rm) { err = "mms morph unresolved"; return false; }
        Morph mo;
        for (int k = 0; k < 3; ++k) mo.bias[k] = rm->read_f32();
        (void)rm->read_f32();
        for (int k = 0; k < 3; ++k) mo.scale[k] = rm->read_f32();
        (void)rm->read_f32();
        const std::uint32_t name_ptr = rm->read_u32();
        const std::uint32_t tcount = rm->read_u32();
        const std::uint32_t toff = rm->read_u32();
        mo.data_off = rm->read_u32();
        mo.name = string_at_slot(parser, name_ptr, rec + 32);
        if (tcount > 100000) { err = "implausible keyframe count"; return false; }
        if (auto rt = open_at(parser, toff)) {
            mo.times.resize(tcount);
            for (auto& t : mo.times) t = rt->read_f32();
        } else if (tcount) {
            err = "mms times unresolved";
            return false;
        }
        out.morphs.push_back(std::move(mo));
    }
    return true;
}

int find_morph(const MorphMesh& mesh, const std::string& name) {
    const std::string want = lower(name);
    for (std::size_t i = 0; i < mesh.morphs.size(); ++i)
        if (lower(mesh.morphs[i].name) == want) return static_cast<int>(i);
    return -1;
}

bool morph_pose(const OvlParser& parser, const MorphMesh& mesh, std::size_t morph,
                std::size_t frame, std::vector<ShapeVertex>& out, std::string& err) {
    if (morph >= mesh.morphs.size()) { err = "no such morph"; return false; }
    const Morph& mo = mesh.morphs[morph];
    if (frame >= std::max<std::size_t>(mo.times.size(), 1)) { err = "no such keyframe"; return false; }
    auto r = open_at(parser, mo.data_off + static_cast<std::uint32_t>(frame) * mesh.frame_stride());
    if (!r) { err = "mms keyframe unresolved"; return false; }
    std::vector<std::array<float, 3>> pos(mesh.position_count), nrm(mesh.normal_count);
    for (auto& p : pos) {
        const std::uint32_t w = r->read_u32();
        p[0] = mo.bias[0] + static_cast<float>(w >> 21) * mo.scale[0];
        p[1] = mo.bias[1] + static_cast<float>((w >> 11) & 0x3FF) * mo.scale[1];
        p[2] = mo.bias[2] + static_cast<float>(w & 0x7FF) * mo.scale[2];
    }
    for (auto& n : nrm) {
        float len = 0;
        for (int k = 0; k < 3; ++k) {
            n[k] = static_cast<float>(static_cast<std::int8_t>(r->read_u8()));
            len += n[k] * n[k];
        }
        len = std::sqrt(len);
        if (len > 0) for (auto& c : n) c /= len;
    }
    out.assign(mesh.verts.size(), ShapeVertex{});
    for (std::size_t i = 0; i < mesh.verts.size(); ++i) {
        const auto& mv = mesh.verts[i];
        auto& v = out[i];
        for (int k = 0; k < 3; ++k) v.pos[k] = pos[mv.pos_index][k];
        if (mesh.normal_count)
            for (int k = 0; k < 3; ++k) v.normal[k] = nrm[mv.normal_index][k];
        v.uv[0] = mv.uv[0];
        v.uv[1] = mv.uv[1];
    }
    return true;
}

// ---------------------------------------------------------------- ssk (§20)
// Header (28 B at datapointer):
//   +0x00 u32 bone count          +0x04 char*[bones] bone names
//   +0x08 u32[bones] vertex count per bone (runs, in order)
//   +0x0C u32 triangle count      +0x10 ptr u16 indices (3 per triangle)
//   +0x14 u32 vertex count        +0x18 ptr vertices (8 f32: pos, normal, U, V)
// Vertex positions are in their bone's space.
bool read_simple_skin(const OvlParser& parser, OvlSide side, std::size_t lf_index,
                      SimpleSkin& out, std::string& err) {
    const auto& lf = parser.side(side).linkedfiles[lf_index];
    out = SimpleSkin{};
    out.symbol = linked_symbol(parser, side, lf_index, false);
    auto r = open_at(parser, lf.loaderreference.datapointer);
    if (!r) { err = "ssk header unresolved"; return false; }
    const std::uint32_t bcount = r->read_u32();
    const std::uint32_t names = r->read_u32();
    const std::uint32_t counts = r->read_u32();
    const std::uint32_t tcount = r->read_u32();
    const std::uint32_t ioff = r->read_u32();
    const std::uint32_t vcount = r->read_u32();
    const std::uint32_t voff = r->read_u32();
    if (bcount == 0 || bcount > 256 || vcount > 65536 || tcount > 200000) {
        err = "implausible ssk header";
        return false;
    }
    auto rn = open_at(parser, names);
    auto rc = open_at(parser, counts);
    if (!rn || !rc) { err = "ssk bone table unresolved"; return false; }
    std::uint32_t total = 0;
    for (std::uint32_t b = 0; b < bcount; ++b) {
        SimpleSkinBone bone;
        const std::uint32_t p = rn->read_u32();
        bone.name = string_at_slot(parser, p, names + b * 4);
        bone.vertex_count = rc->read_u32();
        total += bone.vertex_count;
        out.bones.push_back(std::move(bone));
    }
    if (total != vcount) { err = "ssk bone runs don't add up to the vertex count"; return false; }
    auto rv = open_at(parser, voff);
    auto ri = open_at(parser, ioff);
    if (!rv || !ri) { err = "ssk mesh unresolved"; return false; }
    out.verts.resize(vcount);
    std::uint8_t bone = 0;
    std::uint32_t left = out.bones[0].vertex_count;
    for (auto& v : out.verts) {
        while (left == 0 && bone + 1u < bcount) left = out.bones[++bone].vertex_count;
        for (auto& c : v.pos) c = rv->read_f32();
        for (auto& c : v.normal) c = rv->read_f32();
        v.uv[0] = rv->read_f32();
        v.uv[1] = rv->read_f32();
        v.bones[0] = bone;
        v.weights[0] = 1.0f;
        --left;
    }
    out.indices.resize(static_cast<std::size_t>(tcount) * 3);
    for (auto& i : out.indices) {
        i = ri->read_u16();
        if (i >= vcount) { err = "ssk index out of range"; return false; }
    }
    return true;
}

}  // namespace ovl
