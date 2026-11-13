#include "ovl/ImageCodec.hpp"
#include "ovl/TextOvl.hpp"
#include "rct3mod/Mod.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>

namespace fs = std::filesystem;

namespace {

fs::path temp_dir(const std::string& name) {
    auto d = fs::temp_directory_path() / ("rct3mod_test_" + name);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

void write(const fs::path& p, const std::string& s) { std::ofstream(p) << s; }

void write_picture(const fs::path& p) {
    auto im = ovl::img::make(8, 4);
    for (std::size_t i = 0; i < im.px.size(); ++i) im.px[i] = std::uint8_t(i * 7);
    ovl::img::write_tga(im, p);
}

}  // namespace

TEST_CASE("utf16 converts UTF-8, surrogates included", "[modkit]") {
    CHECK(ovl::utf16("Enzo") == u"Enzo");
    CHECK(ovl::utf16("Chemin d'été") == u"Chemin d'été");
    CHECK(ovl::utf16("\xF0\x9F\x8E\xA2") == u"\U0001F3A2");
    CHECK_THROWS(ovl::utf16("\xC3"));
}

TEST_CASE("read_image reads back a TGA", "[modkit]") {
    auto d = temp_dir("img");
    write_picture(d / "a.tga");
    auto im = ovl::img::read_image(d / "a.tga");
    CHECK(im.w == 8);
    CHECK(im.h == 4);
}

TEST_CASE("mod.toml: the starter manifest loads", "[modkit]") {
    auto d = temp_dir("new");
    rct3mod::new_mod(d / "my-mod", "my-mod");
    write_picture(d / "my-mod" / "texture.png");  // name only matters for the check
    auto m = rct3mod::load_mod(d / "my-mod");
    CHECK(m.id == "my-mod");
    REQUIRE(m.paths.size() == 1);
    CHECK(m.paths[0].name == "Mymod");
    CHECK(m.paths[0].base == "Asphalt");
    CHECK(m.scenery.empty());
    CHECK_THROWS(rct3mod::new_mod(d / "my-mod", "my-mod"));  // never overwrites
}

TEST_CASE("mod.toml: fields, defaults and errors", "[modkit]") {
    auto d = temp_dir("load");
    write_picture(d / "p.tga");
    write(d / "mod.toml", R"(
[mod]
id = "test"
[[path]]
name = "Brick"
texture = "p.tga"
crop = [1, 2, 3]
icon_crop = [4, 5, 6]
[[scenery]]
name = "Bench2"
title = "Banc"
visual = "Style/A/B.common.ovl"
svd = "B"
style = "Style/A/Style.common.ovl"
sid = "B"
theme = 2
tint = [255, 0, 10]
cost = 40
)");
    auto m = rct3mod::load_mod(d);
    REQUIRE(m.paths.size() == 1);
    CHECK(m.paths[0].title == "Brick");  // defaults to the name
    CHECK(m.paths[0].crop->size == 3);
    CHECK(m.paths[0].icon_crop->x == 4);
    CHECK(m.paths[0].icon.empty());
    REQUIRE(m.scenery.size() == 1);
    CHECK(m.scenery[0].folder == fs::path("Style/Custom/test/Bench2"));
    CHECK(m.scenery[0].theme == 2);
    CHECK((*m.scenery[0].tint)[2] == 10.f);
    CHECK(*m.scenery[0].cost == 40);

    write(d / "mod.toml", "[mod]\nid = \"bad id\"\n[[path]]\nname=\"A\"\ntexture=\"p.tga\"\n");
    CHECK_THROWS(rct3mod::load_mod(d));
    write(d / "mod.toml", "[mod]\nid = \"x\"\n[[path]]\nname=\"A\"\ntexture=\"missing.png\"\n");
    CHECK_THROWS(rct3mod::load_mod(d));
    write(d / "mod.toml", "[mod]\nid = \"x\"\n");
    CHECK_THROWS(rct3mod::load_mod(d));  // nothing to build
}

TEST_CASE("install / uninstall track their files and never overwrite the game's", "[modkit]") {
    const char* home = std::getenv("HOME");
    const fs::path game = home ? fs::path(home) /
                                     "Library/Application Support/Steam/steamapps/common/"
                                     "RollerCoaster Tycoon 3 Complete Edition/"
                                     "RollerCoaster Tycoon 3 Platinum.app/Contents/Assets"
                               : fs::path();
    if (!fs::exists(game / "Path/Asphalt") || !fs::exists(game / "Path/PathIcons.common.ovl")) SKIP("no RCT3 install");

    // A throwaway Assets folder: the game's Path/ inputs, linked.
    auto d = temp_dir("install");
    auto assets = d / "Assets";
    fs::create_directories(assets / "Path");
    fs::create_directory_symlink(game / "Path/Asphalt", assets / "Path/Asphalt");
    for (auto f : {"PathIcons.common.ovl", "PathIcons.unique.ovl"}) fs::create_symlink(game / "Path" / f, assets / "Path" / f);

    auto mod = d / "mod";
    fs::create_directories(mod);
    write_picture(mod / "p.tga");
    write(mod / "mod.toml", "[mod]\nid = \"t\"\n[[path]]\nname = \"TestPath\"\ntexture = \"p.tga\"\n");
    auto r = rct3mod::install_mod(rct3mod::load_mod(mod), assets);
    CHECK(r.files.size() > 10);
    CHECK(fs::exists(assets / "Path/TestPath/TestPath_Stub.common.ovl"));
    CHECK(fs::exists(assets / "Path/TestPath/TestPathIcons.common.ovl"));
    REQUIRE(rct3mod::installed_mods(assets).size() == 1);

    // Another mod may not take its files.
    auto mod2 = d / "mod2";
    fs::create_directories(mod2);
    write_picture(mod2 / "p.tga");
    write(mod2 / "mod.toml", "[mod]\nid = \"u\"\n[[path]]\nname = \"TestPath\"\ntexture = \"p.tga\"\n");
    CHECK_THROWS(rct3mod::install_mod(rct3mod::load_mod(mod2), assets));

    rct3mod::uninstall_mod("t", assets);
    CHECK_FALSE(fs::exists(assets / "Path/TestPath"));
    CHECK(fs::exists(assets / "Path/Asphalt"));
    CHECK(rct3mod::installed_mods(assets).empty());
    fs::remove_all(d);
}
