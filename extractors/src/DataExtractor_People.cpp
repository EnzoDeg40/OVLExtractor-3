// Layouts of the people group, see Layout.hpp and docs/RCT3_OVL_FORMAT.md
// (§20 people, ManifoldMesh). Evidence: consumers in the Mac build
// (decompile/ghidra/decompiled.c), addresses quoted per field. The Mac build
// keeps the 32-bit on-disk structs and turns each 32-bit slot into a pointer
// with FUN_1001f2550, so the offsets read there are the on-disk offsets.
#include "ovl/extract/Layout.hpp"

namespace ovl::layout {

void register_people(Registry& r) {
    // ---- prt PeoplePart (20 B) ------------------------------------------------
    // Loader FUN_100635c78 (load) / FUN_100635e5a (unload); draw
    // FUN_1006359d8 (PeoplePart_TransformAndCopyBatch), FUN_1006a2ef8,
    // FUN_1006a71a2.
    const Struct& prt = r.make("prt", 0x14, {
        f(0x00, "unk_00"),          // 1 in every object; no reader found
        zero(0x04),                 // runtime: loader stores a handle to its 0x161 B bone-remap cache here
        f(0x08, "texture", K::Ref), // bound by FUN_1006a71a2 (+8), sort key in FUN_1006a2ef8
        f(0x0C, "mesh", K::Ref),    // mms: loader reads its bone names, renderer its vertex / index counts
        f(0x10, "palette"),         // index into the person's 5 palette bytes (FUN_1006359d8: UV cell offset)
    });
    // 3 stray objects (Characters/CF/CF01/SS_SkinLegs_CF01, duplicates of the
    // well-formed SS_SkinLegs_CF01_L1..3:prt in CF01_SwimSuitLegs*_Main) use
    // another order: texture at +0, the constant 1 at +0xC, mesh at +0x10.
    const Struct& prt_stray = r.make("prt_stray", 0x14, {
        f(0x00, "texture", K::Ref), zero(0x04), zero(0x08), f(0x0C, "unk_0C"), f(0x10, "mesh", K::Ref),
    });
    r.add({"prt", "PeoplePart", "20.2", [&prt, &prt_stray](const PairView& v, std::uint32_t a) {
        return v.symref(a) ? &prt_stray : &prt;
    }});

    // ---- ppg PeoplePartGroup (0x54 B) ------------------------------------------
    // Readers: FUN_1006a6774 / FUN_1006a68dc / FUN_1006a6b5c / FUN_1006a71a2
    // (draw, pick, attachment, preview), FUN_1006a6d8e (validation text
    // "LOD %d %s Fixed Part %d Unresolved"), FUN_1006a765a / FUN_1006a76b0
    // (heads), FUN_1005df742 (person setup), FUN_1006b893a + FUN_1006b7ffe
    // (part-group registry and weighted random choice).
    Field parts = arr(0x04, "parts", K::RefArray, Count{});
    parts.count.fn = [](const Ctx&, std::uint32_t, const Json& so) {
        return so.get("fixed_count")->as_int() + so.get("optional_count")->as_int();
    };
    const Struct& lod = r.make("ppg_lod", 8, {
        f(0x00, "fixed_count", K::U8),    // always drawn
        f(0x01, "optional_count", K::U8), // drawn when bit i of the person's optional mask is set
        zero(0x02, 2),
        parts,                            // prt refs: fixed parts, then optional ones (null = unresolved)
    });
    const Struct& ppg = r.make("ppg", 0x54, {
        f(0x00, "unk_00"),                // 1, 2 on the Wild! groups (AK01, Smythe, Tiger); no reader found
        f(0x04, "expansion"),             // 0 base game, 1 Soaked! (BobW, Sharky), 2 Wild! (as CarriedItem +4)
        f(0x08, "unk_08"),                // 0/1: 1 on most swimsuit groups and on the lifeguard; no reader found
        f(0x0C, "person_type", K::U8),    // = Person.PersonType (+0x168): 0 child 1 teen 2 adult 3 VIP 4 handyman
                                          // 5 mechanic 6 security 7 entertainer 8 vendor 9 animal keeper
                                          // 10 paramedic 11 park inspector 12 lifeguard
        f(0x0D, "sub_type", K::U8),       // = Person.SubType (+0x169): 0 male, 1 female
        f(0x0E, "slot", K::U8),           // 0 head 1 body 2 legs 3 swimsuit body 4 swimsuit legs
        f(0x0F, "optional_part_count", K::U8),
        f(0x10, "unk_10"),                // 1 on the AF01 / AM05 / CM05 / TM05 heads only; no reader found
        arr(0x14, "optional_part_chances", K::Scalars, Count::of("optional_part_count"), K::F32),
        f(0x18, "weight", K::F32),        // weighted random choice among groups of the same type/sub_type/slot
        sub(0x1C, "lods", K::Inline, lod, Count::n(3)),
        f(0x34, "head", K::Ref),          // phd: detailed head for LOD 0-1 (skipped at LOD >= 2)
        sub(0x38, "swim_lods", K::Inline, lod, Count::n(3)), // used instead of `lods` when UseSwimSuit
        f(0x50, "swim_head", K::Ref),
    });
    r.add({"ppg", "PeoplePartGroup", "20.3", [&ppg](const PairView&, std::uint32_t) { return &ppg; }});

    // ---- phd PeopleHead (0xC0 B) -----------------------------------------------
    // Loader FUN_100626cce / FUN_100626dd8; pose FUN_100626e16; draw
    // FUN_1006a28ea / FUN_1006a71a2; attachments FUN_1006a6b5c.
    const Struct& faces = r.make("phd_lod", 0x18, {
        f(0x00, "neutral", K::Ref), f(0x04, "angry", K::Ref), f(0x08, "happy", K::Ref),
        f(0x0C, "sad", K::Ref), f(0x10, "scared", K::Ref), f(0x14, "sick", K::Ref),
    });
    const Struct& phd = r.make("phd", 0xC0, {
        // Row-major 4x4, relative to the sal's "Bip01 Head" bone: attachment point 2 (hat) and 3 (face).
        arr(0x00, "hat_attachment", K::InlineScalars, Count::n(16), K::F32),
        arr(0x40, "face_attachment", K::InlineScalars, Count::n(16), K::F32),
        f(0x80, "unk_80"),                // 1 in every object; no reader found
        zero(0x84),                       // runtime: loader stores a handle to its 0x160 B cache here
        f(0x88, "texture", K::Ref),       // skin + hair texture bound for the ssk
        f(0x8C, "animation", K::Ref),     // sal: the head's bone animations ("Bip01 Head" looked up by name)
        sub(0x90, "lods", K::Inline, faces, Count::n(2)), // [person LOD 0/1][expression]
    });
    r.add({"phd", "PeopleHead", "20.4", [&phd](const PairView&, std::uint32_t) { return &phd; }});

    // ---- ssk SimpleSkin (28 B) -------------------------------------------------
    // FUN_1003fc026 (SimpleSkin_TransformVertices), FUN_1003fc236 (ray pick),
    // FUN_1006a28ea / FUN_1006a71a2 (index copy). Loader is empty.
    const Struct& ssk = r.make("ssk", 0x1C, {
        f(0x00, "bone_count"),
        arr(0x04, "bone_names", K::StrArray, Count::of("bone_count")),
        arr(0x08, "bone_vertex_counts", K::Scalars, Count::of("bone_count"), K::U32),
        f(0x0C, "triangle_count"),
        arr(0x10, "indices", K::Scalars, Count::of("triangle_count", 3), K::U16),
        f(0x14, "vertex_count"),
        // 8 floats per vertex: position, normal (both in the vertex's bone space), U, V.
        arr(0x18, "vertices", K::Scalars, Count::of("vertex_count", 8), K::F32),
    });
    r.add({"ssk", "SimpleSkin", "20.5", [&ssk](const PairView&, std::uint32_t) { return &ssk; }});

    // ---- mam ManifoldMesh (0x30 B) ---------------------------------------------
    // libOVLng manifoldmesh.h; consumer FUN_1004f8d48 slices the mesh with a
    // horizontal plane (vertex y vs a height, edge crossings per triangle).
    CustomFn xyz_pad = [](Ctx& c, std::uint32_t slot, std::uint32_t, const Json& so) -> Json {
        Json out = Json::array();
        const std::int64_t n = so.get("vertex_count")->as_int();
        auto p = c.pointer(slot, c.path + "+0x28");
        if (!p) {
            if (n) c.issue(c.path + "+0x28", "null vertices with a count");
            return out;
        }
        c.cover(*p, std::uint32_t(n) * 16);
        for (std::int64_t i = 0; i < n; ++i) {
            const std::uint32_t e = *p + std::uint32_t(i) * 16;
            for (int k = 0; k < 3; ++k) out.push(c.scalar(K::F32, e + 4 * std::uint32_t(k)));
            if (c.view().u32(e + 12)) c.issue(c.path + "+0x28", "vertex pad word not 0");
        }
        return out;
    };
    const Struct& mam = r.make("mam", 0x30, {
        arr(0x00, "bbox_min", K::InlineScalars, Count::n(3), K::F32), zero(0x0C),
        arr(0x10, "bbox_max", K::InlineScalars, Count::n(3), K::F32), zero(0x1C),
        f(0x20, "vertex_count"),
        f(0x24, "triangle_count"),
        custom(0x28, "vertices", 4, xyz_pad), // x, y, z per vertex (16 B, 4th word 0)
        arr(0x2C, "indices", K::Scalars, Count::of("triangle_count", 3), K::U16),
    });
    r.add({"mam", "ManifoldMesh", "", [&mam](const PairView&, std::uint32_t) { return &mam; }});
}

}  // namespace ovl::layout
