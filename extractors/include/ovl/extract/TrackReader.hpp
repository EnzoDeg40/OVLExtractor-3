#pragma once

#include "ovl/OvlTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ovl {

class OvlParser;

// In-memory readers for the ride-side loaders (docs §15): `spl` (Spline),
// `tks` (TrackSection), `sid` (SceneryItem — placement/footprint) and `trr`
// (TrackedRide). Values are returned as stored: Direct3D left-handed, Y-up,
// metres (one grid tile = 4 units, one height step = 1 unit).
//
// Struct layouts come from rct3-importer's tracksection.h / trackedride.h /
// spline.h / sceneryrevised.h (Jonathan Wilson's dumper, extended by
// Belgabor), cross-checked against the rerct3 Ghidra notes
// (research/re/track.md) and against the retail data.

// ------------------------------------------------------------------ spline

// One Bézier node. `cp1`/`cp2` are RELATIVE to `pos`: cp1 points back to the
// previous node, cp2 forward to the next. Segment i is the cubic
//   P0 = n[i].pos, P1 = n[i].pos + n[i].cp2,
//   P2 = n[i+1].pos + n[i+1].cp1, P3 = n[i+1].pos.
struct SplineNode {
    float pos[3] = {0, 0, 0};
    float cp1[3] = {0, 0, 0};
    float cp2[3] = {0, 0, 0};
};

struct Spline {
    std::string             symbol;          // without ":spl"
    bool                    cyclic = false;
    float                   total_length = 0;
    float                   max_y = 0;
    std::vector<SplineNode> nodes;
    std::vector<float>      lengths;         // per segment
};

// Evaluate segment `seg` of `s` at t ∈ [0,1]; writes position and (unnormalised)
// tangent. Returns false when the segment does not exist.
bool spline_eval(const Spline& s, std::size_t seg, float t, float pos[3], float tan[3]);

// --------------------------------------------------------------- sid (subset)

// SceneryItem_V (rct3-importer sceneryrevised.h). Only what a ride needs:
// the footprint used by grid placement and the svd(s) the item draws.
struct SceneryItemSquare {
    std::uint32_t flags = 0;          // bit0 collision, bit1 supports
    std::int32_t  height_adjust = 0;
    std::uint32_t height_span = 0;
    std::uint32_t direction_type = 0;
};

struct SceneryItemInfo {
    std::string                     symbol;          // without ":sid"
    std::uint32_t                   flags = 0;
    std::uint16_t                   position_type = 0;  // 0 = full tile (footprint-centred)
    std::uint32_t                   squares_x = 1;
    std::uint32_t                   squares_z = 1;
    float                           position[3] = {0, 0, 0};
    float                           size[3] = {0, 0, 0};
    std::uint32_t                   type = 0;        // 18 = ride track, 23 = ride, ...
    std::int32_t                    cost = 0;        // cents
    std::int32_t                    removal_cost = 0;
    std::string                     supports;        // support style name ("" = none)
    std::vector<std::string>        svds;            // "foo:svd" symbols
    std::vector<SceneryItemSquare>  squares;         // squares_x * squares_z
    std::uint32_t                   default_colours[3] = {0, 0, 0};
};

// --------------------------------------------------------------- tks

// TKS enums (rct3constants.h r3::Constants::TKS). Slope / bank are what the
// game compares at a join (see docs §15.4).
const char* tks_slope_name(std::uint32_t v);    // Flat, MediumUp, ...
const char* tks_bank_name(std::uint32_t v);     // Flat, HalfLeft, ...
const char* tks_special_name(std::uint32_t v);  // Loop, Corkscrew, ... ("" when unknown)
std::vector<std::string> tks_flag_names(std::uint32_t flags);

struct TrackSpeed {
    float v[5] = {0, 0, 0, 0, 0};    // 3 floats Vanilla, 5 Soaked/Wild
};

struct TrackSectionDef {
    std::string   symbol;            // without ":tks"
    std::string   internal_name;     // +0x00 (equals the symbol in retail data)
    std::string   sid;               // "foo:sid"
    std::uint32_t entry_curve = 0;   // +0x08
    std::uint32_t exit_curve = 0;    // +0x0C
    std::uint32_t special = 0;       // +0x10 special-piece type (Loop, Corkscrew…)
    std::uint32_t direction = 0;     // +0x14 0 straight, 1 left, 2 right
    std::uint32_t entry_flags = 0;   // +0x18
    std::uint32_t exit_flags = 0;    // +0x1C
    // spline symbols ("" when absent): main L/R, join L/R, extra L/R
    std::string   spline_left, spline_right;
    std::string   join_spline_left, join_spline_right;
    std::string   extra_spline_left, extra_spline_right;
    std::uint32_t unk18 = 0;         // +0x44 (0 or 25)
    std::uint32_t entry_slope = 0;   // +0x48
    std::uint32_t entry_bank = 0;    // +0x4C
    std::string   entry_group;       // +0x50 char*
    std::uint32_t unk25 = 0;         // +0x60
    std::uint32_t exit_slope = 0;    // +0x64
    std::uint32_t exit_bank = 0;     // +0x68
    std::string   exit_group;        // +0x6C char*
    std::vector<TrackSpeed> speeds;  // +0x70 count, +0x74 ptr
    std::uint32_t tower_ride_base = 0;
    float         reverser_val = 0;
    float         water_splash[2] = {0, 0};
    std::int32_t  anim[9];           // Vanilla inline animation indices (-1 = none)
    std::string   water_spline_left, water_spline_right;
    // Soaked / Wild extension (entry_flags & 0x00400000)
    std::uint32_t version = 1;       // 1 vanilla, 2 soaked, 3 wild
    std::string   auto_group;
    std::uint32_t entry_wide = 0, exit_wide = 0;
    std::uint32_t soaked_options = 0;
    std::vector<std::string> groups_is_at_entry, groups_is_at_exit;
    std::vector<std::string> groups_must_have_at_entry, groups_must_have_at_exit;
    std::vector<std::string> groups_must_not_be_at_entry, groups_must_not_be_at_exit;
    // Wild extension
    std::uint32_t splitter_half = 0;
    std::uint32_t rotator_type = 0;
    TrackSectionDef() { for (auto& a : anim) a = -1; }
};

// --------------------------------------------------------------- trr

struct TrackedRideSectionCost {
    std::string   name;
    std::uint32_t cost = 0;
};

// TrackedRide_S/W group definitions drive the automatic start / middle / end
// piece selection (e.g. a slide's top piece). Each side is a group name
// (flags&0xFF == 0), a direct tks reference (== 1) or nothing (== 2); the
// upper flag bits are condition modifiers (rct3constants.h Group_Flags).
struct TrackGroupRef {
    std::uint32_t flags = 0;
    std::string   value;
};
struct TrackGroupPair { TrackGroupRef a, b; };

struct TrackedRideInfo {
    std::string   symbol;                  // without ":trr"
    bool          soaked_wild = false;     // TrackedRide_S/W layout
    std::uint32_t attraction_type = 0;     // Vanilla Attraction_V.type
    std::vector<std::string> sections;     // "foo:tks", the ride's piece catalogue
    std::vector<TrackedRideSectionCost> section_costs;
    std::vector<std::string> train_names;  // RideTrain names (trn/rtn symbols)
    std::string   cable_lift, lift_car, station_name;
    std::vector<std::string> track_paths;  // relative dir of the piece OVLs
    std::uint32_t platform_height_over_track = 0;
    std::uint32_t only_on_water = 0;
    std::uint32_t start_preset = 0, start_possibilities = 0, blocks_possible = 0;
    float         station_roll_speed = 0;
    float         launched_speed_preset = 0, chain_speed_preset = 0;
    float         chain_speed_min = 0, chain_speed_max = 0;
    float         launched_speed_min = 0, launched_speed_max = 0;
    float         constant_speed_preset = -1;
    std::uint32_t loop_not_allowed = 0;
    std::uint32_t car_rotation = 0;
    std::uint32_t auto_complete = 0;
    std::uint32_t upkeep_per_train = 0, upkeep_per_car = 0, upkeep_per_station = 0;
    std::uint32_t cost_per_4h_height = 0;
    std::string   other_top, other_mid, tower_top, tower_mid;   // tks symbols
    std::string   track_spline, track_big_spline, car_spline, car_swing_spline;
    std::vector<std::vector<TrackGroupPair>> group_definitions;
};

// --------------------------------------------------------------- ric / rit

// RideCar_V (240 B) + Soaked/Wild extensions (rct3-importer car.h).
struct RideCarInfo {
    std::string   symbol;             // without ":ric"
    std::string   name, username;
    std::uint8_t  seating = 0;        // RCT3_Seating
    std::uint8_t  version = 0;        // 0 vanilla, 2 soaked, 3 wild
    std::string   svd;                // car body "foo:svd"
    float         inertia = 0;
    std::string   moving_svd;         // rotating / swinging part
    std::uint32_t axis_type = 0;      // 0 none, 2 across track, 3 along track, 4 vertical
    float         moving_inertia = -1;
    float         axis_stability = 0, axis_momentum = 0, axis_maximum = -1;
    std::uint32_t bobbing = 0;
    std::int32_t  anims[20];          // indices into the svd's animation list (-1 = none)
    std::vector<std::uint32_t> seat_types;
    std::string   wheels[4];          // front right, front left, back right, back left svd
    std::uint32_t wheels_flipped[4] = {0, 0, 0, 0};
    std::string   axle_front, axle_rear;
    std::uint32_t axle_front_type = 0, axle_rear_type = 0;
    RideCarInfo() { for (auto& a : anims) a = -1; }
};

// RideTrain_V (188 B) + Soaked (16 B) / Wild (36 B) extensions (train.h).
struct RideTrainInfo {
    std::string   symbol;             // without ":rit"
    std::string   name, description;  // txt lookups (plain strings)
    // front, second, mid (repeated), penultimate, rear, link — "foo:ric"
    std::string   cars[6];
    std::uint32_t car_count_min = 0, car_count_max = 0, car_count_def = 0;
    float         friction_dynamic = 0, friction_static = 0, air_drag = 0;
    float         camera_lookahead = 0;
    std::uint32_t overtake = 0;
    std::string   liftcar_left, liftcar_right;   // spl
    std::string   station;            // platform style, overrides the trr's
    std::uint32_t version = 1;
    std::string   sext_name;
    std::uint32_t icon = 0;           // 0 none, 1 soaked, 2 wild
};

// Readers. `lf_index` indexes parser.side(side).linkedfiles.
bool read_spline(const OvlParser& parser, OvlSide side, std::size_t lf_index,
                 Spline& out, std::string& err);
bool read_track_section(const OvlParser& parser, OvlSide side, std::size_t lf_index,
                        TrackSectionDef& out, std::string& err);
bool read_scenery_item(const OvlParser& parser, OvlSide side, std::size_t lf_index,
                       SceneryItemInfo& out, std::string& err);
bool read_tracked_ride(const OvlParser& parser, OvlSide side, std::size_t lf_index,
                       TrackedRideInfo& out, std::string& err);
bool read_ride_car(const OvlParser& parser, OvlSide side, std::size_t lf_index,
                   RideCarInfo& out, std::string& err);
bool read_ride_train(const OvlParser& parser, OvlSide side, std::size_t lf_index,
                     RideTrainInfo& out, std::string& err);

}  // namespace ovl
