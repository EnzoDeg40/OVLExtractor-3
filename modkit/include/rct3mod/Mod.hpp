#pragma once

// A mod is a folder with a mod.toml manifest and the files it names:
//
//   [mod]
//   id = "enzo"                  # letters, digits, - and _
//   name = "Enzo's paths"
//   version = "1.0.0"
//   author = "Enzo"
//
//   [[path]]                     # one table per path type (rct3mod::PathType)
//   name = "Enzo"
//   title = "Enzo"
//   base = "Asphalt"
//   texture = "photo.jpg"
//   crop = [0, 544, 3120]        # x, y, size (optional)
//   icon_crop = [560, 1200, 1750]
//
//   [[scenery]]                  # one table per item (rct3mod::SceneryItem)
//   ...
//
// build_mod writes the mod's OVLs to <mod>/build/Assets/; install_mod copies
// them into the game's Assets folder and records each file in
// Assets/.rct3mod/<id>.files, which is what uninstall_mod removes. Nothing of
// the game's own is ever overwritten.

#include "rct3mod/Content.hpp"

#include <string>
#include <vector>

namespace rct3mod {

struct Mod {
    fs::path                 dir;  // folder of mod.toml; relative files resolve from it
    std::string              id, name, version, author;
    std::vector<PathType>    paths;
    std::vector<SceneryItem> scenery;
};

Mod load_mod(const fs::path& dir);

// Writes a starter mod.toml (one path type) into dir.
void new_mod(const fs::path& dir, const std::string& id);

// Returns the files written, relative to the output Assets folder.
std::vector<fs::path> build_mod(const Mod& m, const fs::path& game_assets, const fs::path& out_assets,
                                const fs::path& preview = {});

struct Installed {
    std::string           id;
    std::vector<fs::path> files;  // relative to Assets
};

// Builds into <mod>/build/Assets, then copies into game_assets. Replaces a
// previous install of the same mod; refuses files that exist and are not its.
Installed install_mod(const Mod& m, const fs::path& game_assets);
// Removes the recorded files (and folders left empty). Returns them.
Installed uninstall_mod(const std::string& id, const fs::path& game_assets);
std::vector<Installed> installed_mods(const fs::path& game_assets);

}  // namespace rct3mod
