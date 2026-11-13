// Layouts of the anims group, see Layout.hpp and docs/RCT3_OVL_FORMAT.md.
//
//   san        SimpleAnim          people head/neck keyframes (Characters/*/Shared/Head_*)
//   sal        SimpleAnimList      the bones and the san list of one head
//   modelanim  ModelAnim           wild-animal skeletal clip (WildAnimals/*_anims)
//   wad        WildAnimalAnimData  the clip set + locomotion data of one animal
//   mdl        Model               wild-animal skinned model (WildAnimals/*_data)
#include "ovl/extract/Layout.hpp"

#include <cstring>
#include <map>

namespace ovl::layout {

namespace {

std::string hex(std::uint32_t x) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%X", x);
    return b;
}

// Fails (issue) when [a, a+n) holds a pointer / symbol-reference slot or
// crosses its block end: keyframe payloads are plain numbers.
bool plain(Ctx& c, std::uint32_t a, std::uint32_t n, const std::string& where) {
    if (c.view().block_remaining(a) < n) {
        c.issue(where, "payload of " + std::to_string(n) + " B crosses its block end");
        return false;
    }
    if (!c.view().slots_in(a, n).empty()) c.issue(where, "pointer slot inside a keyframe payload");
    c.cover(a, n);
    return true;
}

Json vec(const PairView& v, std::uint32_t a, int n) {
    Json j = Json::array();
    for (int i = 0; i < n; ++i) j.push(Json(v.f32(a + 4u * std::uint32_t(i))));
    return j;
}

std::int64_t geti(const Json& so_far, const char* k) {
    const Json* j = so_far.get(k);
    return j && j->is_int() ? j->as_int() : 0;
}

// ---- san -----------------------------------------------------------------
// Value record (24 B): f32 translation[3], i16 x_axis[3], i16 y_axis[3]
// (unit vectors * 32767: columns 0 and 1 of the bone rotation, z = x × y).
// Records are key-major: record (key k, channel c) = values + (k*channels + c)*24.
Json san_channels(Ctx& c, std::uint32_t fa, std::uint32_t, const Json& so) {
    const std::int64_t keys = geti(so, "key_count"), chans = geti(so, "channel_count");
    auto p = c.pointer(fa, c.path + "+0xC");
    Json out = Json::array();
    if (!p) return out;
    const auto& v = c.view();
    if (!plain(c, *p, std::uint32_t(keys * chans * 24), c.path + "+0xC")) return out;
    const double s = 1.0 / 32767.0;
    for (std::int64_t ch = 0; ch < chans; ++ch) {
        Json tr = Json::array(), xa = Json::array(), ya = Json::array();
        for (std::int64_t k = 0; k < keys; ++k) {
            const std::uint32_t r = *p + std::uint32_t((k * chans + ch) * 24);
            tr.push(vec(v, r, 3));
            Json x = Json::array(), y = Json::array();
            for (int i = 0; i < 3; ++i) {
                x.push(Json(std::int16_t(v.u16(r + 12 + 2u * std::uint32_t(i))) * s));
                y.push(Json(std::int16_t(v.u16(r + 18 + 2u * std::uint32_t(i))) * s));
            }
            xa.push(std::move(x));
            ya.push(std::move(y));
        }
        Json o = Json::object();
        o.set("translation", std::move(tr));
        o.set("x_axis", std::move(xa));
        o.set("y_axis", std::move(ya));
        out.push(std::move(o));
    }
    return out;
}

// ---- modelanim -------------------------------------------------------------
// The single extra-data chunk holds the bone names, NUL-terminated, in the order
// the loader assigns them: translation bones (+0x18), rotation bones (+0x1C),
// list 3 (+0x20), then name pairs (+0x24).
std::vector<std::string> extra_names(const Ctx& c, std::size_t chunk) {
    std::vector<std::string> out;
    if (!c.object || c.object->extra.size() <= chunk) return out;
    const Bytes& x = c.object->extra[chunk];
    std::size_t i = 0;
    while (i < x.size()) {
        std::string s;
        while (i < x.size() && x[i]) s += char(x[i++]);
        ++i;
        out.push_back(std::move(s));
    }
    return out;
}

// u32 name-id slots (written by the loader, 0 on disk) -> bone names from the extra chunk.
CustomFn name_slots(const char* count_field, std::size_t first_after_counts_mask, int per) {
    return [count_field, first_after_counts_mask, per](Ctx& c, std::uint32_t fa, std::uint32_t, const Json& so) -> Json {
        const std::int64_t n = geti(so, count_field) * per;
        // names before this list: sum of the earlier lists selected by the mask bits
        static const char* counts[4] = {"translation_bone_count", "rotation_bone_count", "list3_count", "name_pair_count"};
        std::int64_t skip = 0;
        for (int i = 0; i < 4; ++i)
            if (first_after_counts_mask & (1u << i)) skip += geti(so, counts[i]) * (i == 3 ? 2 : 1);
        const std::string where = c.path + "+" + hex(fa);
        auto p = c.pointer(fa, where);
        Json out = Json::array();
        if (!p) {
            if (n) c.issue(where, "null name list with count " + std::to_string(n));
            return out;
        }
        if (!plain(c, *p, std::uint32_t(n * 4), where)) return out;
        for (std::int64_t i = 0; i < n; ++i)
            if (c.view().u32(*p + 4u * std::uint32_t(i))) { c.issue(where, "name-id slot not 0 on disk"); break; }
        auto names = extra_names(c, 0);
        for (std::int64_t i = 0; i < n; ++i) {
            const std::size_t k = std::size_t(skip + i);
            out.push(k < names.size() ? Json(names[k]) : Json());
            if (k >= names.size()) c.issue(where, "extra chunk has too few names");
        }
        return out;
    };
}

// Frame-major keyframes: frame f, bone b at base + (f*bones + b)*stride; output per bone.
CustomFn frames(const char* bones_field, const char* names_field, int comps) {
    return [bones_field, names_field, comps](Ctx& c, std::uint32_t fa, std::uint32_t, const Json& so) -> Json {
        const std::int64_t nf = geti(so, "frame_count"), nb = geti(so, bones_field);
        const std::string where = c.path + "+" + hex(fa);
        auto p = c.pointer(fa, where);
        Json out = Json::array();
        if (!p) {
            if (nf * nb) c.issue(where, "null keyframes");
            return out;
        }
        const std::uint32_t stride = std::uint32_t(comps) * 4;
        if (!plain(c, *p, std::uint32_t(nf * nb) * stride, where)) return out;
        const Json* names = so.get(names_field);
        for (std::int64_t b = 0; b < nb; ++b) {
            Json o = Json::object();
            o.set("bone", names && names->is_array() && std::size_t(b) < names->arr().size() ? names->arr()[std::size_t(b)] : Json());
            Json k = Json::array();
            for (std::int64_t f = 0; f < nf; ++f) k.push(vec(c.view(), *p + std::uint32_t(f * nb + b) * stride, comps));
            o.set("keys", std::move(k));
            out.push(std::move(o));
        }
        return out;
    };
}

// ---- mdl -------------------------------------------------------------------
// Extra chunk 0: the model image the loader (fgdk3 ModelRes, Model.cpp) relocates
// in place; chunk 1: the bone names. Walk mirrors the Mac loader (FUN_100255a02 /
// FUN_1002560f6): every section is checked to end exactly at the chunk end.
class Reader {
public:
    Reader(const Bytes& b) : b_(b) {}
    bool ok(std::size_t o, std::size_t n) const { return o + n <= b_.size(); }
    std::uint32_t u32(std::size_t o) const { std::uint32_t x = 0; if (ok(o, 4)) std::memcpy(&x, &b_[o], 4); return x; }
    std::uint16_t u16(std::size_t o) const { std::uint16_t x = 0; if (ok(o, 2)) std::memcpy(&x, &b_[o], 2); return x; }
    std::uint8_t u8(std::size_t o) const { return ok(o, 1) ? b_[o] : 0; }
    float f32(std::size_t o) const { float x = 0; if (ok(o, 4)) std::memcpy(&x, &b_[o], 4); return x; }
    Json floats(std::size_t o, int n) const { Json j = Json::array(); for (int i = 0; i < n; ++i) j.push(Json(f32(o + 4u * std::size_t(i)))); return j; }
    std::size_t size() const { return b_.size(); }
private:
    const Bytes& b_;
};

std::size_t al(std::size_t x, std::size_t a) { return (x + a - 1) & ~(a - 1); }

Json u16o(std::uint16_t x) { return x == 0xFFFF ? Json(-1) : Json(std::uint32_t(x)); }

Json mdl_model(Ctx& c, std::uint32_t fa, std::uint32_t base, const Json&) {
    const auto& v = c.view();
    const std::string where = c.path + "+0x30";
    for (std::uint32_t o = 0; o < 0x20; o += 4)
        if (v.u32(fa + o)) { c.issue(where, "runtime slot +" + hex(0x30 + o) + " not 0 on disk"); break; }
    Json out = Json::object();
    if (!c.object || c.object->extra.size() != 2) { c.issue(where, "expected 2 extra-data chunks"); return out; }
    const Reader d(c.object->extra[0]);
    const std::vector<std::string> names = extra_names(c, 1);
    const std::uint32_t lods = v.bytes(base + 4, 1)[0], mats = v.u16(base + 6);
    if (d.size() < 16) { c.issue(where, "model chunk too small"); return out; }
    const std::uint32_t nb = d.u32(0), n1 = d.u32(4), n2 = d.u32(8), n3 = d.u32(12);
    if (n1 || n2 || n3) c.issue(where, "unsupported name/aux lists in the model header");
    if (names.size() != nb) c.issue(where, "bone-name chunk holds " + std::to_string(names.size()) + " names for " + std::to_string(nb) + " bones");
    // bones: 0x60 each, then (parent, next) u16 pairs
    std::size_t q = 16;
    const std::size_t par = q + nb * 0x60;
    if (!d.ok(q, nb * 0x64)) { c.issue(where, "bone table past chunk end"); return out; }
    Json bones = Json::array();
    for (std::uint32_t i = 0; i < nb; ++i) {
        const std::size_t r = q + i * 0x60;
        Json b = Json::object();
        b.set("name", i < names.size() ? Json(names[i]) : Json());
        b.set("parent", u16o(d.u16(par + 4 * i)));
        b.set("next", u16o(d.u16(par + 4 * i + 2)));
        b.set("position", d.floats(r, 4));
        b.set("rotation", d.floats(r + 0x10, 4));
        b.set("inverse_bind_matrix", d.floats(r + 0x20, 16));
        bones.push(std::move(b));
    }
    q = al(par + nb * 4, 16);
    // material names: u32 size table, then the strings
    Json materials = Json::array();
    const std::size_t sizes = q;
    q += mats * 4;
    for (std::uint32_t i = 0; i < mats; ++i) {
        const std::uint32_t n = d.u32(sizes + 4 * i);
        std::string s;
        for (std::uint32_t k = 0; k < n && d.u8(q + k); ++k) s += char(d.u8(q + k));
        materials.push(s);
        q += n;
    }
    q = al(q, 16);
    const std::size_t lodr = q, lodp = q + lods * 0x14;
    q = lodp + lods * 4;
    for (std::uint32_t l = 0; l < lods; ++l)
        if (d.u32(lodp + 4 * l)) { c.issue(where, "LOD extension record not supported"); return out; }
    q = al(q, 16);
    Json jl = Json::array();
    for (std::uint32_t l = 0; l < lods; ++l) {
        const std::size_t R = lodr + l * 0x14;
        Json lo = Json::object();
        const std::uint16_t nmat = d.u16(R), nmesh = d.u16(R + 2);
        lo.set("bone_count", Json(d.u32(R + 4)));
        if (d.u32(R + 8) || d.u32(R + 12)) c.issue(where, "LOD runtime pointers not 0 on disk");
        if (d.u32(R + 0x10)) { c.issue(where, "LOD bone mask not supported"); return out; }
        const std::size_t M = q;
        q = al(q + nmesh * 0x20, 16);
        Json meshes = Json::array();
        for (std::uint16_t m = 0; m < nmesh; ++m) {
            const std::size_t S = M + m * 0x20;
            const std::uint32_t fmt = d.u32(S), ic = d.u32(S + 4), sets = d.u16(S + 8), vc = d.u16(S + 10);
            Json me = Json::object();
            me.set("format", hex(fmt));
            me.set("vertex_count", Json(vc));
            me.set("index_count", Json(ic));
            me.set("unk_0C", Json(std::uint32_t(d.u16(S + 12))));
            me.set("unk_0E", u16o(d.u16(S + 14)));
            if (d.u32(S + 16) || d.u32(S + 20) || d.u32(S + 24) || d.u32(S + 28)) c.issue(where, "mesh runtime slots not 0 on disk");
            if (fmt != 0x1305 || sets != 1) { c.issue(where, "unsupported vertex format " + hex(fmt) + " / index sets " + std::to_string(sets)); return out; }
            // 0x1305: f32 pos[3], f32 normal[3], u8 bone[4], u8 weight[4] (/255), u32 colour, f32 uv[2]
            const std::size_t vs = 44;
            if (!d.ok(q, vc * vs + ic * 2)) { c.issue(where, "mesh data past chunk end"); return out; }
            Json pos = Json::array(), nrm = Json::array(), bi = Json::array(), bw = Json::array(), col = Json::array(), uv = Json::array();
            for (std::uint32_t k = 0; k < vc; ++k) {
                const std::size_t x = q + k * vs;
                for (int i = 0; i < 3; ++i) pos.push(Json(d.f32(x + 4u * std::size_t(i))));
                for (int i = 0; i < 3; ++i) nrm.push(Json(d.f32(x + 12 + 4u * std::size_t(i))));
                for (int i = 0; i < 4; ++i) bi.push(Json(std::uint32_t(d.u8(x + 24 + std::size_t(i)))));
                for (int i = 0; i < 4; ++i) bw.push(Json(std::uint32_t(d.u8(x + 28 + std::size_t(i)))));
                char b[16];
                std::snprintf(b, sizeof b, "%08X", d.u32(x + 32));
                col.push(Json(std::string(b)));
                uv.push(Json(d.f32(x + 36)));
                uv.push(Json(d.f32(x + 40)));
            }
            q += vc * vs;
            Json idx = Json::array();
            for (std::uint32_t k = 0; k < ic; ++k) {
                const std::uint16_t i = d.u16(q + 2 * k);
                if (i >= vc) c.issue(where, "index out of range");
                idx.push(Json(std::uint32_t(i)));
            }
            q = al(q + ic * 2, 4);
            me.set("positions", std::move(pos));
            me.set("normals", std::move(nrm));
            me.set("bone_indices", std::move(bi));
            me.set("bone_weights", std::move(bw));
            me.set("colours", std::move(col));
            me.set("uvs", std::move(uv));
            me.set("indices", std::move(idx));
            meshes.push(std::move(me));
        }
        // per-material mesh ranges: 16-B records {u16 material, u16 n_ranges, u16 n_ranges_b, u16 0, ptr, ptr}
        const std::size_t Q = q;
        q += nmat * 0x10;
        Json mr = Json::array();
        for (std::uint16_t k = 0; k < nmat; ++k) {
            const std::size_t r = Q + k * 0x10;
            if (d.u16(r + 6) || d.u32(r + 8) || d.u32(r + 12)) c.issue(where, "material record runtime slots not 0");
            Json e = Json::object();
            e.set("material", Json(std::uint32_t(d.u16(r))));
            for (int list = 0; list < 2; ++list) {
                const std::uint16_t n = d.u16(r + 2 + 2u * std::size_t(list));
                Json a = Json::array();
                for (std::uint16_t j = 0; j < n; ++j) {
                    Json g = Json::object();
                    g.set("first_mesh", Json(std::uint32_t(d.u16(q + 4u * j))));
                    g.set("mesh_count", Json(std::uint32_t(d.u16(q + 4u * j + 2))));
                    a.push(std::move(g));
                }
                q += n * 4;
                e.set(list ? "ranges_b" : "ranges", std::move(a));
            }
            mr.push(std::move(e));
        }
        lo.set("meshes", std::move(meshes));
        lo.set("materials", std::move(mr));
        jl.push(std::move(lo));
    }
    if (q != d.size()) c.issue(where, "model chunk walk ends at " + hex(std::uint32_t(q)) + ", chunk is " + hex(std::uint32_t(d.size())) + " B");
    out.set("bones", std::move(bones));
    out.set("materials", std::move(materials));
    out.set("lods", std::move(jl));
    return out;
}

}  // namespace

void register_anims(Registry& r) {
    // ---- san: SimpleAnim (16 B) ------------------------------------------------
    const Struct& san = r.make("SimpleAnim", 16, {
        f(0x00, "key_count"),
        f(0x04, "channel_count"),  // = bone_count of the owning sal (Bip01 Neck, Bip01 Head, ...)
        arr(0x08, "times", K::Scalars, Count::of("key_count"), K::F32),
        custom(0x0C, "channels", 4, san_channels),
    });
    r.add({"san", "SimpleAnim", "anims", [&san](const PairView&, std::uint32_t) { return &san; }});

    // ---- sal: SimpleAnimList (40 B) -----------------------------------------------
    const Struct& sal = r.make("SimpleAnimList", 40, {
        f(0x00, "bone_count"),
        arr(0x04, "bone_names", K::StrArray, Count::of("bone_count")),
        arr(0x08, "bone_parents", K::Scalars, Count::of("bone_count"), K::I32),
        zero(0x0C), zero(0x10), zero(0x14), zero(0x18),
        f(0x1C, "anim_count"),
        arr(0x20, "anim_names", K::StrArray, Count::of("anim_count")),
        arr(0x24, "anims", K::RefArray, Count::of("anim_count")),
    });
    r.add({"sal", "SimpleAnimList", "anims", [&sal](const PairView&, std::uint32_t) { return &sal; }});

    // ---- modelanim: ModelAnim (88 B) ------------------------------------------------
    // Counts first (fields are decoded in table order; offsets are independent).
    const Struct& ma = r.make("ModelAnim", 88, {
        f(0x00, "duration", K::F32),  // = (frame_count - 1) / 30
        f(0x04, "frame_count"),
        f(0x18, "translation_bone_count"),
        f(0x1C, "rotation_bone_count"),
        f(0x20, "list3_count"),
        f(0x24, "name_pair_count"),
        custom(0x08, "translation_bones", 4, name_slots("translation_bone_count", 0x0, 1)),
        custom(0x0C, "rotation_bones", 4, name_slots("rotation_bone_count", 0x1, 1)),
        custom(0x10, "list3", 4, name_slots("list3_count", 0x3, 1)),
        custom(0x14, "name_pairs", 4, name_slots("name_pair_count", 0x7, 2)),
        custom(0x28, "translations", 4, frames("translation_bone_count", "translation_bones", 3)),
        custom(0x2C, "rotations", 4, frames("rotation_bone_count", "rotation_bones", 4)),
        zero(0x30), zero(0x34), zero(0x38), zero(0x3C), zero(0x40),
        zero(0x44), zero(0x48), zero(0x4C), zero(0x50), zero(0x54),
    });
    r.add({"modelanim", "ModelAnim", "anims", [&ma](const PairView&, std::uint32_t) { return &ma; }});

    // ---- wad: WildAnimalAnimData (0x38 + 4 * anim_count B) -------------------------
    auto wad_inline = [](Ctx& c, std::uint32_t fa, std::uint32_t, const Json& so) -> Json {
        Json out = Json::array();
        const std::int64_t n = geti(so, "anim_count");
        for (std::int64_t i = 0; i < n; ++i) {
            const std::uint32_t s = fa + 4u * std::uint32_t(i);
            c.touch(s);
            if (auto ref = c.view().symref(s)) out.push(*ref);
            else {
                if (c.view().u32(s) || c.view().is_reloc(s)) c.issue(c.path + "+0x38", "anim slot is not a symbol reference");
                out.push(Json());
            }
        }
        return out;
    };
    auto wad_self = [](Ctx& c, std::uint32_t fa, std::uint32_t base, const Json&) -> Json {
        auto p = c.pointer(fa, c.path + "+0xC");
        if (!p || *p != base + 0x38) c.issue(c.path + "+0xC", "anims pointer does not point at +0x38 of the struct");
        return Json(0x38);
    };
    auto wad_struct = [&r, wad_inline, wad_self](std::uint32_t n) -> const Struct* {
        static std::map<std::uint32_t, const Struct*> cache;
        auto it = cache.find(n);
        if (it != cache.end()) return it->second;
        const Struct& s = r.make("WildAnimalAnimData", 0x38 + 4 * n, {
            f(0x00, "head_look_neck_chain"),  // bones from Bip01 Neck up its parent chain that share the head turn
            f(0x04, "head_look_head_chain"),  // bones from Bip01 Head down that share it
            f(0x08, "anim_count"),
            custom(0x0C, "anims_offset", 4, wad_self),
            arr(0x10, "anim_durations", K::Scalars, Count::of("anim_count"), K::F32),
            arr(0x14, "anim_speeds", K::Scalars, Count::of("anim_count"), K::F32),
            f(0x18, "unk_18", K::F32),
            f(0x1C, "unk_1C"),
            f(0x20, "unk_20", K::F32),
            f(0x24, "unk_24", K::F32),
            f(0x28, "run_blend_in_speed", K::F32),
            f(0x2C, "run_blend_out_speed", K::F32),
            f(0x30, "unk_30", K::F32),
            f(0x34, "unk_34", K::F32),
            custom(0x38, "anims", 4 * n, wad_inline),
        });
        cache[n] = &s;
        return &s;
    };
    r.add({"wad", "WildAnimalAnimData", "anims", [wad_struct](const PairView& v, std::uint32_t a) {
               const std::uint32_t n = v.u32(a + 8);
               return n < 4096 ? wad_struct(n) : nullptr;
           }});
    // ---- mdl: Model (80 B) + 2 extra-data chunks -------------------------------------
    const Struct& mdl = r.make("Model", 80, {
        zero(0x00, 2),
        f(0x02, "flags", K::U16),               // 1 on disk; bits 2/4/8/0x10 are runtime load stages
        f(0x04, "lod_count", K::U8),
        f(0x05, "resident_lod_count", K::U8),   // == lod_count: every LOD is in the model chunk
        f(0x06, "material_count", K::U16),
        arr(0x08, "bounding_sphere", K::InlineScalars, Count::n(4), K::F32),  // centre xyz, radius
        arr(0x18, "bbox_min", K::InlineScalars, Count::n(3), K::F32),
        arr(0x24, "bbox_max", K::InlineScalars, Count::n(3), K::F32),
        custom(0x30, "model", 0x20, mdl_model),  // +0x30..+0x4F runtime handles, 0 on disk
    });
    r.add({"mdl", "Model", "anims", [&mdl](const PairView&, std::uint32_t) { return &mdl; }});
}

}  // namespace ovl::layout
