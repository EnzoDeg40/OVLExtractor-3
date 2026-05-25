#include "ovl/OvlParser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>

using namespace ovl;

namespace {

// Returns a path that exists locally if the user has RCT3 installed on macOS
// via Steam. Tests using this fixture are skipped silently if the asset
// directory is absent (CI environments will not have the game installed).
std::filesystem::path rct3_asset(const std::string& subpath) {
    const char* home = std::getenv("HOME");
    if (!home) return {};
    std::filesystem::path p =
        std::filesystem::path(home) /
        "Library/Application Support/Steam/steamapps/common/"
        "RollerCoaster Tycoon 3 Complete Edition/"
        "RollerCoaster Tycoon 3 Platinum.app/Contents/Assets" / subpath;
    return p;
}

}  // namespace

TEST_CASE("OvlParser parses nullbmp.common.ovl (smallest real OVL)", "[parser][real]") {
    auto path = rct3_asset("nullbmp.common.ovl");
    if (!std::filesystem::exists(path)) {
        SKIP("RCT3 Assets not installed at " + path.string());
    }
    OvlParser p;
    REQUIRE_NOTHROW(p.parse(path));
    const auto& d = p.side(OvlSide::Common);
    REQUIRE(d.h1.magic == kOvlMagic);
    // Sanity: should have 9 chunks recorded.
    REQUIRE(d.chunks.size() == 9);
}

TEST_CASE("OvlParser parses Main.common.ovl (medium real OVL)", "[parser][real]") {
    auto path = rct3_asset("Main.common.ovl");
    if (!std::filesystem::exists(path)) {
        SKIP("RCT3 Assets not installed at " + path.string());
    }
    OvlParser p;
    REQUIRE_NOTHROW(p.parse(path));
    const auto& d = p.side(OvlSide::Common);
    REQUIRE(d.h1.magic == kOvlMagic);
    REQUIRE(d.h1.version == 5);
    REQUIRE(d.chunks.size() == 9);
    REQUIRE(!d.loaders.empty());
    REQUIRE(!d.symbolstring.empty());
}

TEST_CASE("OvlParser rejects non-OVL files", "[parser]") {
    auto tmp = std::filesystem::temp_directory_path() / "not_an_ovl.bin";
    {
        std::ofstream out(tmp, std::ios::binary);
        const char garbage[] = "Hello, this is not an OVL file at all really.";
        out.write(garbage, sizeof(garbage));
    }
    OvlParser p;
    REQUIRE_THROWS(p.parse(tmp));
    std::filesystem::remove(tmp);
}
