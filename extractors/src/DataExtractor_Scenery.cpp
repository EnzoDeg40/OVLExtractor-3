// Layouts of the scenery group, see Layout.hpp and docs/RCT3_OVL_FORMAT.md.
//
//   sid  SceneryItem        (SceneryItem_V / _S / _W, chosen by structure_version @ +0xA)
//   sta  Stall              (Stall_V, or Stall_SW marked by 0xFFFFFFFF @ +0)
//   sat  SpecialAttraction  (SpecialAttraction_V, or _SW marked by 0xFFFFFFFF @ +0)
//
// Sources: libOVLng sceneryrevised.h / stall.h / attraction.h and ManagerSID / STA / SAT,
// rerct3 scenery-item.ts / stall.ts / special-attraction.ts / supports.md, value statistics.
#include "ovl/extract/Layout.hpp"

#include <cstdio>

namespace ovl::layout {

namespace {

// Number of u32 words of a square's height-block bit set: one bit per height unit
// from min_height_block to max_height_block (ManagerSID cSidSquareUnknowns::Fill).
std::int64_t block_bit_words(const Json& so_far) {
    const Json* lo = so_far.get("min_height_block");
    const Json* hi = so_far.get("max_height_block");
    if (!lo || !hi) return 0;
    const std::int64_t h = hi->as_int() - lo->as_int();
    return h > 0 ? (h + 31) / 32 : 0;
}

// A SoundScript: { f32 time; u32 command; f32 parameter[command >= 3 ? 2 : 0] } repeated,
// ended by command 0 (ManagerSID.cpp). Commands: 1 start, 2 stop, 3 volume fade
// (to p0 at time p1), 4 pitch fade (to p0 at time p1).
Json sound_script(Ctx& c, std::uint32_t a, const std::string& where) {
    const PairView& v = c.view();
    Json out = Json::array();
    for (int guard = 0; guard < 4096; ++guard) {
        Json e = Json::object();
        e.set("time", Json(v.f32(a)));
        const std::uint32_t cmd = v.u32(a + 4);
        e.set("command", Json(cmd));
        std::uint32_t n = 8;
        if (cmd == 3 || cmd == 4) {
            e.set("value", Json(v.f32(a + 8)));
            e.set("end_time", Json(v.f32(a + 12)));
            n = 16;
        } else if (cmd > 4) {
            c.issue(where, "unknown sound script command " + std::to_string(cmd));
        }
        if (!v.slots_in(a, n).empty()) c.issue(where, "pointer slot inside a sound script");
        c.cover(a, n);
        out.push(std::move(e));
        a += n;
        if (cmd == 0 || cmd > 4) return out;
    }
    c.issue(where, "unterminated sound script");
    return out;
}

}  // namespace

void register_scenery(Registry& r) {
    // ---------------------------------------------------------------- sid
    // SceneryItemData: one per footprint square (squares_x * squares_z, row-major).
    const Struct& square = r.make("square", 20, [] {
        std::vector<Field> fs = {
            f(0x00, "flags", K::Hex32),
            f(0x04, "min_height_block", K::I32),
            f(0x08, "max_height_block", K::I32),
            arr(0x0C, "block_bits", K::Scalars, Count{}, K::Hex32),
            f(0x10, "support_type", K::U32),
        };
        fs[3].count.fn = [](const Ctx&, std::uint32_t, const Json& so_far) { return block_bit_words(so_far); };
        return fs;
    }());

    // SceneryItemTrack: a support / girder connection (88 B). Three points in the
    // item's local grid: start, end and an unused control point (angled links).
    // The game compares start and end cells to register cross-cell girders
    // (FUN_1007d1c3c) and rotates each cell pair through FUN_1007ca90a.
    const Struct& track = r.make("track_struct", 88, {
        f(0x00, "start_x", K::I32), f(0x04, "start_z", K::I32),
        f(0x08, "start_sub_x"), f(0x0C, "start_sub_z"),
        f(0x10, "start_offset_x", K::F32), f(0x14, "start_offset_z", K::F32),
        f(0x18, "start_height", K::F32),
        f(0x1C, "end_x", K::I32), f(0x20, "end_z", K::I32),
        f(0x24, "end_sub_x"), f(0x28, "end_sub_z"),
        f(0x2C, "end_height", K::F32),
        f(0x30, "angled"),
        f(0x34, "control_x", K::I32), f(0x38, "control_z", K::I32),
        f(0x3C, "control_sub_x"), f(0x40, "control_sub_z"),
        f(0x44, "control_offset_x", K::F32), f(0x48, "control_offset_z", K::F32),
        f(0x4C, "control_height", K::F32),
        f(0x50, "flags", K::Hex32),
        f(0x54, "group"),
    });

    const Struct& param = r.make("param", 8, {f(0x00, "type", K::Str), f(0x04, "params", K::Str)});

    const Struct& sound = r.make("sound", 16, {
        f(0x00, "sound_count"),
        arr(0x04, "sounds", K::RefArray, Count::of("sound_count")),
        f(0x08, "script_count"),
        custom(0x0C, "scripts", 4, [](Ctx& c, std::uint32_t fa, std::uint32_t base, const Json&) -> Json {
            const std::string where = c.path + "+0xC";
            const std::uint32_t n = c.view().u32(base + 0x8);
            auto p = c.pointer(fa, where);
            if (!p) return n ? Json() : Json::array();
            Json out = Json::array();
            for (std::uint32_t i = 0; i < n; ++i) {
                auto q = c.pointer(*p + 4 * i, where);
                out.push(q ? sound_script(c, *q, where) : Json());
            }
            return out;
        }),
    });

    const Struct& extra_sound = r.make("extra_sound", 12, {
        f(0x00, "min_distance", K::F32), f(0x04, "max_distance", K::F32), f(0x08, "category"),
    });

    auto sid_fields = [&](int version) {
        std::vector<Field> fs = {
            zero(0x00),
            f(0x04, "flags", K::Hex32),
            f(0x08, "position_type", K::U16),
            f(0x0A, "structure_version", K::U16),
            f(0x0C, "height"),  // 0, or the item height in height units (== size_y == max square block)
            f(0x10, "squares_x"),
            f(0x14, "squares_z"),
            sub(0x18, "squares", K::Array, square, Count{}),
            f(0x1C, "position_x", K::F32), f(0x20, "position_y", K::F32), f(0x24, "position_z", K::F32),
            f(0x28, "size_x", K::F32), f(0x2C, "size_y", K::F32), f(0x30, "size_z", K::F32),
            f(0x34, "track_struct_count"),
            sub(0x38, "track_structs", K::Array, track, Count::of("track_struct_count")),
            f(0x3C, "cost", K::I32),
            f(0x40, "removal_cost", K::I32),
            f(0x44, "unk_44"),  // 5 rides / track / stalls / most scenery, 0 walls / paths / supports, 4 raised paths
            f(0x48, "type"),
            f(0x4C, "supports", K::Str),
            f(0x50, "svd_count"),
            arr(0x54, "svds", K::RefArray, Count::of("svd_count")),
            f(0x58, "icon", K::Ref),
            f(0x5C, "group_icon", K::Ref),
            f(0x60, "group_name", K::Ref),
            f(0x64, "ovl_path", K::Str),
            f(0x68, "param_count"),
            sub(0x6C, "params", K::Array, param, Count::of("param_count")),
            f(0x70, "sound_count"),
            sub(0x74, "sounds", K::Array, sound, Count::of("sound_count")),
            f(0x78, "name", K::Ref),
            zero(0x7C), zero(0x80),
            // Breakdown timer: BREAKTIME = min + rand() * range game-time units (2880 = 1 day),
            // FUN_1007cf59e; 0 / 0 = never breaks; stalls (type 27) use 6000 + rand() * 8000.
            f(0x84, "break_time_min", K::I32), f(0x88, "break_time_range", K::I32),
            // FlexiColour indices 0..31 of the three recolourable channels.
            f(0x8C, "default_colour_1"), f(0x90, "default_colour_2"), f(0x94, "default_colour_3"),
            zero(0x98, 32),
            f(0xB8, "anr_name_count"),
            arr(0xBC, "anr_names", K::StrArray, Count::of("anr_name_count")),
            zero(0xC0),
            f(0xC4, "anr_age_alternatives"),
            f(0xC8, "anr_animation_chunks"),
            f(0xCC, "anr_alternate_run_animations"),
            f(0xD0, "anr_cycles_per_circuit"),
        };
        fs[7].count.fn = [](const Ctx& c, std::uint32_t base, const Json&) {
            return std::int64_t(c.view().u32(base + 0x10)) * c.view().u32(base + 0x14);
        };
        if (version >= 1) {
            fs.push_back(sub(0xD4, "extra_sounds", K::Array, extra_sound, Count::of("sound_count")));
            fs.push_back(zero(0xD8));
            fs.push_back(f(0xDC, "addon_pack"));
            fs.push_back(f(0xE0, "generic_addon"));
        }
        if (version >= 2) {
            fs.push_back(f(0xE4, "unk_E4", K::F32));  // -1, except Wild trees (Acacia 0.5, Baobab 2, Banyan 3.4)
            fs.push_back(f(0xE8, "billboard_aspect", K::I32));
        }
        return fs;
    };
    const Struct& sid0 = r.make("sid", 0xD4, sid_fields(0));
    const Struct& sid1 = r.make("sid", 0xE4, sid_fields(1));
    const Struct& sid2 = r.make("sid", 0xEC, sid_fields(2));
    r.add({"sid", "SceneryItem", "", [s0 = &sid0, s1 = &sid1, s2 = &sid2](const PairView& v, std::uint32_t a) -> const Struct* {
               switch (v.u16(a + 0xA)) {
                   case 0: return s0;
                   case 1: return s1;
                   case 2: return s2;
                   default: return nullptr;
               }
           }});

    // ---------------------------------------------------------------- Attraction
    auto att_fields = [](int version) {
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
        };
        if (version >= 1) {
            fs.push_back(f(0x38, "addon_pack"));
            fs.push_back(f(0x3C, "generic_addon"));
        }
        if (version >= 2) fs.push_back(f(0x40, "ui_deactivation", K::Hex32));
        return fs;
    };
    const Struct& attS = r.make("attraction", 0x40, att_fields(1));
    const Struct& attW = r.make("attraction", 0x44, att_fields(2));
    // Pointer to an Attraction_S (type byte 1 == 2) or Attraction_W (type byte 1 == 3).
    auto att_ptr = [pS = &attS, pW = &attW](std::uint32_t off) {
        return custom(off, "attraction", 4, [pS, pW, off](Ctx& c, std::uint32_t fa, std::uint32_t, const Json&) -> Json {
            char b[16];
            std::snprintf(b, sizeof b, "+0x%X", off);
            const std::string where = c.path + b;
            auto p = c.pointer(fa, where);
            if (!p) return Json();
            const std::uint32_t hi = (c.view().u32(*p) >> 8) & 0xFF;
            if (hi == 2) return c.decode(*pS, *p);
            if (hi == 3) return c.decode(*pW, *p);
            c.issue(where, "attraction of unknown version " + std::to_string(hi));
            return Json();
        });
    };

    // ---------------------------------------------------------------- sta
    const Struct& stall_item = r.make("item", 8, {f(0x00, "item", K::Ref), f(0x04, "price", K::I32)});
    // Appeal 0..9 per guest group (FUN_10054a81e: PersonType 0/1/2 = child/teen/adult,
    // SubType 1 = female -> first triple, 0 = male -> second; feeds powf(base, appeal…)).
    auto stall_tail = [](std::uint32_t o) {
        return std::vector<Field>{
            f(o + 0x00, "appeal_female_child"), f(o + 0x04, "appeal_female_teen"), f(o + 0x08, "appeal_female_adult"),
            f(o + 0x0C, "appeal_male_child"), f(o + 0x10, "appeal_male_teen"), f(o + 0x14, "appeal_male_adult"),
        };
    };
    std::vector<Field> staV = att_fields(0);
    staV.push_back(f(0x38, "sid", K::Ref));
    staV.push_back(f(0x3C, "item_count"));
    staV.push_back(sub(0x40, "items", K::Array, stall_item, Count::of("item_count")));
    for (auto& x : stall_tail(0x44)) staV.push_back(x);
    const Struct& staVs = r.make("sta", 0x5C, staV);
    std::vector<Field> staSW = {
        f(0x00, "sw_marker", K::Hex32),
        f(0x04, "item_count"),
        sub(0x08, "items", K::Array, stall_item, Count::of("item_count")),
    };
    for (auto& x : stall_tail(0x0C)) staSW.push_back(x);
    staSW.push_back(att_ptr(0x24));
    staSW.push_back(f(0x28, "sid", K::Ref));
    const Struct& staSWs = r.make("sta", 0x2C, staSW);
    r.add({"sta", "Stall", "", [sw = &staSWs, sv = &staVs](const PairView& v, std::uint32_t a) -> const Struct* {
               return v.u32(a) == 0xFFFFFFFF ? sw : sv;
           }});

    // ---------------------------------------------------------------- sat
    // The game tells the variants apart by *data != 2 (FUN_1006ec382).
    std::vector<Field> satV = att_fields(0);
    satV.push_back(f(0x38, "sid", K::Ref));
    const Struct& satVs = r.make("sat", 0x3C, satV);
    const Struct& satSWs = r.make("sat", 0x0C, {f(0x00, "sw_marker"), att_ptr(0x04), f(0x08, "sid", K::Ref)});
    r.add({"sat", "SpecialAttraction", "", [sw = &satSWs, sv = &satVs](const PairView& v, std::uint32_t a) -> const Struct* {
               return v.u32(a) == 2 ? sw : sv;
           }});
}

}  // namespace ovl::layout
