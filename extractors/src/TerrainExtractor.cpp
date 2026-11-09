#include "ovl/extract/TerrainExtractor.hpp"

#include "ovl/BinaryReader.hpp"
#include "ovl/OvlParser.hpp"
#include "ovl/extract/ShapeReader.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace ovl {

namespace {

std::optional<BinaryReader> open_at(const OvlParser& parser, std::uint32_t off) {
    auto p = parser.offset_to_position(off);
    if (!p.found) return std::nullopt;
    BinaryReader r(parser.side(p.currentOVL).ovlname);
    r.seek(p.position);
    return r;
}

std::string json_string(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
    return o + "\"";
}

std::string json_float(float f) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", static_cast<double>(f));
    return b;
}

std::string hex_colour(std::uint32_t c) {
    char b[16];
    std::snprintf(b, sizeof b, "\"%08X\"", c);
    return b;
}

std::string pair_stem(const OvlParser& parser) {
    std::string s = std::filesystem::path(parser.side(OvlSide::Common).ovlname).filename().string();
    for (std::string suffix : {".common.ovl", ".unique.ovl", ".ovl"})
        if (s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0)
            return s.substr(0, s.size() - suffix.size());
    return s;
}

// Drops a symbol's ":tag".
std::string untagged(const std::string& s) {
    auto colon = s.rfind(':');
    return colon == std::string::npos ? s : s.substr(0, colon);
}

}  // namespace

ExtractResult TerrainExtractor::extract(const OvlParser& parser, const ExtractContext& ctx) {
    ExtractResult res{};
    std::vector<std::string> records;
    for (std::size_t s = 0; s < (parser.has_unique() ? 2u : 1u); ++s) {
        auto side = static_cast<OvlSide>(s);
        const auto& d = parser.side(side);
        for (std::size_t i = 0; i < d.linkedfiles.size(); ++i) {
            if (parser.loader_by_id(d.linkedfiles[i].loaderreference.loadernumber, side).tag != "ter")
                continue;
            const std::string sym = linked_symbol(parser, side, i, false);
            const std::uint32_t b = d.linkedfiles[i].loaderreference.datapointer;
            auto r = open_at(parser, b);
            if (!r) {
                ++res.errors;
                ctx.log("ter: " + sym + ": datapointer unresolved");
                continue;
            }
            std::uint32_t v[15];
            for (auto& x : v) x = r->read_u32();
            auto f = [&](int k) { float out; std::memcpy(&out, &v[k], 4); return out; };
            static const char* kinds[] = {"ground", "cliff", "blended_ground"};
            const std::string kind = v[4] < 3 ? kinds[v[4]] : std::to_string(v[4]);
            std::string o = "  {\"symbol\": " + json_string(sym) +
                ", \"addon\": " + std::to_string(v[2]) +
                ", \"number\": " + std::to_string(v[3]) +
                ", \"type\": " + json_string(kind) +
                ", \"texture\": " + json_string(untagged(resolved_symbol_at(parser, b + 20))) +
                ", \"description\": " + json_string(string_at_slot(parser, v[6], b + 24)) +
                ", \"icon\": " + json_string(string_at_slot(parser, v[7], b + 28)) +
                ", \"colour_simple\": " + hex_colour(v[8]) +
                ", \"colour_map\": " + hex_colour(v[9]) +
                ", \"inv_width\": " + json_float(f(10)) +
                ", \"inv_height\": " + json_float(f(11)) +
                ", \"unk\": [" + json_float(f(12)) + ", " + json_float(f(13)) + ", " + json_float(f(14)) + "]}";
            records.push_back(o);
        }
    }
    if (records.empty()) return res;

    std::filesystem::create_directories(ctx.output_dir);
    auto out_path = ctx.output_dir / (pair_stem(parser) + ".terrain.json");
    if (!ctx.overwrite && std::filesystem::exists(out_path)) {
        ctx.log("skip (exists): " + out_path.filename().string());
        return res;
    }
    std::ofstream o(out_path, std::ios::binary);
    o << "[\n";
    for (std::size_t k = 0; k < records.size(); ++k) o << records[k] << (k + 1 < records.size() ? ",\n" : "\n");
    o << "]\n";
    ++res.files_written;
    ctx.log("wrote " + out_path.filename().string() + " (" + std::to_string(records.size()) + " terrain types)");
    return res;
}

}  // namespace ovl
