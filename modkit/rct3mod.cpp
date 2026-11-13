// rct3mod: make, build and install RCT3 mods described by a mod.toml
// (modkit/include/rct3mod/Mod.hpp).
//
//   rct3mod new <dir> [--id ID]
//   rct3mod build [<dir>] [--game <Assets>] [--preview <dir>]
//   rct3mod install [<dir>] [--game <Assets>]
//   rct3mod uninstall <id> [--game <Assets>]
//   rct3mod list [--game <Assets>]
//
// --game defaults to $RCT3_ASSETS.
#include "rct3mod/Mod.hpp"

#include "CLI11.hpp"

#include <cstdlib>
#include <iostream>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    CLI::App app{"rct3mod: build and install RCT3 mods (mod.toml)"};
    app.require_subcommand(1);
    std::string game;
    if (const char* e = std::getenv("RCT3_ASSETS")) game = e;
    auto add_game = [&](CLI::App* c) {
        c->add_option("--game", game, "the game's Assets folder (default $RCT3_ASSETS)");
    };
    auto need_game = [&]() -> fs::path {
        if (game.empty()) throw std::runtime_error("no game folder: pass --game <Assets> or set RCT3_ASSETS");
        return game;
    };

    std::string dir = ".", id, preview;
    auto* c_new = app.add_subcommand("new", "start a mod folder with a mod.toml");
    c_new->add_option("dir", dir, "folder to create")->required();
    c_new->add_option("--id", id, "mod id (default: the folder name)");

    auto* c_build = app.add_subcommand("build", "write the mod's OVLs to <dir>/build/Assets");
    c_build->add_option("dir", dir, "mod folder (default .)");
    c_build->add_option("--preview", preview, "also write the textures made, as TGA, here");
    add_game(c_build);

    auto* c_install = app.add_subcommand("install", "build, then copy into the game");
    c_install->add_option("dir", dir, "mod folder (default .)");
    add_game(c_install);

    auto* c_uninstall = app.add_subcommand("uninstall", "remove an installed mod's files");
    c_uninstall->add_option("id", id, "mod id")->required();
    add_game(c_uninstall);

    auto* c_list = app.add_subcommand("list", "installed mods");
    add_game(c_list);

    CLI11_PARSE(app, argc, argv);
    try {
        if (*c_new) {
            if (id.empty()) id = fs::path(dir).filename().string();
            rct3mod::new_mod(dir, id);
            std::cout << "created " << (fs::path(dir) / "mod.toml").string()
                      << ": put your picture beside it and edit it\n";
        } else if (*c_build) {
            auto m = rct3mod::load_mod(dir);
            const fs::path out = fs::path(dir) / "build" / "Assets";
            auto files = rct3mod::build_mod(m, need_game(), out, preview);
            for (auto& f : files) std::cout << "  " << f.generic_string() << "\n";
            std::cout << m.id << " " << m.version << ": " << files.size() << " files -> " << out.string() << "\n";
        } else if (*c_install) {
            auto m = rct3mod::load_mod(dir);
            auto r = rct3mod::install_mod(m, need_game());
            std::cout << m.id << " " << m.version << " installed: " << r.files.size() << " files in "
                      << need_game().string() << "\n";
        } else if (*c_uninstall) {
            auto r = rct3mod::uninstall_mod(id, need_game());
            std::cout << r.id << " uninstalled: " << r.files.size() << " files removed\n";
        } else if (*c_list) {
            auto mods = rct3mod::installed_mods(need_game());
            if (mods.empty()) std::cout << "no mods installed\n";
            for (auto& r : mods) std::cout << r.id << " (" << r.files.size() << " files)\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
