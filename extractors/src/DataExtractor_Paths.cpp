// Layouts of the paths group, see Layout.hpp and docs/RCT3_OVL_FORMAT.md.
//
//   ptd  PathType           336 B, + PathTypeExtended (268 B) when flags & 0x01000000
//   qtd  QueueType          52 B
//   pty  PoolType           600 B
//   chg  ChangingRoom       12 B -> Attraction_S (64 B)
//   enc  EnclosureType      92 B
//   asd  AnimalShelterDesc  84 B + trailing spline arrays
//   vwg  ViewingGallery     4 B  -> Attraction_W (68 B)
//
// Sources: libOVLng path.h / ManagerPTD / ManagerQTD / stall.h / attraction.h /
// ManagerCHG; rerct3 research/re/loader-structs.md (PathType/QueueType byte
// verification, EnclosureType scenery loop); the Mac game's consumers in
// decompile/ghidra/decompiled.c (all loaders are no-op relocators, the
// consumers read the on-disk struct through FUN_1001f2550 = resolve a 32-bit
// relocated slot):
//   PathType:   FUN_100... path type resolve (~line 1067640): 19x4 pieces at
//               word 6, 16x4 extended pieces at word 0x56 and paving at word
//               0x96 when flags >= 0x01000000, research at +0x148/+0x14C.
//   PoolType:   FUN_100374e50 (89 scenery pieces at +0x0C, research +0x170/+0x174),
//               FUN_10096f6a8 / FUN_10096f90a (segment build cost / refund),
//               pool segment geometry code (+0x1BC, +0x1C4).
//   Research:   FUN_1005729ce / FUN_100572a52 (12-byte entries).
//   Enclosure:  FUN_1004b2... resolve loop (9 pieces), cost/refund readers
//               (~lines 1251049, 1507011), strength (+0x50) picks.
//   Shelter:    AnimalShelter code (~lines 1708290-1713120): hut placement,
//               troughs, food type, invisible proxies, spline switch FUN_100a153ac.
#include "ovl/extract/Layout.hpp"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace ovl::layout {

namespace {

std::string where_of(const Ctx& c, std::uint32_t off) {
    char b[16];
    std::snprintf(b, sizeof b, "+0x%X", off);
    return c.path + b;
}

// `n` string pointers stored in place -> array of strings (null -> null).
Field strs(std::uint32_t off, std::string name, int n) {
    return custom(off, std::move(name), std::uint32_t(4 * n),
                  [off, n](Ctx& c, std::uint32_t fa, std::uint32_t, const Json&) -> Json {
                      Json a = Json::array();
                      for (int i = 0; i < n; ++i) {
                          auto p = c.pointer(fa + std::uint32_t(4 * i), where_of(c, off + std::uint32_t(4 * i)));
                          a.push(p ? Json(c.view().cstr(*p)) : Json());
                      }
                      return a;
                  });
}

// One symbol-reference slot -> "name:tag".
Json ref_at(Ctx& c, std::uint32_t slot, std::uint32_t off) {
    c.touch(slot);
    if (auto s = c.view().symref(slot)) return Json(*s);
    if (std::uint32_t x = c.view().u32(slot)) {
        char b[48];
        std::snprintf(b, sizeof b, "non-symbol reference 0x%08X", x);
        c.issue(where_of(c, off), b);
    }
    return Json();
}

}  // namespace

void register_paths(Registry& r) {
    // ------------------------------------------------------------ Research
    // {category ref, runtime slot (-1 on disk, overwritten by FUN_1005729ce with
    // the category registration), f32 progress the category must reach}.
    const Struct& research = r.make("research", 12, {
        f(0x00, "category", K::Ref),
        f(0x04, "runtime_slot", K::I32),
        f(0x08, "min_progress", K::F32),
    });

    // ------------------------------------------------------------ ptd
    static const char* const kBase[19] = {
        "flat", "straight_a", "straight_b", "turn_u", "turn_la", "turn_lb", "turn_ta",
        "turn_tb", "turn_tc", "turn_x", "corner_a", "corner_b", "corner_c", "corner_d",
        "slope", "slope_straight", "slope_straight_left", "slope_straight_right", "slope_mid",
    };
    static const char* const kExt[16] = {
        "flat_fc", "slope_fc", "slope_bc", "slope_tc",
        "slope_straight_fc", "slope_straight_bc", "slope_straight_tc",
        "slope_straight_left_fc", "slope_straight_left_bc", "slope_straight_left_tc",
        "slope_straight_right_fc", "slope_straight_right_bc", "slope_straight_right_tc",
        "slope_mid_fc", "slope_mid_bc", "slope_mid_tc",
    };
    auto ptd_fields = [&](bool ext) {
        std::vector<Field> fs = {
            f(0x00, "flags", K::Hex32),
            f(0x04, "internal_name", K::Str),
            f(0x08, "name", K::Ref),
            f(0x0C, "icon", K::Ref),
            f(0x10, "texture_a", K::Str),
            f(0x14, "texture_b", K::Str),
        };
        for (int g = 0; g < 19; ++g) fs.push_back(strs(0x18 + 16 * g, kBase[g], 4));
        fs.push_back(f(0x148, "research_count"));
        fs.push_back(sub(0x14C, "research", K::Array, research, Count::of("research_count")));
        if (ext) {
            fs.push_back(zero(0x150));
            fs.push_back(f(0x154, "unk_154"));
            for (int g = 0; g < 16; ++g) fs.push_back(strs(0x158 + 16 * g, kExt[g], 4));
            fs.push_back(f(0x258, "paving", K::Str));
        }
        return fs;
    };
    const Struct& ptd = r.make("PathType", 336, ptd_fields(false));
    const Struct& ptd2 = r.make("PathType2", 604, ptd_fields(true));
    r.add({"ptd", "PathType", "", [&ptd, &ptd2](const PairView& v, std::uint32_t a) {
               return (v.u32(a) & 0x01000000u) ? &ptd2 : &ptd;
           }});

    // ------------------------------------------------------------ qtd
    const Struct& qtd = r.make("QueueType", 52, {
        f(0x00, "internal_name", K::Str),
        f(0x04, "name", K::Ref),
        f(0x08, "icon", K::Ref),
        f(0x0C, "texture", K::Ref),
        f(0x10, "straight", K::Str),
        f(0x14, "turn_l", K::Str),
        f(0x18, "turn_r", K::Str),
        f(0x1C, "slope_up", K::Str),
        f(0x20, "slope_down", K::Str),
        f(0x24, "slope_straight_1", K::Str),
        f(0x28, "slope_straight_2", K::Str),
        f(0x2C, "research_count"),
        sub(0x30, "research", K::Array, research, Count::of("research_count")),
    });
    r.add({"qtd", "QueueType", "", [&qtd](const PairView&, std::uint32_t) { return &qtd; }});

    // ------------------------------------------------------------ pty
    // 36 guest swim splines: 6 tile shapes (order of the shape pieces) x
    // {straight, 5 curved variants}.
    static const char* const kPoolShapes[6] = {"corner3", "edge", "corner1", "corner2", "end", "full"};
    const Struct& pty = r.make("PoolType", 600, {
        f(0x000, "internal_name", K::Str),
        f(0x004, "name", K::Ref),
        f(0x008, "icon", K::Ref),
        strs(0x00C, "pieces", 89),
        f(0x170, "research_count"),
        sub(0x174, "research", K::Array, research, Count::of("research_count")),
        // Segment build cost / refund (FUN_10096f6a8 / FUN_10096f90a), by the
        // PoolSegment's SegmentType (+0x15C), Depth (+0x174), SpecialHeight (+0x160).
        f(0x178, "type0_cost_base", K::I32),
        f(0x17C, "type0_cost_per_depth", K::I32),
        f(0x180, "type0_refund_base", K::I32),
        f(0x184, "type0_refund_per_depth", K::I32),
        f(0x188, "type1to4_cost_base", K::I32),
        f(0x18C, "type1to4_cost_per_depth", K::I32),
        f(0x190, "type1to4_refund_base", K::I32),
        f(0x194, "type1to4_refund_per_depth", K::I32),
        f(0x198, "type5to9_cost", K::I32),
        f(0x19C, "type5to9_refund", K::I32),
        f(0x1A0, "type4_cost_extra", K::I32),
        f(0x1A4, "type4_refund_extra", K::I32),
        f(0x1A8, "type2_cost_per_height", K::I32),
        f(0x1AC, "type2_refund_per_height", K::I32),
        f(0x1B0, "type3_cost_per_height", K::I32),
        f(0x1B4, "type3_refund_per_height", K::I32),
        f(0x1B8, "unk_1B8", K::F32),
        f(0x1BC, "water_surface_offset", K::F32),
        f(0x1C0, "unk_1C0", K::F32),
        f(0x1C4, "edge_inset", K::F32),
        custom(0x1C8, "swim_splines", 36 * 4, [](Ctx& c, std::uint32_t fa, std::uint32_t, const Json&) -> Json {
            Json o = Json::object();
            for (int s = 0; s < 6; ++s) {
                Json a = Json::array();
                for (int i = 0; i < 6; ++i) {
                    const std::uint32_t k = std::uint32_t(4 * (6 * s + i));
                    a.push(ref_at(c, fa + k, 0x1C8 + k));
                }
                o.set(kPoolShapes[s], std::move(a));
            }
            return o;
        }),
    });
    r.add({"pty", "PoolType", "", [&pty](const PairView&, std::uint32_t) { return &pty; }});

    // ------------------------------------------------------------ Attraction (chg, vwg)
    // Same block as the scenery group's `attraction` (Attraction_V + _Sext [+ _Wext]).
    auto att_fields = [](bool wild) {
        std::vector<Field> fs = {
            f(0x00, "type", K::Hex32),
            f(0x04, "name", K::Ref),
            f(0x08, "description", K::Ref),
            f(0x0C, "icon", K::Ref),
            f(0x10, "cost", K::I32),
            f(0x14, "refund", K::I32),
            zero(0x18), zero(0x1C),
            f(0x20, "base_upkeep", K::I32),
            f(0x24, "spline", K::Ref),
            f(0x28, "path_count"),
            arr(0x2C, "paths", K::RefArray, Count::of("path_count")),
            f(0x30, "flags", K::Hex32),
            f(0x34, "max_height", K::I32),
            f(0x38, "addon_pack"),
            f(0x3C, "unk_3C"),
        };
        if (wild) fs.push_back(f(0x40, "ui_deactivation", K::Hex32));
        return fs;
    };
    const Struct& attS = r.make("attraction", 0x40, att_fields(false));
    const Struct& attW = r.make("attraction", 0x44, att_fields(true));

    // ------------------------------------------------------------ chg
    const Struct& chg = r.make("ChangingRoom", 12, {
        sub(0x00, "attraction", K::Ptr, attS),
        f(0x04, "scenery", K::Ref),
        f(0x08, "spline", K::Ref),
    });
    r.add({"chg", "ChangingRoom", "", [&chg](const PairView&, std::uint32_t) { return &chg; }});

    // ------------------------------------------------------------ vwg
    const Struct& vwg = r.make("ViewingGallery", 4, {sub(0x00, "attraction", K::Ptr, attW)});
    r.add({"vwg", "ViewingGallery", "", [&vwg](const PairView&, std::uint32_t) { return &vwg; }});

    // ------------------------------------------------------------ enc
    const Struct& enc = r.make("EnclosureType", 92, {
        f(0x00, "internal_name", K::Str),
        f(0x04, "name", K::Ref),
        f(0x08, "icon", K::Ref),
        f(0x0C, "full", K::Str),
        f(0x10, "end", K::Str),
        f(0x14, "corner", K::Str),
        f(0x18, "edge", K::Str),
        f(0x1C, "corner_inner", K::Str),
        f(0x20, "gate", K::Str),
        f(0x24, "view_left", K::Str),
        f(0x28, "view_mid", K::Str),
        f(0x2C, "view_right", K::Str),
        f(0x30, "fence_cost", K::I32),
        f(0x34, "fence_refund", K::I32),
        f(0x38, "gallery_cost", K::I32),
        f(0x3C, "gallery_refund", K::I32),
        f(0x40, "spline_right_top", K::Ref),
        f(0x44, "spline_right_bottom", K::Ref),
        f(0x48, "spline_left_top", K::Ref),
        f(0x4C, "spline_left_bottom", K::Ref),
        f(0x50, "strength", K::F32),
        f(0x54, "animal_offset", K::F32),
        f(0x58, "height", K::F32),
    });
    r.add({"enc", "EnclosureType", "", [&enc](const PairView&, std::uint32_t) { return &enc; }});

    // ------------------------------------------------------------ asd
    const Struct& tile = r.make("tile", 8, {f(0x00, "x", K::I32), f(0x04, "z", K::I32)});
    const Struct& asd = r.make("AnimalShelterDesc", 84, {
        f(0x00, "shelter", K::Ref),
        f(0x04, "hut_x", K::I32),
        f(0x08, "hut_z", K::I32),
        f(0x0C, "hut_rotation", K::I32),
        f(0x10, "hut", K::Ref),
        f(0x14, "food_trough_offset", K::F32),
        f(0x18, "food_trough_slot", K::I32),
        f(0x1C, "drink_trough_offset", K::F32),
        f(0x20, "drink_trough_slot", K::I32),
        f(0x24, "eat_spline_count"),
        arr(0x28, "eat_splines", K::RefArray, Count::of("eat_spline_count")),
        f(0x2C, "drink_spline_count"),
        arr(0x30, "drink_splines", K::RefArray, Count::of("drink_spline_count")),
        f(0x34, "sleep_spline_count"),
        arr(0x38, "sleep_splines", K::RefArray, Count::of("sleep_spline_count")),
        f(0x3C, "trough_transition_spline", K::Ref),
        f(0x40, "food_type"),
        f(0x44, "proxy_tile_count"),
        sub(0x48, "proxy_tiles", K::Array, tile, Count::of("proxy_tile_count")),
        f(0x4C, "male_spline", K::Ref),
        f(0x50, "female_spline", K::Ref),
    });
    r.add({"asd", "AnimalShelterDesc", "", [&asd](const PairView&, std::uint32_t) { return &asd; }});
}

}  // namespace ovl::layout
