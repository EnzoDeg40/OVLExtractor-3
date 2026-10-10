#include "ovl/extract/TrackReader.hpp"

#include "ovl/BinaryReader.hpp"
#include "ovl/OvlParser.hpp"
#include "ovl/extract/ShapeReader.hpp"

#include <cmath>
#include <cstring>
#include <optional>

namespace ovl {

namespace {

std::optional<BinaryReader> open_at(const OvlParser& parser, std::uint32_t off) {
    if (off == 0) return std::nullopt;
    auto p = parser.offset_to_position(off);
    if (!p.found) return std::nullopt;
    BinaryReader r(parser.side(p.currentOVL).ovlname);
    r.seek(p.position);
    return r;
}

// Reads `n` consecutive u32 starting at internal offset `off`.
std::vector<std::uint32_t> u32s_at(const OvlParser& parser, std::uint32_t off, std::size_t n) {
    std::vector<std::uint32_t> v(n, 0);
    if (auto r = open_at(parser, off))
        for (auto& x : v) x = r->read_u32();
    return v;
}

float as_f32(std::uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// A char* stored in a struct: either a string-table entry (the usual case) or
// a plain string somewhere in the data. A zero, unrelocated pointer is null.
std::string cstr(const OvlParser& parser, std::uint32_t ptr) {
    if (ptr == 0) return {};
    std::string s = parser.string_from_offset(ptr);
    if (s != "STRINGNOTFOUND") return s;
    if (auto r = open_at(parser, ptr)) return r->read_cstring(1024);
    return {};
}

// A `char**` array of `count` strings.
std::vector<std::string> cstr_array(const OvlParser& parser, std::uint32_t arr, std::uint32_t count) {
    std::vector<std::string> out;
    if (count == 0 || count > 1024) return out;
    for (std::uint32_t p : u32s_at(parser, arr, count)) {
        std::string s = cstr(parser, p);
        if (!s.empty()) out.push_back(s);
    }
    return out;
}

// An array of `count` pointer slots each patched by a symbol resolve.
std::vector<std::string> symbol_array(const OvlParser& parser, std::uint32_t arr, std::uint32_t count) {
    std::vector<std::string> out;
    if (arr == 0 || count == 0 || count > 4096) return out;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::string s = resolved_symbol_at(parser, arr + 4 * i);
        if (!s.empty()) out.push_back(s);
    }
    return out;
}

// A reference slot that is either symbol-resolved (cross-OVL) or a plain
// relocated pointer to a linked file's data in the same pair: the full
// "name:tag" symbol in both cases, "" for null.
std::string ref_at(const OvlParser& parser, std::uint32_t slot) {
    std::string s = resolved_symbol_at(parser, slot);
    if (!s.empty()) return s;
    std::uint32_t ptr = u32s_at(parser, slot, 1)[0];
    if (ptr == 0) return {};
    for (std::size_t si = 0; si < (parser.has_unique() ? 2u : 1u); ++si) {
        auto side = static_cast<OvlSide>(si);
        const auto& d = parser.side(side);
        for (std::size_t i = 0; i < d.linkedfiles.size(); ++i)
            if (d.linkedfiles[i].loaderreference.datapointer == ptr)
                return linked_symbol(parser, side, i, true);
    }
    return {};
}

}  // namespace

// ---------------------------------------------------------------- spline

bool spline_eval(const Spline& s, std::size_t seg, float t, float pos[3], float tan[3]) {
    const std::size_t n = s.nodes.size();
    if (n < 2) return false;
    const std::size_t segs = s.cyclic ? n : n - 1;
    if (seg >= segs) return false;
    const SplineNode& a = s.nodes[seg];
    const SplineNode& b = s.nodes[(seg + 1) % n];
    float u = 1 - t;
    for (int k = 0; k < 3; ++k) {
        float p0 = a.pos[k], p1 = a.pos[k] + a.cp2[k];
        float p2 = b.pos[k] + b.cp1[k], p3 = b.pos[k];
        pos[k] = u * u * u * p0 + 3 * u * u * t * p1 + 3 * u * t * t * p2 + t * t * t * p3;
        tan[k] = 3 * u * u * (p1 - p0) + 6 * u * t * (p2 - p1) + 3 * t * t * (p3 - p2);
    }
    return true;
}

// Spline (spl), at datapointer (rct3-importer spline.h):
//   +0x00 u32 nodecount   +0x04 ptr SplineNode[nodecount] (36 B: pos, cp1, cp2)
//   +0x08 u32 cyclic      +0x0C f32 totallength   +0x10 f32 1/totallength
//   +0x14 ptr f32 lengths[segments]   +0x18 ptr SplineData[segments] (14 B, opaque)
//   +0x1C f32 max_y
bool read_spline(const OvlParser& parser, OvlSide side, std::size_t lf_index,
                 Spline& out, std::string& err) {
    const auto& lf = parser.side(side).linkedfiles[lf_index];
    out = Spline{};
    out.symbol = linked_symbol(parser, side, lf_index);
    auto h = u32s_at(parser, lf.loaderreference.datapointer, 8);
    std::uint32_t nc = h[0];
    if (nc == 0 || nc > 10000) { err = "implausible node count " + std::to_string(nc); return false; }
    out.cyclic = h[2] != 0;
    out.total_length = as_f32(h[3]);
    out.max_y = as_f32(h[7]);
    auto r = open_at(parser, h[1]);
    if (!r) { err = "node array unresolved"; return false; }
    out.nodes.resize(nc);
    for (auto& n : out.nodes) {
        for (float& f : n.pos) f = r->read_f32();
        for (float& f : n.cp1) f = r->read_f32();
        for (float& f : n.cp2) f = r->read_f32();
    }
    std::size_t segs = out.cyclic ? nc : nc - 1;
    if (auto lr = open_at(parser, h[5])) {
        out.lengths.resize(segs);
        for (auto& l : out.lengths) l = lr->read_f32();
    }
    return true;
}

// ---------------------------------------------------------------- enums

const char* tks_slope_name(std::uint32_t v) {
    static const char* n[] = {"Flat", "MediumUp", "MediumDown", "SteepUp",
                              "SteepDown", "VerticalUp", "VerticalDown", "AutoEnd"};
    return v < 8 ? n[v] : "?";
}

const char* tks_bank_name(std::uint32_t v) {
    static const char* n[] = {"Flat", "HalfLeft", "Left", "InvHalfLeft",
                              "Inverted", "InvHalfRight", "Right", "HalfRight"};
    return v < 8 ? n[v] : "?";
}

const char* tks_special_name(std::uint32_t v) {
    // rct3constants.h TKS::Special_Part — first name of each value. Several
    // values are reused by later track sets for unrelated pieces (the header
    // lists the aliases); the piece name is the authoritative description.
    switch (v) {
        case 0: return "";
        case 1: return "Brakes";
        case 2: return "BlockBrakes";
        case 3: return "Station";
        case 4: return "Photosection";
        case 5: return "SCurve";
        case 6: return "SCurveTilted";
        case 7: return "Corkscrew";
        case 8: return "CorkscrewLarge";
        case 9: return "HalfLoop";
        case 10: return "QuarterLoop";
        case 11: return "HalfLoopLarge";
        case 12: return "Loop";
        case 13: return "Figure8Loop";
        case 14: return "InlineTwist";
        case 15: return "BarrelRoll";
        case 16: return "OverbankedTurnSmall";
        case 17: return "OverbankedTurnLarge";
        case 18: return "OverbankedTurnMedslope";
        case 19: return "HelixUp";
        case 20: return "HelixDown";
        case 21: return "HelixUpSmall";
        case 22: return "HelixUpLarge";
        case 23: return "QuarterHelix";
        case 24: return "HoldingBrake";
        case 25: return "LiftHillCurved";
        case 26: return "SpinningControlToggle";
        case 27: return "Reverser";
        case 28: return "LiftHillLaunched";
        case 29: return "LiftHillCable";
        case 30: return "TransferUpper";
        case 31: return "TransferLower";
        case 32: return "LiftEnd";
        case 33: return "TiltSection";
        case 34: return "Drop95Degrees";
        case 36: return "SpinningTunnel";
        case 37: return "LogReverser";
        case 38: return "LogBumps";
        case 39: return "WaterSplash";
        case 40: return "Rapids";
        case 41: return "Waterfall";
        case 42: return "Whirlpool";
        case 43: return "ZeroGRollSteep";
        case 44: return "ZeroGRollMedium";
        case 47: return "LiftMiddle";
        case 50: return "DivingLoop90Left";
        case 51: return "DivingLoop90Right";
        case 52: case 53: case 54: case 55: return "GolfHole";
        case 59: return "VerticalTightCurve";
        case 60: return "SteepSlopeToVertical";
        case 61: return "VerticalToSteepSlope";
        case 62: return "SerpentHead";
        case 63: return "SerpentTail";
        case 64: return "HelixDownSmall";
        case 65: return "HelixDownLarge";
        case 66: return "WaterSpray";
        case 67: return "InvStationMiddleEnd";
        case 68: return "AquariumArch";
        case 69: return "AquariumCircle";
        case 70: return "AquariumInsideL";
        case 71: return "AquariumInsideWall";
        case 73: return "AquariumRay";
        case 74: return "SpiralLiftHillBottom";
        case 75: return "SpiralLiftHillMiddle";
        case 76: return "SpiralLiftHillTop";
        case 77: case 78: return "RapidsWhiteWater";
        case 80: return "StraightToVerticalSmall";
        case 81: return "StraightToVerticalMedium";
        case 82: return "SlideStationTransition";
        case 83: return "SlideBowl";
        case 84: return "SlideFunnel";
        case 85: return "SlideEnd";
        case 86: return "SlideEndToLiftHill";
        case 88: return "FlyingSnakeDive";
        case 89: return "DivingLoop";
        case 90: return "CorkscrewLoose";
        case 91: return "RotatingTowerBase";
        case 92: return "RotatingTowerVertical";
        case 93: return "RotatingTowerCap180";
        case 94: return "RotatingTowerCap90CW";
        case 96: return "LoopTiltIncline";
        case 97: return "StraightToVerticalLarge";
        case 99: return "InvStraightToVerticalSmall";
        case 100: return "VerticalToStraightLarge";
        case 101: return "VerticalToStraightSmall";
        case 102: return "VerticalToInvStraightLarge";
        case 103: return "VerticalToInvStraightSmall";
        case 104: return "SteepSlopeToStraightShort";
        case 105: return "SteepSlopeToStraightShortChain";
        case 106: return "StraightToSteepSlopeShort";
        case 107: return "StraightToSteepSlopeShortChain";
        case 108: return "HalfLoopLarge33";
        case 109: return "HalfLoopMedium33";
        case 110: return "HalfLoopSmall33";
        case 111: return "HalfLoopDownSmall33";
        case 112: return "HalfLoopInvLarge33";
        case 113: return "HalfLoopInvDownLarge33";
        case 114: return "HalfLoopInvMedium33";
        case 115: return "HalfLoopInvDownMedium33";
        case 116: return "HalfLoopInvDownSmall33";
        case 117: return "StraightToVerticalDownInvLarge";
        case 118: return "VerticalToStraightInvLarge";
        case 119: return "StraightToVerticalDownLarge";
        case 120: return "VerticalToStraightDownLarge";
        case 121: return "LoopLargeLeft";
        case 122: return "LoopLargeRight";
        case 123: return "LoopSmallLeft";
        case 124: return "LoopSmallRight";
        case 125: return "OverbankedTurnSmallLeft34";
        case 126: return "VerticalToInvStraightDownLarge";
        case 127: return "InvCorkscrew";
        case 128: return "InvCorkscrewLarge";
        default: return "";
    }
}

std::vector<std::string> tks_flag_names(std::uint32_t f) {
    static const std::pair<std::uint32_t, const char*> kFlags[] = {
        {0x00000001, "StartSection"}, {0x00000002, "Chain"},
        {0x00000004, "ForwardOnly"}, {0x00000008, "Capped"},
        {0x00000010, "Transfer"}, {0x00000020, "CableLift"},
        {0x00000040, "Station"}, {0x00000080, "TowerRideBase"},
        {0x00000100, "Hold"}, {0x00000200, "LiftBottom"},
        {0x00000400, "LiftMiddle"}, {0x00000800, "LiftTop"},
        {0x00001000, "WaterOption"}, {0x00002000, "StationEndSuspendedMono"},
        {0x00004000, "Accelerating"}, {0x00008000, "RapidsTightCurve"},
        {0x00010000, "Reverser"}, {0x00020000, "AlternateSpline"},
        {0x00040000, "TriggerAlternateSpline"}, {0x00200000, "UnknownSpecial"},
        {0x00400000, "ExtendedStructure"}, {0x00800000, "SerpentHead"},
        {0x01000000, "Booster"}, {0x02000000, "Aquarium"},
        {0x08000000, "RingSlideStation1"}, {0x20000000, "RingSlideStation2"},
    };
    std::vector<std::string> out;
    std::uint32_t known = 0;
    for (auto& [bit, name] : kFlags) {
        known |= bit;
        if (f & bit) out.push_back(name);
    }
    for (int b = 0; b < 32; ++b)
        if ((f & ~known) & (1u << b)) out.push_back("bit" + std::to_string(b));
    return out;
}

// ---------------------------------------------------------------- tks

// TrackSection_V (228 B) + _Sext (136 B, Soaked) + _Wext (28 B, Wild).
// See docs §15.2 for the field table.
bool read_track_section(const OvlParser& parser, OvlSide side, std::size_t lf_index,
                        TrackSectionDef& out, std::string& err) {
    const auto& lf = parser.side(side).linkedfiles[lf_index];
    out = TrackSectionDef{};
    out.symbol = linked_symbol(parser, side, lf_index);
    const std::uint32_t b = lf.loaderreference.datapointer;
    auto v = u32s_at(parser, b, 0xE4 / 4);
    if (!open_at(parser, b)) { err = "datapointer unresolved"; return false; }
    auto sym = [&](std::uint32_t off) { return resolved_symbol_at(parser, b + off); };

    out.internal_name = cstr(parser, v[0]);
    out.sid = sym(0x04);
    out.entry_curve = v[0x08 / 4];
    out.exit_curve = v[0x0C / 4];
    out.special = v[0x10 / 4];
    out.direction = v[0x14 / 4];
    out.entry_flags = v[0x18 / 4];
    out.exit_flags = v[0x1C / 4];
    out.spline_left = sym(0x20);
    out.spline_right = sym(0x24);
    out.join_spline_left = sym(0x28);
    out.join_spline_right = sym(0x2C);
    out.extra_spline_left = sym(0x30);
    out.extra_spline_right = sym(0x34);
    out.unk18 = v[0x44 / 4];
    out.entry_slope = v[0x48 / 4];
    out.entry_bank = v[0x4C / 4];
    out.entry_group = cstr(parser, v[0x50 / 4]);
    out.unk25 = v[0x60 / 4];
    out.exit_slope = v[0x64 / 4];
    out.exit_bank = v[0x68 / 4];
    out.exit_group = cstr(parser, v[0x6C / 4]);

    const bool ext = (out.entry_flags & 0x00400000u) != 0;
    std::uint32_t speed_count = v[0x70 / 4];
    if (speed_count > 0 && speed_count < 256) {
        const std::size_t stride = ext ? 5 : 3;
        auto sp = u32s_at(parser, v[0x74 / 4], speed_count * stride);
        for (std::uint32_t i = 0; i < speed_count; ++i) {
            TrackSpeed s;
            for (std::size_t k = 0; k < stride; ++k) s.v[k] = as_f32(sp[i * stride + k]);
            out.speeds.push_back(s);
        }
    }
    out.tower_ride_base = v[0x78 / 4];
    out.water_splash[0] = as_f32(v[0x80 / 4]);
    out.water_splash[1] = as_f32(v[0x84 / 4]);
    out.reverser_val = as_f32(v[0x88 / 4]);
    if (!ext) {
        for (int k = 0; k < 9; ++k) out.anim[k] = static_cast<std::int32_t>(v[0x8C / 4 + k]);
    } else {
        // Soaked/Wild: anim_count @+0x8C, ptr @+0x90 → TrackSectionAnimation_S/W
        std::uint32_t ac = v[0x8C / 4];
        if (ac >= 9 && ac <= 16) {
            auto a = u32s_at(parser, v[0x90 / 4], 9);
            for (int k = 0; k < 9; ++k) out.anim[k] = static_cast<std::int32_t>(a[k]);
        }
    }
    if (v[0xD4 / 4] != 0) {
        out.water_spline_left = sym(0xD8);
        out.water_spline_right = sym(0xDC);
    }

    if (ext) {
        const std::uint32_t s = b + 0xE4;
        auto x = u32s_at(parser, s, 0x88 / 4);
        out.version = x[0];
        out.auto_group = cstr(parser, x[0x20 / 4]);
        out.entry_wide = x[0x30 / 4];
        out.exit_wide = x[0x34 / 4];
        out.soaked_options = x[0x44 / 4];
        out.groups_is_at_entry          = cstr_array(parser, x[0x5C / 4], x[0x58 / 4]);
        out.groups_is_at_exit           = cstr_array(parser, x[0x64 / 4], x[0x60 / 4]);
        out.groups_must_have_at_entry   = cstr_array(parser, x[0x6C / 4], x[0x68 / 4]);
        out.groups_must_have_at_exit    = cstr_array(parser, x[0x74 / 4], x[0x70 / 4]);
        out.groups_must_not_be_at_entry = cstr_array(parser, x[0x7C / 4], x[0x78 / 4]);
        out.groups_must_not_be_at_exit  = cstr_array(parser, x[0x84 / 4], x[0x80 / 4]);
        if (out.version >= 3) {
            auto w = u32s_at(parser, s + 0x88, 7);
            out.splitter_half = w[0];
            out.rotator_type = w[2];
        }
    }
    return true;
}

// ---------------------------------------------------------------- sid

// SceneryItem_V (rct3-importer sceneryrevised.h; rerct3 scenery-item.ts):
//   +0x04 flags  +0x08 u16 position_type  +0x10/+0x14 squares x/z
//   +0x18 ptr SceneryItemData[sx*sz] (20 B)  +0x1C f32[3] position
//   +0x28 f32[3] size  +0x3C cost  +0x40 removal cost  +0x48 type
//   +0x4C char* supports  +0x50 svd count  +0x54 ptr svd slots
//   +0x8C u32[3] default colours
bool read_scenery_item(const OvlParser& parser, OvlSide side, std::size_t lf_index,
                       SceneryItemInfo& out, std::string& err) {
    const auto& lf = parser.side(side).linkedfiles[lf_index];
    out = SceneryItemInfo{};
    out.symbol = linked_symbol(parser, side, lf_index);
    const std::uint32_t b = lf.loaderreference.datapointer;
    if (!open_at(parser, b)) { err = "datapointer unresolved"; return false; }
    auto v = u32s_at(parser, b, 0x98 / 4);
    out.flags = v[1];
    out.position_type = static_cast<std::uint16_t>(v[2] & 0xFFFF);
    out.squares_x = v[4];
    out.squares_z = v[5];
    for (int k = 0; k < 3; ++k) {
        out.position[k] = as_f32(v[7 + k]);
        out.size[k] = as_f32(v[10 + k]);
    }
    out.cost = static_cast<std::int32_t>(v[0x3C / 4]);
    out.removal_cost = static_cast<std::int32_t>(v[0x40 / 4]);
    out.type = v[0x48 / 4];
    out.supports = cstr(parser, v[0x4C / 4]);
    std::uint32_t svd_count = v[0x50 / 4];
    out.svds = symbol_array(parser, v[0x54 / 4], svd_count);
    for (int k = 0; k < 3; ++k) out.default_colours[k] = v[0x8C / 4 + k];
    std::uint32_t nsq = out.squares_x * out.squares_z;
    if (nsq > 0 && nsq < 4096) {
        auto d = u32s_at(parser, v[0x18 / 4], nsq * 5);
        for (std::uint32_t i = 0; i < nsq; ++i) {
            SceneryItemSquare q;
            q.flags = d[i * 5];
            q.height_adjust = static_cast<std::int32_t>(d[i * 5 + 1]);
            q.height_span = d[i * 5 + 2];
            q.direction_type = d[i * 5 + 4];
            out.squares.push_back(q);
        }
    }
    return true;
}

// ---------------------------------------------------------------- trr

// TrackedRide_V = Attraction_V (56 B) + Ride_V (24 B) + TrackedRide_Common
// (300 B). Soaked/Wild (TrackedRide_S/W) start directly with Common, whose
// first u32 is then 0xFFFFFFFF, followed by an extension block.
bool read_tracked_ride(const OvlParser& parser, OvlSide side, std::size_t lf_index,
                       TrackedRideInfo& out, std::string& err) {
    const auto& lf = parser.side(side).linkedfiles[lf_index];
    out = TrackedRideInfo{};
    out.symbol = linked_symbol(parser, side, lf_index);
    const std::uint32_t b = lf.loaderreference.datapointer;
    if (!open_at(parser, b)) { err = "datapointer unresolved"; return false; }
    std::uint32_t first = u32s_at(parser, b, 1)[0];
    out.soaked_wild = first == 0xFFFFFFFFu;
    if (!out.soaked_wild) out.attraction_type = first;
    const std::uint32_t c = out.soaked_wild ? b : b + 80;
    auto v = u32s_at(parser, c, 300 / 4);
    auto f = [&](std::size_t idx) { return as_f32(v[idx]); };

    // Field indices below are 0-based u32 slots of TrackedRide_Common
    // (the "// 10", "// 20"… comments in trackedride.h count from 1).
    std::uint32_t section_count = v[0];
    out.train_names = cstr_array(parser, v[3], v[2]);
    out.cable_lift = cstr(parser, v[4]);
    out.lift_car = cstr(parser, v[9]);
    out.station_name = cstr(parser, v[14]);
    out.platform_height_over_track = v[15];
    out.only_on_water = v[16];
    out.start_preset = v[18];
    out.start_possibilities = v[19];
    out.blocks_possible = v[20];
    out.station_roll_speed = f(21);
    out.launched_speed_preset = f(23);
    out.chain_speed_preset = f(25);
    out.constant_speed_preset = f(31);
    out.upkeep_per_train = v[41];
    out.upkeep_per_car = v[42];
    out.upkeep_per_station = v[43];
    out.car_rotation = v[44];
    out.loop_not_allowed = v[47];
    out.cost_per_4h_height = v[51];
    std::string path = cstr(parser, v[52]);
    if (!path.empty()) out.track_paths.push_back(path);
    out.other_top = resolved_symbol_at(parser, c + 53 * 4);
    out.other_mid = resolved_symbol_at(parser, c + 54 * 4);
    out.tower_top = resolved_symbol_at(parser, c + 55 * 4);
    out.tower_mid = resolved_symbol_at(parser, c + 56 * 4);
    out.auto_complete = v[57];
    out.chain_speed_min = f(60);
    out.chain_speed_max = f(61);
    std::uint32_t struct_count = v[64];
    std::uint32_t structs_ptr = v[65];
    out.launched_speed_min = f(66);
    out.launched_speed_max = f(67);
    out.track_spline = resolved_symbol_at(parser, c + 69 * 4);
    out.track_big_spline = resolved_symbol_at(parser, c + 70 * 4);
    out.car_spline = resolved_symbol_at(parser, c + 71 * 4);
    out.car_swing_spline = resolved_symbol_at(parser, c + 72 * 4);

    if (out.soaked_wild) {
        // Extension after Common: +0x00 Ride_S*, +0x04 track_section_count,
        // ... +0x3C track_path_count, +0x40 char** track_paths.
        auto e = u32s_at(parser, c + 300, 17);
        section_count = e[1];
        if (struct_count == 0) struct_count = section_count;
        out.track_paths = cstr_array(parser, e[16], e[15]);
        // group_definitions: e[7] arrays of {u32 count; Definition* defs},
        // Definition = two {u32 flags; char* group | TrackSection* ref}.
        if (e[7] > 0 && e[7] < 256) {
            auto arrs = u32s_at(parser, e[8], e[7] * 2);
            for (std::uint32_t g = 0; g < e[7]; ++g) {
                std::uint32_t n = arrs[g * 2], p = arrs[g * 2 + 1];
                if (n == 0 || n > 256) continue;
                auto defs = u32s_at(parser, p, n * 4);
                std::vector<TrackGroupPair> group;
                for (std::uint32_t k = 0; k < n; ++k) {
                    TrackGroupPair gp;
                    for (int h = 0; h < 2; ++h) {
                        std::uint32_t fl = defs[k * 4 + h * 2], ptr = defs[k * 4 + h * 2 + 1];
                        TrackGroupRef& r = h ? gp.b : gp.a;
                        r.flags = fl;
                        if ((fl & 0xFF) == 1) r.value = resolved_symbol_at(parser, p + (k * 4 + h * 2 + 1) * 4);
                        else if ((fl & 0xFF) == 0) r.value = cstr(parser, ptr);
                    }
                    group.push_back(gp);
                }
                out.group_definitions.push_back(std::move(group));
            }
        }
    }
    if (section_count > 4096) { err = "implausible section count"; return false; }
    out.sections = symbol_array(parser, v[1], section_count);
    if (struct_count > 0 && struct_count < 4096) {
        auto s = u32s_at(parser, structs_ptr, struct_count * 2);
        for (std::uint32_t i = 0; i < struct_count; ++i) {
            TrackedRideSectionCost sc{cstr(parser, s[i * 2]), s[i * 2 + 1]};
            if (!sc.name.empty()) out.section_costs.push_back(sc);
        }
    }
    return true;
}

// ---------------------------------------------------------------- ric / rit

bool read_ride_car(const OvlParser& parser, OvlSide side, std::size_t lf_index,
                   RideCarInfo& out, std::string& err) {
    const auto& lf = parser.side(side).linkedfiles[lf_index];
    out = RideCarInfo{};
    out.symbol = linked_symbol(parser, side, lf_index);
    const std::uint32_t b = lf.loaderreference.datapointer;
    if (!open_at(parser, b)) { err = "datapointer unresolved"; return false; }
    auto v = u32s_at(parser, b, 240 / 4);
    auto sym = [&](std::uint32_t off) { return resolved_symbol_at(parser, b + off); };
    out.name = cstr(parser, v[0]);
    out.username = cstr(parser, v[1]);
    out.seating = static_cast<std::uint8_t>(v[2] & 0xFF);
    out.version = static_cast<std::uint8_t>((v[2] >> 8) & 0xFF);
    out.svd = sym(12);
    out.inertia = as_f32(v[4]);
    out.moving_svd = sym(20);
    out.axis_type = v[6];
    out.moving_inertia = as_f32(v[7]);
    out.axis_stability = as_f32(v[8]);
    out.axis_momentum = as_f32(v[11]);
    out.axis_maximum = as_f32(v[14]);
    out.bobbing = v[15];
    for (int k = 0; k < 20; ++k) out.anims[k] = static_cast<std::int32_t>(v[19 + k]);
    std::uint32_t seat_count = v[39];
    if (seat_count > 0 && seat_count < 64) out.seat_types = u32s_at(parser, v[40], seat_count);
    for (int k = 0; k < 4; ++k) {
        out.wheels[k] = sym(164 + 8 * k);
        out.wheels_flipped[k] = v[(168 + 8 * k) / 4];
    }
    out.axle_front = sym(196);
    out.axle_rear = sym(200);
    out.axle_front_type = v[204 / 4];
    out.axle_rear_type = v[208 / 4];
    return true;
}

bool read_ride_train(const OvlParser& parser, OvlSide side, std::size_t lf_index,
                     RideTrainInfo& out, std::string& err) {
    const auto& lf = parser.side(side).linkedfiles[lf_index];
    out = RideTrainInfo{};
    out.symbol = linked_symbol(parser, side, lf_index);
    const std::uint32_t b = lf.loaderreference.datapointer;
    if (!open_at(parser, b)) { err = "datapointer unresolved"; return false; }
    auto v = u32s_at(parser, b, 188 / 4);
    if (v[0] != 0xFFFFFFFFu) out.name = cstr(parser, v[0]);
    out.description = cstr(parser, v[1]);
    for (int k = 0; k < 6; ++k) out.cars[k] = ref_at(parser, b + 8 + 4 * k);
    out.car_count_min = v[8];
    out.car_count_max = v[9];
    out.car_count_def = v[10];
    out.friction_dynamic = as_f32(v[11]);
    out.friction_static = as_f32(v[12]);
    out.air_drag = as_f32(v[13]);
    out.camera_lookahead = as_f32(v[14]);
    out.overtake = v[35];
    out.liftcar_left = resolved_symbol_at(parser, b + 176);
    out.liftcar_right = resolved_symbol_at(parser, b + 180);
    out.station = cstr(parser, v[46]);
    // The extension is present when the name slot is the 0xFFFFFFFF marker
    // (RideTrainB/C); its first u32 is the version.
    if (v[0] == 0xFFFFFFFFu) {
        auto x = u32s_at(parser, b + 188, 4);
        if (x[0] == 2 || x[0] == 3) {
            out.version = x[0];
            out.sext_name = cstr(parser, x[1]);
            out.icon = x[2];
        }
    }
    return true;
}

}  // namespace ovl
