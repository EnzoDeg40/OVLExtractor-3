#include "ovl/OvlBuilder.hpp"
#include "ovl/OvlImage.hpp"
#include "ovl/OvlParser.hpp"
#include "ovl/TextOvl.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace ovl;
namespace fs = std::filesystem;

namespace {

fs::path rct3_asset(const std::string& subpath) {
    const char* home = std::getenv("HOME");
    if (!home) return {};
    return fs::path(home) /
           "Library/Application Support/Steam/steamapps/common/"
           "RollerCoaster Tycoon 3 Complete Edition/"
           "RollerCoaster Tycoon 3 Platinum.app/Contents/Assets" / subpath;
}

Bytes slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("symbol_hash is djb2 over the lower-cased name:tag", "[writer]") {
    // Stored in Main.common.ovl for the symbol the Mac port renamed to "Xtari".
    REQUIRE(symbol_hash("CheatPeepName_Atari:txt") == 0xF0D5033Fu);
    REQUIRE(symbol_hash("NAME:TXT") == symbol_hash("name:txt"));
}

TEST_CASE("OvlImage round-trips a built pair", "[writer]") {
    auto im = build_text_ovl({{"B", u"two"}, {"A", u"one"}});
    for (auto& side : im) {
        auto bytes = write_image(side);
        REQUIRE(write_image(read_image(bytes)) == bytes);
    }
}

TEST_CASE("Built text OVL parses back with OvlParser", "[writer]") {
    auto dir = fs::temp_directory_path() / "ovl_writer_test";
    fs::create_directories(dir);
    auto im = build_text_ovl({{"Hello", u"Bonjour"}, {"World", u"Monde é"}});
    write_image_file(im[0], dir / "t.common.ovl");
    write_image_file(im[1], dir / "t.unique.ovl");

    OvlParser p;
    REQUIRE_NOTHROW(p.parse(dir / "t.common.ovl"));
    const auto& c = p.side(OvlSide::Common);
    REQUIRE(c.symbolpointers.size() == 2);
    REQUIRE(c.loaderreference.size() == 2);
    REQUIRE(c.linkedfiles.size() == 2);
    for (const auto& s : c.symbolpointers) REQUIRE(s.ispointer == 1);
    // v5 symbols are sorted by hash within their loader type.
    REQUIRE(c.symbolpointers[0].hash < c.symbolpointers[1].hash);
    REQUIRE((c.symbolpointers[0].hash == symbol_hash("Hello:txt") ||
             c.symbolpointers[1].hash == symbol_hash("Hello:txt")));
    REQUIRE(p.has_resource("World:txt"));
    REQUIRE(c.extradata[0].size() == 1);  // the {u32 1} chunk on the first loader
    fs::remove_all(dir);
}

TEST_CASE("read_text_json decodes escapes and UTF-8", "[writer]") {
    auto e = read_text_json(R"({"A": "x\"yé", "B":"cafÃ© →"})");
    REQUIRE(e.size() == 2);
    REQUIRE(e[0].text == u"x\"yé");
    auto f = read_text_json("{\"C\": \"caf\xC3\xA9\"}");
    REQUIRE(f[0].text == u"café");
}

TEST_CASE("Text writer reproduces ContentPack0001 byte for byte (minus debug tail)",
          "[writer][real]") {
    auto path = rct3_asset("ContentPacks/ContentPack0001.common.ovl");
    if (!fs::exists(path)) SKIP("RCT3 Assets not installed");
    auto im = build_text_ovl({{"Name", u"ContentPackName_WildAddOn"}});
    const char* sides[] = {"common", "unique"};
    for (int s = 0; s < 2; ++s) {
        auto game = slurp(rct3_asset(std::string("ContentPacks/ContentPack0001.") + sides[s] + ".ovl"));
        auto ours = write_image(im[s]);
        REQUIRE(ours.size() < game.size());
        // The game files carry a FilesAndLines source map after the data, which
        // the game does not read; everything before it must match.
        REQUIRE(Bytes(game.begin(), game.begin() + std::ptrdiff_t(ours.size())) == ours);
    }
}
