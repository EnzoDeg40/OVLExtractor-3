#include "ovl/extract/FontExtractor.hpp"

#include "ovl/BinaryReader.hpp"
#include "ovl/OvlParser.hpp"
#include "ovl/extract/ShapeReader.hpp"
#include "ovl/extract/TextureExtractor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace ovl {

namespace {

// Font (fct), 112 bytes at the data pointer (docs §17):
//   +0x00 f32 line_gap   in em (1 or 2 px over the font size)
//   +0x04 f32 ascent     in em
//   +0x08 f32 descent    in em
//   +0x0C u32 0
//   +0x10 u32 glyph_count
//   +0x14 ptr texture    slot resolved to the "<name>:tex" glyph page
//   +0x18 u16 has_codes  1: a sorted code-point table maps glyph → code;
//                        0: glyph i is code i (dense ASCII fonts)
//   +0x1A u16 1
//   +0x1C u32 1          +0x20 u32 0
//   +0x24 u32 weight     Win32 FW_* (400 normal, 700 bold)
//   +0x28 char[32] face  "Tahoma", "Verdana", "Arial", "Lucida Console"
//   +0x48 u32[8]         runtime scratch (uninitialised on disk)
//   +0x68 ptr glyphs     → glyph_count × Glyph (36 B)
//   +0x6C ptr codes      → glyph_count × u16 code point (when has_codes)
// Glyph (36 B):
//   +0x00 f32 advance    pen advance in em
//   +0x04 f32 spacing    usually one pixel in em (0 for unrendered glyphs)
//   +0x08 f32 0
//   +0x0C f32 u0, +0x10 f32 v0, +0x14 f32 u1, +0x18 f32 v1
//                        rectangle in the page, stored orientation (0..1)
//   +0x1C f32 offset_x   bitmap offset from the pen position, pixels
//   +0x20 f32 offset_y
// The pixel size of the font is a separate "<name>:flt" float symbol
// ("GUIFontSizeScaledTiny:flt" = 9.0) declared next to the fct.
struct Glyph {
    std::uint32_t code = 0;
    float advance = 0, spacing = 0, u0 = 0, v0 = 0, u1 = 0, v1 = 0, offset_x = 0, offset_y = 0;
};

struct Font {
    std::string symbol, face, texture;
    float line_gap = 0, ascent = 0, descent = 0;
    std::uint32_t weight = 0, glyph_count = 0;
    bool has_codes = false;
    std::vector<Glyph> glyphs;
};

std::string jnum(double v) {
    if (!std::isfinite(v)) return "null";
    std::ostringstream o;
    o.precision(6);
    o << v;
    return o.str();
}

std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c >= 0x20) o += static_cast<char>(c);
    }
    return o + "\"";
}

bool read_font(const OvlParser& parser, OvlSide side, std::size_t lf_index, Font& f, std::string& err) {
    const auto& lf = parser.side(side).linkedfiles[lf_index];
    f.symbol = linked_symbol(parser, side, lf_index, false);
    auto pr = parser.offset_to_position(lf.loaderreference.datapointer);
    if (!pr.found) { err = "data pointer unresolved"; return false; }
    BinaryReader r(parser.side(pr.currentOVL).ovlname);
    r.seek(pr.position);
    f.line_gap = r.read_f32();
    f.ascent   = r.read_f32();
    f.descent  = r.read_f32();
    r.read_u32();
    f.glyph_count = r.read_u32();
    r.read_u32();                                   // texture slot (relocated)
    f.has_codes = (r.read_u16() != 0);
    r.read_u16();
    r.read_u32();
    r.read_u32();
    f.weight = r.read_u32();
    for (int i = 0; i < 32; ++i) {
        char c = static_cast<char>(r.read_u8());
        if (c == 0) { r.skip(31 - i); break; }
        f.face += c;
    }
    r.skip(32);
    std::uint32_t glyphs_ptr = r.read_u32();
    std::uint32_t codes_ptr  = r.read_u32();
    f.texture = resolved_symbol_at(parser, lf.loaderreference.datapointer + 0x14);
    if (f.glyph_count == 0 || f.glyph_count > 65536) { err = "implausible glyph count"; return false; }

    auto gp = parser.offset_to_position(glyphs_ptr);
    if (!gp.found) { err = "glyph table unresolved"; return false; }
    BinaryReader gr(parser.side(gp.currentOVL).ovlname);
    gr.seek(gp.position);
    f.glyphs.resize(f.glyph_count);
    for (auto& g : f.glyphs) {
        g.advance = gr.read_f32(); g.spacing = gr.read_f32(); gr.read_f32();
        g.u0 = gr.read_f32(); g.v0 = gr.read_f32(); g.u1 = gr.read_f32(); g.v1 = gr.read_f32();
        g.offset_x = gr.read_f32(); g.offset_y = gr.read_f32();
    }
    if (f.has_codes) {
        auto cp = parser.offset_to_position(codes_ptr);
        if (!cp.found) { err = "code table unresolved"; return false; }
        BinaryReader cr(parser.side(cp.currentOVL).ovlname);
        cr.seek(cp.position);
        for (auto& g : f.glyphs) g.code = cr.read_u16();
    } else {
        for (std::uint32_t i = 0; i < f.glyph_count; ++i) f.glyphs[i].code = i;
    }
    return true;
}

void write_tga(const std::filesystem::path& path, std::uint32_t w, std::uint32_t h,
               const std::vector<std::uint8_t>& bgra) {
    std::ofstream f(path, std::ios::binary);
    std::uint8_t hdr[18] = {};
    hdr[2] = 2;
    hdr[12] = static_cast<std::uint8_t>(w & 0xFF);  hdr[13] = static_cast<std::uint8_t>(w >> 8);
    hdr[14] = static_cast<std::uint8_t>(h & 0xFF);  hdr[15] = static_cast<std::uint8_t>(h >> 8);
    hdr[16] = 32;
    hdr[17] = 0x28;  // top-left origin, 8 alpha bits
    f.write(reinterpret_cast<const char*>(hdr), 18);
    f.write(reinterpret_cast<const char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
}

std::string sanitize(std::string s) {
    for (auto& c : s)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') c = '_';
    return s;
}

bool extract_font(const OvlParser& parser, OvlSide side, std::size_t lf_index,
                  const ExtractContext& ctx) {
    Font f;
    std::string err;
    if (!read_font(parser, side, lf_index, f, err)) {
        ctx.log("fct: " + linked_symbol(parser, side, lf_index, false) + " skipped (" + err + ")");
        return false;
    }
    std::string base = sanitize(f.symbol);
    auto json_path = ctx.output_dir / (base + ".font.json");
    auto tga_path  = ctx.output_dir / (base + ".tga");
    if (!ctx.overwrite && std::filesystem::exists(json_path)) {
        ctx.log("skip (exists): " + base);
        return true;
    }

    // The glyph page, flipped upright (it is stored bottom-up, docs §17).
    std::uint32_t w = 0, h = 0;
    std::vector<std::uint8_t> bgra;
    bool page = false;
    if (auto ref = find_linked(parser, f.texture)) {
        page = TextureExtractor::decode_linked_bgra(parser, ref->side, ref->index, w, h, bgra);
    }
    if (page) {
        std::vector<std::uint8_t> flipped(bgra.size());
        for (std::uint32_t y = 0; y < h; ++y)
            std::copy(bgra.begin() + static_cast<std::ptrdiff_t>(y) * w * 4,
                      bgra.begin() + static_cast<std::ptrdiff_t>(y + 1) * w * 4,
                      flipped.begin() + static_cast<std::ptrdiff_t>(h - 1 - y) * w * 4);
        write_tga(tga_path, w, h, flipped);
    } else {
        ctx.log("fct: " + f.symbol + ": glyph page " + f.texture + " not decoded (rects stay in page units)");
    }

    std::size_t rendered = 0;
    std::ofstream o(json_path, std::ios::binary);
    o << "{\n  \"symbol\": " << jstr(f.symbol) << ",\n"
      << "  \"face\": " << jstr(f.face) << ",\n"
      << "  \"weight\": " << f.weight << ",\n"
      << "  \"line_gap_em\": " << jnum(f.line_gap) << ",\n"
      << "  \"ascent_em\": " << jnum(f.ascent) << ",\n"
      << "  \"descent_em\": " << jnum(f.descent) << ",\n"
      << "  \"glyph_count\": " << f.glyph_count << ",\n"
      << "  \"code_table\": " << (f.has_codes ? "true" : "false") << ",\n"
      << "  \"page\": " << jstr(f.texture) << ",\n"
      << "  \"page_file\": " << (page ? jstr(tga_path.filename().string()) : "null") << ",\n"
      << "  \"page_size\": " << (page ? "[" + std::to_string(w) + ", " + std::to_string(h) + "]" : "null") << ",\n"
      << "  \"glyphs\": [\n";
    bool first = true;
    for (const auto& g : f.glyphs) {
        bool has_rect = g.u1 > g.u0 && g.v1 > g.v0;
        if (has_rect) ++rendered;
        o << (first ? "" : ",\n") << "    {\"code\": " << g.code
          << ", \"advance_em\": " << jnum(g.advance)
          << ", \"spacing_em\": " << jnum(g.spacing)
          << ", \"offset\": [" << jnum(g.offset_x) << ", " << jnum(g.offset_y) << "]";
        if (has_rect) {
            o << ", \"uv\": [" << jnum(g.u0) << ", " << jnum(g.v0) << ", " << jnum(g.u1) << ", " << jnum(g.v1) << "]";
            if (page) {
                // Upright pixel rect: x from u, y flipped (page stored bottom-up).
                auto px = [&](float u) { return static_cast<int>(std::lround(u * static_cast<float>(w))); };
                auto py = [&](float v) { return static_cast<int>(std::lround((1.0f - v) * static_cast<float>(h))); };
                o << ", \"rect\": [" << px(g.u0) << ", " << py(g.v1) << ", " << px(g.u1) << ", " << py(g.v0) << "]";
            }
        }
        o << "}";
        first = false;
    }
    o << "\n  ]\n}\n";
    ctx.log("wrote " + base + ".font.json (" + f.face + " " + std::to_string(f.weight) + ", " +
            std::to_string(f.glyph_count) + " glyphs, " + std::to_string(rendered) + " on page " +
            f.texture + ")");
    return true;
}

}  // namespace

ExtractResult FontExtractor::extract(const OvlParser& parser, const ExtractContext& ctx) {
    ExtractResult res{};
    std::filesystem::create_directories(ctx.output_dir);
    for (std::size_t s = 0; s < (parser.has_unique() ? 2u : 1u); ++s) {
        auto side = static_cast<OvlSide>(s);
        const auto& d = parser.side(side);
        for (std::size_t i = 0; i < d.linkedfiles.size(); ++i) {
            if (parser.loader_by_id(d.linkedfiles[i].loaderreference.loadernumber, side).tag != "fct")
                continue;
            try {
                if (extract_font(parser, side, i, ctx)) ++res.files_written;
            } catch (const std::exception& e) {
                ++res.errors;
                ctx.log(std::string("error: ") + e.what());
            }
        }
    }
    return res;
}

}  // namespace ovl
