// GUI resources of a real install: the per-loader extra-data stream, the
// tex → flic → btbl chain, fonts (fct) and language strings (txt). Skipped
// when the game is not installed (CI).
#include "ovl/OvlParser.hpp"
#include "ovl/extract/FontExtractor.hpp"
#include "ovl/extract/TextExtractor.hpp"
#include "ovl/extract/TextureExtractor.hpp"
#include "ovl/extract/ShapeReader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace ovl;

namespace {

std::filesystem::path rct3_asset(const std::string& subpath) {
    const char* home = std::getenv("HOME");
    if (!home) return {};
    return std::filesystem::path(home) /
           "Library/Application Support/Steam/steamapps/common/"
           "RollerCoaster Tycoon 3 Complete Edition/"
           "RollerCoaster Tycoon 3 Platinum.app/Contents/Assets" / subpath;
}

std::filesystem::path temp_dir(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / ("ovl_tests_" + name);
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}

}  // namespace

TEST_CASE("Extra-data stream: Resolution's btbl and flic chunks", "[parser][real][gui]") {
    auto path = rct3_asset("gui/1024/Resolution.common.ovl");
    if (!std::filesystem::exists(path)) SKIP("RCT3 Assets not installed");
    OvlParser p;
    p.parse(path);
    const auto& c = p.side(OvlSide::Common);
    REQUIRE(c.extradata.size() == c.loaderreference.size());
    REQUIRE(c.loaderreference.size() == 5);
    // btbl: the FlicHeader table (8 + 4 × 16 B) and the 4 DXT3 256² chains.
    REQUIRE(c.extradata[0].size() == 2);
    CHECK(c.extradata[0][0].size == 72);
    CHECK(c.extradata[0][1].size == 4u * (65536 + 16384 + 4096 + 1024));
    // flic: one 4-byte slot index each, 0..3 in order.
    for (std::size_t i = 1; i < 5; ++i) {
        REQUIRE(c.extradata[i].size() == 1);
        CHECK(c.extradata[i][0].size == 4);
        std::ifstream f(c.ovlname, std::ios::binary);
        f.seekg(static_cast<std::streamoff>(c.extradata[i][0].position));
        std::uint32_t slot = 0;
        f.read(reinterpret_cast<char*>(&slot), 4);
        CHECK(slot == i - 1);
    }
    // The unique side declares the fonts and textures and owns no extra data.
    const auto& u = p.side(OvlSide::Unique);
    for (const auto& chunks : u.extradata) CHECK(chunks.empty());
}

TEST_CASE("tex → flic → btbl: every GUI font page decodes", "[texture][real][gui]") {
    auto path = rct3_asset("gui/1024/Resolution.common.ovl");
    if (!std::filesystem::exists(path)) SKIP("RCT3 Assets not installed");
    OvlParser p;
    p.parse(path);
    const char* names[] = {"GUIFontScaledTiny:tex", "GUIFontScaledSmall:tex",
                           "GUIFontScaledMedium:tex", "GUIFontScaledLarge:tex"};
    for (const char* n : names) {
        auto ref = find_linked(p, n);
        REQUIRE(ref.has_value());
        std::uint32_t w = 0, h = 0;
        std::vector<std::uint8_t> bgra;
        REQUIRE(TextureExtractor::decode_tex_bgra(p, ref->side, ref->index, w, h, bgra));
        CHECK(w == 256);
        CHECK(h == 256);
        REQUIRE(bgra.size() == 256u * 256u * 4u);
        // A glyph page is white with the glyphs in the alpha plane: some
        // texels are opaque, most are transparent.
        std::size_t opaque = 0;
        for (std::size_t i = 3; i < bgra.size(); i += 4) if (bgra[i] == 255) ++opaque;
        CHECK(opaque > 500);
        CHECK(opaque < 256u * 256u / 2);
    }
}

TEST_CASE("FontExtractor writes a glyph table and an upright page", "[font][real][gui]") {
    auto path = rct3_asset("gui/1024/Resolution.common.ovl");
    if (!std::filesystem::exists(path)) SKIP("RCT3 Assets not installed");
    OvlParser p;
    p.parse(path);
    auto out = temp_dir("font");
    ExtractContext ctx;
    ctx.output_dir = out;
    ctx.overwrite = true;
    FontExtractor fx;
    auto r = fx.extract(p, ctx);
    CHECK(r.errors == 0);
    CHECK(r.files_written == 4);
    REQUIRE(std::filesystem::exists(out / "GUIFontScaledLarge.font.json"));
    REQUIRE(std::filesystem::exists(out / "GUIFontScaledLarge.tga"));
    std::ifstream f(out / "GUIFontScaledLarge.font.json");
    std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(json.find("\"face\": \"Arial\"") != std::string::npos);
    CHECK(json.find("\"weight\": 700") != std::string::npos);
    CHECK(json.find("\"glyph_count\": 4210") != std::string::npos);
    // 'A' (code 65) is pre-rendered: 15 × 15 px at (133, 17) of the upright page.
    CHECK(json.find("{\"code\": 65, \"advance_em\": 0.809524, \"spacing_em\": 0, \"offset\": [0, 0], "
                    "\"uv\": [0.519531, 0.875, 0.578125, 0.933594], \"rect\": [133, 17, 148, 32]}")
          != std::string::npos);
    std::filesystem::remove_all(out);
}

TEST_CASE("TextExtractor writes every string of a language file", "[text][real][gui]") {
    auto path = rct3_asset("gui/English.common.ovl");
    if (!std::filesystem::exists(path)) SKIP("RCT3 Assets not installed");
    OvlParser p;
    p.parse(path);
    auto out = temp_dir("text");
    ExtractContext ctx;
    ctx.output_dir = out;
    ctx.overwrite = true;
    TextExtractor tx;
    auto r = tx.extract(p, ctx);
    CHECK(r.errors == 0);
    CHECK(r.files_written == 1);
    std::ifstream f(out / "English.txt.json");
    REQUIRE(f.good());
    std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::size_t lines = 0;
    for (char c : json) if (c == '\n') ++lines;
    CHECK(lines == 12388 + 2);
    CHECK(json.find("\"AOP_RequiredMessage\": \"Add-on-pack required:\"") != std::string::npos);
    std::filesystem::remove_all(out);
}
