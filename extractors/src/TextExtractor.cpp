#include "ovl/extract/TextExtractor.hpp"

#include "ovl/OvlParser.hpp"
#include "ovl/extract/ShapeReader.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace ovl {

namespace {

std::string json_string(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    o += "\\u00";
                    o += hex[c >> 4];
                    o += hex[c & 15];
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o + "\"";
}

std::string pair_stem(const OvlParser& parser) {
    std::string s = std::filesystem::path(parser.side(OvlSide::Common).ovlname).filename().string();
    for (const char* suf : {".common.ovl", ".unique.ovl", ".ovl"}) {
        std::string suffix(suf);
        if (s.size() >= suffix.size() &&
            s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0)
            return s.substr(0, s.size() - suffix.size());
    }
    return s;
}

}  // namespace

ExtractResult TextExtractor::extract(const OvlParser& parser, const ExtractContext& ctx) {
    ExtractResult res{};
    std::map<std::string, std::string> texts;  // symbol (without tag) → UTF-8
    for (std::size_t s = 0; s < (parser.has_unique() ? 2u : 1u); ++s) {
        auto side = static_cast<OvlSide>(s);
        const auto& d = parser.side(side);
        for (std::size_t i = 0; i < d.linkedfiles.size(); ++i) {
            if (parser.loader_by_id(d.linkedfiles[i].loaderreference.loadernumber, side).tag != "txt")
                continue;
            std::string sym = linked_symbol(parser, side, i, false);
            try {
                texts[sym] = read_text(parser, side, i);
            } catch (const std::exception& e) {
                ++res.errors;
                ctx.log("txt: " + sym + ": " + e.what());
            }
        }
    }
    if (texts.empty()) return res;

    std::filesystem::create_directories(ctx.output_dir);
    auto out_path = ctx.output_dir / (pair_stem(parser) + ".txt.json");
    if (!ctx.overwrite && std::filesystem::exists(out_path)) {
        ctx.log("skip (exists): " + out_path.filename().string());
        return res;
    }
    std::ofstream o(out_path, std::ios::binary);
    o << "{\n";
    bool first = true;
    for (const auto& [sym, text] : texts) {
        o << (first ? "" : ",\n") << "  " << json_string(sym) << ": " << json_string(text);
        first = false;
    }
    o << "\n}\n";
    ++res.files_written;
    ctx.log("wrote " + out_path.filename().string() + " (" + std::to_string(texts.size()) + " strings)");
    return res;
}

}  // namespace ovl
