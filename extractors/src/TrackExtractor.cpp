#include "ovl/extract/TrackExtractor.hpp"

#include "ovl/OvlParser.hpp"
#include "ovl/extract/ShapeReader.hpp"
#include "ovl/extract/TrackReader.hpp"

#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace ovl {

namespace {

std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (static_cast<unsigned char>(c) >= 0x20) o += c;
    }
    return o + "\"";
}

std::string jnum(double v) {
    if (!std::isfinite(v)) return "null";
    if (std::fabs(v) < 5e-7) v = 0;
    std::ostringstream o;
    o.precision(6);
    o << v;
    return o.str();
}

std::string jvec(const float* v, int n = 3) {
    std::string o = "[";
    for (int i = 0; i < n; ++i) o += (i ? "," : "") + jnum(v[i]);
    return o + "]";
}

std::string jstrs(const std::vector<std::string>& v) {
    std::string o = "[";
    for (std::size_t i = 0; i < v.size(); ++i) o += (i ? "," : "") + jstr(v[i]);
    return o + "]";
}

std::string strip_tag(std::string s) {
    auto cut = s.rfind(':');
    if (cut != std::string::npos) s.resize(cut);
    return s;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string base_name(const OvlParser& p) {
    std::string n = std::filesystem::path(p.side(OvlSide::Common).ovlname).filename().string();
    for (const char* suf : {".common.ovl", ".unique.ovl", ".ovl"}) {
        std::string s(suf);
        if (n.size() > s.size() && n.compare(n.size() - s.size(), s.size(), s) == 0) {
            n.resize(n.size() - s.size());
            break;
        }
    }
    return n;
}

// End pose of a rail pair, measured on the main L/R splines: position = the
// mid-point of the two rails, `dir` = unit travel direction, `right` = unit
// vector from the left to the right rail, `up` = dir × right (LH frame: up
// is right × dir in the right-handed sense). Roll is the angle of `right`
// out of the horizontal plane.
struct Pose {
    float pos[3] = {0, 0, 0};
    float dir[3] = {0, 0, 1};
    float right[3] = {1, 0, 0};
    float up[3] = {0, 1, 0};
    float yaw_deg = 0, pitch_deg = 0, roll_deg = 0;
    float gauge = 0;   // rail distance
};

void normalize(float* v) {
    float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-9f) for (int k = 0; k < 3; ++k) v[k] /= l;
}

bool end_pose(const Spline* L, const Spline* R, bool at_end, Pose& out) {
    if (!L && !R) return false;
    const Spline* one = L ? L : R;
    auto eval = [&](const Spline& s, float p[3], float t[3]) {
        std::size_t segs = s.cyclic ? s.nodes.size() : s.nodes.size() - 1;
        return spline_eval(s, at_end ? segs - 1 : 0, at_end ? 1.f : 0.f, p, t);
    };
    float pl[3], tl[3], pr[3], tr[3];
    if (!eval(*one, pl, tl)) return false;
    if (L && R) {
        if (!eval(*R, pr, tr)) return false;
    } else {
        std::copy(pl, pl + 3, pr);
        std::copy(tl, tl + 3, tr);
    }
    for (int k = 0; k < 3; ++k) {
        out.pos[k] = 0.5f * (pl[k] + pr[k]);
        out.dir[k] = tl[k] / 2 + tr[k] / 2;
        out.right[k] = pr[k] - pl[k];
    }
    normalize(out.dir);
    out.gauge = std::sqrt(out.right[0] * out.right[0] + out.right[1] * out.right[1] +
                          out.right[2] * out.right[2]);
    normalize(out.right);
    // up = dir × right (component formula of the RH cross product; for
    // dir=(0,0,1), right=(1,0,0) — the LH frame's forward/right — it gives
    // (0,1,0)).
    const float* r = out.right; const float* d = out.dir;
    out.up[0] = d[1] * r[2] - d[2] * r[1];
    out.up[1] = d[2] * r[0] - d[0] * r[2];
    out.up[2] = d[0] * r[1] - d[1] * r[0];
    normalize(out.up);
    const float rad = 57.29577951f;
    out.yaw_deg = std::atan2(d[0], d[2]) * rad;   // 0 = +Z, 90 = +X
    out.pitch_deg = std::asin(std::fmax(-1.f, std::fmin(1.f, d[1]))) * rad;
    // roll: signed angle between `up` and the vertical plane through `dir`;
    // positive = right rail lower (banked right).
    out.roll_deg = std::atan2(-r[1], out.up[1]) * rad;
    return true;
}

std::string pose_json(const Pose& p) {
    return "{\"pos\":" + jvec(p.pos) + ",\"dir\":" + jvec(p.dir) + ",\"up\":" + jvec(p.up) +
           ",\"yaw\":" + jnum(p.yaw_deg) + ",\"pitch\":" + jnum(p.pitch_deg) +
           ",\"roll\":" + jnum(p.roll_deg) + ",\"gauge\":" + jnum(p.gauge) + "}";
}

std::string spline_json(const Spline& s) {
    std::ostringstream o;
    o << "{\"cyclic\":" << (s.cyclic ? "true" : "false") << ",\"length\":" << jnum(s.total_length)
      << ",\"nodes\":[";
    for (std::size_t i = 0; i < s.nodes.size(); ++i) {
        const auto& n = s.nodes[i];
        o << (i ? "," : "") << "[" << jvec(n.pos) << "," << jvec(n.cp1) << "," << jvec(n.cp2) << "]";
    }
    o << "],\"lengths\":[";
    for (std::size_t i = 0; i < s.lengths.size(); ++i) o << (i ? "," : "") << jnum(s.lengths[i]);
    o << "]}";
    return o.str();
}

std::string sid_json(const SceneryItemInfo& s) {
    std::ostringstream o;
    o << "{\"symbol\":" << jstr(s.symbol) << ",\"type\":" << s.type
      << ",\"flags\":" << s.flags << ",\"position_type\":" << s.position_type
      << ",\"squares\":[" << s.squares_x << "," << s.squares_z << "]"
      << ",\"position\":" << jvec(s.position) << ",\"size\":" << jvec(s.size)
      << ",\"cost\":" << s.cost << ",\"removal_cost\":" << s.removal_cost
      << ",\"supports\":" << jstr(s.supports) << ",\"svds\":" << jstrs(s.svds)
      << ",\"default_colours\":[" << s.default_colours[0] << "," << s.default_colours[1] << ","
      << s.default_colours[2] << "],\"cells\":[";
    for (std::size_t i = 0; i < s.squares.size(); ++i) {
        const auto& q = s.squares[i];
        o << (i ? "," : "") << "[" << q.flags << "," << q.height_adjust << "," << q.height_span
          << "," << q.direction_type << "]";
    }
    o << "]}";
    return o.str();
}

std::string end_json(std::uint32_t slope, std::uint32_t bank, std::uint32_t curve,
                     const std::string& group, std::uint32_t wide) {
    return "{\"slope\":" + std::to_string(slope) + ",\"slope_name\":" + jstr(tks_slope_name(slope)) +
           ",\"bank\":" + std::to_string(bank) + ",\"bank_name\":" + jstr(tks_bank_name(bank)) +
           ",\"curve\":" + std::to_string(curve) + ",\"group\":" + jstr(group) +
           ",\"wide\":" + std::to_string(wide) + "}";
}

}  // namespace

ExtractResult TrackExtractor::extract(const OvlParser& parser, const ExtractContext& ctx) {
    ExtractResult res;
    std::map<std::string, Spline> splines;            // full symbol (lower-case-insensitive not needed)
    std::map<std::string, SceneryItemInfo> sids;
    std::vector<TrackSectionDef> sections;
    std::vector<TrackedRideInfo> rides;
    std::vector<RideTrainInfo> trains;
    std::map<std::string, RideCarInfo> cars;   // lower "name:ric"

    for (std::size_t s = 0; s < (parser.has_unique() ? 2u : 1u); ++s) {
        auto side = static_cast<OvlSide>(s);
        const auto& d = parser.side(side);
        for (std::size_t i = 0; i < d.linkedfiles.size(); ++i) {
            const auto& lf = d.linkedfiles[i];
            std::string tag = parser.loader_by_id(lf.loaderreference.loadernumber, side).tag;
            std::string err;
            if (tag == "spl") {
                Spline sp;
                if (read_spline(parser, side, i, sp, err)) splines[lower(sp.symbol + ":spl")] = sp;
                else { ctx.log("spl " + linked_symbol(parser, side, i) + ": " + err); ++res.errors; }
            } else if (tag == "sid") {
                SceneryItemInfo si;
                if (read_scenery_item(parser, side, i, si, err)) sids[lower(si.symbol + ":sid")] = si;
                else { ctx.log("sid " + linked_symbol(parser, side, i) + ": " + err); ++res.errors; }
            } else if (tag == "tks") {
                TrackSectionDef t;
                if (read_track_section(parser, side, i, t, err)) sections.push_back(std::move(t));
                else { ctx.log("tks " + linked_symbol(parser, side, i) + ": " + err); ++res.errors; }
            } else if (tag == "rit") {
                RideTrainInfo t;
                if (read_ride_train(parser, side, i, t, err)) trains.push_back(std::move(t));
                else { ctx.log("rit " + linked_symbol(parser, side, i) + ": " + err); ++res.errors; }
            } else if (tag == "ric") {
                RideCarInfo c;
                if (read_ride_car(parser, side, i, c, err)) cars[lower(c.symbol + ":ric")] = c;
                else { ctx.log("ric " + linked_symbol(parser, side, i) + ": " + err); ++res.errors; }
            } else if (tag == "trr") {
                TrackedRideInfo r;
                if (read_tracked_ride(parser, side, i, r, err)) rides.push_back(std::move(r));
                else { ctx.log("trr " + linked_symbol(parser, side, i) + ": " + err); ++res.errors; }
            }
        }
    }
    if (sections.empty() && rides.empty() && splines.empty() && trains.empty() && cars.empty() &&
        sids.empty())
        return res;

    std::set<std::string> used_splines;
    auto find_spline = [&](const std::string& sym) -> const Spline* {
        if (sym.empty()) return nullptr;
        auto it = splines.find(lower(sym));
        if (it == splines.end()) return nullptr;
        used_splines.insert(lower(sym));
        return &it->second;
    };
    auto write = [&](const std::string& file, const std::string& body) {
        auto path = ctx.output_dir / file;
        if (!ctx.overwrite && std::filesystem::exists(path)) {
            ctx.log("skip existing " + path.string());
            return;
        }
        std::ofstream f(path);
        f << body;
        if (f) ++res.files_written; else ++res.errors;
    };
    const std::string base = base_name(parser);

    if (!sections.empty()) {
        std::ostringstream o;
        o << "{\n  \"track_set\": " << jstr(base) << ",\n  \"sections\": [\n";
        for (std::size_t i = 0; i < sections.size(); ++i) {
            const auto& t = sections[i];
            o << "    {\"name\":" << jstr(t.symbol);
            if (t.internal_name != t.symbol) o << ",\"internal_name\":" << jstr(t.internal_name);
            o << ",\"version\":" << t.version
              << ",\"special\":" << t.special << ",\"special_name\":" << jstr(tks_special_name(t.special))
              << ",\"direction\":" << t.direction
              << ",\"entry_flags\":" << t.entry_flags << ",\"exit_flags\":" << t.exit_flags
              << ",\"flag_names\":" << jstrs(tks_flag_names(t.entry_flags | t.exit_flags))
              << ",\n     \"entry\":" << end_json(t.entry_slope, t.entry_bank, t.entry_curve, t.entry_group, t.entry_wide)
              << ",\"exit\":" << end_json(t.exit_slope, t.exit_bank, t.exit_curve, t.exit_group, t.exit_wide)
              << ",\"unk18\":" << t.unk18 << ",\"unk25\":" << t.unk25;
            if (!t.auto_group.empty()) o << ",\"auto_group\":" << jstr(t.auto_group);
            auto groups = [&](const char* k, const std::vector<std::string>& v) {
                if (!v.empty()) o << ",\"" << k << "\":" << jstrs(v);
            };
            groups("groups_is_at_entry", t.groups_is_at_entry);
            groups("groups_is_at_exit", t.groups_is_at_exit);
            groups("groups_must_have_at_entry", t.groups_must_have_at_entry);
            groups("groups_must_have_at_exit", t.groups_must_have_at_exit);
            groups("groups_must_not_be_at_entry", t.groups_must_not_be_at_entry);
            groups("groups_must_not_be_at_exit", t.groups_must_not_be_at_exit);
            if (t.version >= 2) o << ",\"soaked_options\":" << t.soaked_options;
            if (t.version >= 3) o << ",\"splitter_half\":" << t.splitter_half << ",\"rotator_type\":" << t.rotator_type;
            if (t.tower_ride_base) o << ",\"tower_ride_base\":" << t.tower_ride_base;
            if (t.reverser_val != 0) o << ",\"reverser\":" << jnum(t.reverser_val);
            if (t.water_splash[0] != 0 || t.water_splash[1] != 0)
                o << ",\"water_splash\":" << jvec(t.water_splash, 2);
            o << ",\n     \"animations\":[";
            for (int k = 0; k < 9; ++k) o << (k ? "," : "") << t.anim[k];
            o << "],\"speeds\":[";
            for (std::size_t k = 0; k < t.speeds.size(); ++k)
                o << (k ? "," : "") << jvec(t.speeds[k].v, t.version >= 2 ? 5 : 3);
            o << "]";
            auto sp = [&](const char* k, const std::string& sym) {
                if (!sym.empty()) o << ",\"" << k << "\":" << jstr(sym);
            };
            o << ",\n     \"splines\":{\"_\":0";
            sp("left", t.spline_left); sp("right", t.spline_right);
            sp("join_left", t.join_spline_left); sp("join_right", t.join_spline_right);
            sp("extra_left", t.extra_spline_left); sp("extra_right", t.extra_spline_right);
            sp("water_left", t.water_spline_left); sp("water_right", t.water_spline_right);
            o << "}";
            const Spline* L = find_spline(t.spline_left);
            const Spline* R = find_spline(t.spline_right);
            for (const auto* s : {&t.join_spline_left, &t.join_spline_right, &t.extra_spline_left,
                                  &t.extra_spline_right, &t.water_spline_left, &t.water_spline_right})
                find_spline(*s);
            Pose pin, pout;
            if (end_pose(L, R, false, pin) && end_pose(L, R, true, pout)) {
                float len = 0;
                if (L && R) len = 0.5f * (L->total_length + R->total_length);
                else len = (L ? L : R)->total_length;
                o << ",\n     \"geometry\":{\"length\":" << jnum(len) << ",\"entry\":" << pose_json(pin)
                  << ",\"exit\":" << pose_json(pout) << "}";
            }
            auto si = sids.find(lower(t.sid));
            if (si != sids.end()) o << ",\n     \"sid\":" << sid_json(si->second);
            else if (!t.sid.empty()) o << ",\n     \"sid\":{\"symbol\":" << jstr(strip_tag(t.sid)) << "}";
            o << "}" << (i + 1 < sections.size() ? "," : "") << "\n";
        }
        o << "  ],\n  \"splines\": {\n";
        bool first = true;
        for (const auto& [sym, s] : splines) {
            if (!used_splines.count(sym)) continue;
            o << (first ? "" : ",\n") << "    " << jstr(strip_tag(s.symbol)) << ": " << spline_json(s);
            first = false;
        }
        o << "\n  }\n}\n";
        write(base + ".tracks.json", o.str());
    }

    for (const auto& r : rides) {
        std::ostringstream o;
        o << "{\n  \"ride\": " << jstr(r.symbol)
          << ",\n  \"layout\": " << jstr(r.soaked_wild ? "soaked_wild" : "vanilla");
        if (!r.soaked_wild) o << ",\n  \"attraction_type\": " << r.attraction_type;
        o << ",\n  \"track_paths\": " << jstrs(r.track_paths)
          << ",\n  \"train_names\": " << jstrs(r.train_names)
          << ",\n  \"cable_lift\": " << jstr(r.cable_lift)
          << ",\n  \"lift_car\": " << jstr(r.lift_car)
          << ",\n  \"station_name\": " << jstr(r.station_name)
          << ",\n  \"platform_height_over_track\": " << r.platform_height_over_track
          << ",\n  \"only_on_water\": " << r.only_on_water
          << ",\n  \"start_preset\": " << r.start_preset
          << ",\n  \"start_possibilities\": " << r.start_possibilities
          << ",\n  \"blocks_possible\": " << r.blocks_possible
          << ",\n  \"station_roll_speed\": " << jnum(r.station_roll_speed)
          << ",\n  \"chain_speed\": {\"preset\": " << jnum(r.chain_speed_preset) << ", \"min\": "
          << jnum(r.chain_speed_min) << ", \"max\": " << jnum(r.chain_speed_max) << "}"
          << ",\n  \"launched_speed\": {\"preset\": " << jnum(r.launched_speed_preset) << ", \"min\": "
          << jnum(r.launched_speed_min) << ", \"max\": " << jnum(r.launched_speed_max) << "}"
          << ",\n  \"constant_speed_preset\": " << jnum(r.constant_speed_preset)
          << ",\n  \"loop_not_allowed\": " << r.loop_not_allowed
          << ",\n  \"car_rotation\": " << r.car_rotation
          << ",\n  \"auto_complete\": " << r.auto_complete
          << ",\n  \"upkeep\": {\"per_train\": " << r.upkeep_per_train << ", \"per_car\": "
          << r.upkeep_per_car << ", \"per_station\": " << r.upkeep_per_station << "}"
          << ",\n  \"cost_per_4h_height\": " << r.cost_per_4h_height;
        auto sym = [&](const char* k, const std::string& v) {
            if (!v.empty()) o << ",\n  \"" << k << "\": " << jstr(v);
        };
        sym("other_top", r.other_top); sym("other_mid", r.other_mid);
        sym("tower_top", r.tower_top); sym("tower_mid", r.tower_mid);
        sym("track_spline", r.track_spline); sym("track_big_spline", r.track_big_spline);
        sym("car_spline", r.car_spline); sym("car_swing_spline", r.car_swing_spline);
        if (!r.group_definitions.empty()) {
            o << ",\n  \"group_definitions\": [";
            for (std::size_t g = 0; g < r.group_definitions.size(); ++g) {
                o << (g ? ", [" : "[");
                for (std::size_t k = 0; k < r.group_definitions[g].size(); ++k) {
                    const auto& gp = r.group_definitions[g][k];
                    o << (k ? ", " : "") << "[{\"flags\": " << gp.a.flags << ", \"value\": " << jstr(gp.a.value)
                      << "}, {\"flags\": " << gp.b.flags << ", \"value\": " << jstr(gp.b.value) << "}]";
                }
                o << "]";
            }
            o << "]";
        }
        o << ",\n  \"sections\": [";
        for (std::size_t i = 0; i < r.sections.size(); ++i)
            o << (i ? ", " : "") << jstr(strip_tag(r.sections[i]));
        o << "],\n  \"section_costs\": {";
        for (std::size_t i = 0; i < r.section_costs.size(); ++i)
            o << (i ? ", " : "") << jstr(r.section_costs[i].name) << ": " << r.section_costs[i].cost;
        o << "},\n  \"splines\": {";
        bool first = true;
        for (const auto* s : {&r.track_spline, &r.track_big_spline, &r.car_spline, &r.car_swing_spline}) {
            if (const Spline* sp = find_spline(*s)) {
                o << (first ? "\n    " : ",\n    ") << jstr(strip_tag(*s)) << ": " << spline_json(*sp);
                first = false;
            }
        }
        o << "\n  }\n}\n";
        write(r.symbol + ".ride.json", o.str());
    }

    auto car_json = [&](const RideCarInfo& c) {
        static const char* kAnim[20] = {
            "start", "started_idle", "stop", "stopped_idle", "belt_open", "belt_open_idle",
            "belt_close", "belt_closed_idle", "doors_open", "doors_open_idle", "doors_close",
            "doors_closed_idle", "row_both", "row_left", "row_right", "canoe_station_idle",
            "canoe_idle_front", "canoe_idle_back", "canoe_row_idle", "canoe_row"};
        std::ostringstream o;
        o << "{\"symbol\": " << jstr(c.symbol) << ", \"name\": " << jstr(c.name)
          << ", \"seating\": " << int(c.seating) << ", \"version\": " << int(c.version)
          << ", \"svd\": " << jstr(c.svd) << ", \"inertia\": " << jnum(c.inertia);
        if (!c.moving_svd.empty())
            o << ", \"moving_svd\": " << jstr(c.moving_svd) << ", \"axis_type\": " << c.axis_type
              << ", \"moving_inertia\": " << jnum(c.moving_inertia)
              << ", \"axis_stability\": " << jnum(c.axis_stability)
              << ", \"axis_momentum\": " << jnum(c.axis_momentum)
              << ", \"axis_maximum\": " << jnum(c.axis_maximum);
        if (c.bobbing) o << ", \"bobbing\": " << c.bobbing;
        o << ", \"animations\": {";
        bool first = true;
        for (int k = 0; k < 20; ++k) {
            if (c.anims[k] < 0) continue;
            o << (first ? "" : ", ") << "\"" << kAnim[k] << "\": " << c.anims[k];
            first = false;
        }
        o << "}, \"seat_types\": [";
        for (std::size_t k = 0; k < c.seat_types.size(); ++k) o << (k ? ", " : "") << c.seat_types[k];
        o << "]";
        static const char* kWheel[4] = {"front_right", "front_left", "back_right", "back_left"};
        for (int k = 0; k < 4; ++k)
            if (!c.wheels[k].empty())
                o << ", \"wheels_" << kWheel[k] << "\": {\"svd\": " << jstr(c.wheels[k])
                  << ", \"flipped\": " << c.wheels_flipped[k] << "}";
        if (!c.axle_front.empty()) o << ", \"axle_front\": {\"svd\": " << jstr(c.axle_front) << ", \"type\": " << c.axle_front_type << "}";
        if (!c.axle_rear.empty()) o << ", \"axle_rear\": {\"svd\": " << jstr(c.axle_rear) << ", \"type\": " << c.axle_rear_type << "}";
        o << "}";
        return o.str();
    };
    std::set<std::string> cars_written;
    for (const auto& t : trains) {
        static const char* kRole[6] = {"front", "second", "mid", "penultimate", "rear", "link"};
        std::ostringstream o;
        o << "{\n  \"train\": " << jstr(t.symbol) << ",\n  \"name\": " << jstr(t.name.empty() ? t.sext_name : t.name)
          << ",\n  \"description\": " << jstr(t.description)
          << ",\n  \"version\": " << t.version << ",\n  \"icon\": " << t.icon
          << ",\n  \"cars_per_train\": {\"min\": " << t.car_count_min << ", \"max\": " << t.car_count_max
          << ", \"default\": " << t.car_count_def << "}"
          << ",\n  \"friction\": {\"dynamic\": " << jnum(t.friction_dynamic) << ", \"static\": "
          << jnum(t.friction_static) << ", \"air_drag\": " << jnum(t.air_drag) << "}"
          << ",\n  \"camera_lookahead\": " << jnum(t.camera_lookahead)
          << ",\n  \"overtake\": " << t.overtake
          << ",\n  \"station\": " << jstr(t.station);
        if (!t.liftcar_left.empty())
            o << ",\n  \"liftcar_splines\": [" << jstr(t.liftcar_left) << ", " << jstr(t.liftcar_right) << "]";
        o << ",\n  \"car_roles\": {";
        for (int k = 0; k < 6; ++k)
            o << (k ? ", " : "") << "\"" << kRole[k] << "\": " << jstr(strip_tag(t.cars[k]));
        o << "},\n  \"cars\": {";
        bool first = true;
        for (int k = 0; k < 6; ++k) {
            auto it = cars.find(lower(t.cars[k]));
            if (it == cars.end() || cars_written.count(it->first)) continue;
            o << (first ? "\n    " : ",\n    ") << jstr(it->second.symbol) << ": " << car_json(it->second);
            cars_written.insert(it->first);
            first = false;
        }
        o << "\n  }\n}\n";
        write(t.symbol + ".train.json", o.str());
    }
    for (const auto& [k, c] : cars) {
        if (cars_written.count(k)) continue;
        write(c.symbol + ".car.json", car_json(c) + "\n");
    }

    // Scenery items not owned by a track section: station platforms
    // (Tracks/Platforms/<style>), support parts, ride entrances…
    if (sections.empty() && !sids.empty()) {
        std::ostringstream o;
        o << "{\n  \"ovl\": " << jstr(base) << ",\n  \"items\": [";
        bool first = true;
        for (const auto& [k, si] : sids) {
            o << (first ? "\n    " : ",\n    ") << sid_json(si);
            first = false;
        }
        o << "\n  ]\n}\n";
        write(base + ".sids.json", o.str());
    }

    if (sections.empty() && rides.empty() && trains.empty() && cars.empty() && !splines.empty()) {
        std::ostringstream o;
        o << "{\n";
        bool first = true;
        for (const auto& [sym, s] : splines) {
            o << (first ? "" : ",\n") << "  " << jstr(strip_tag(sym)) << ": " << spline_json(s);
            first = false;
        }
        o << "\n}\n";
        write(base + ".splines.json", o.str());
    }
    return res;
}

}  // namespace ovl
