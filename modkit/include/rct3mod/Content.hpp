#pragma once

// The kinds of content a mod can add, each built from a game object of the
// same kind (the "base") so that everything the format needs and we do not
// author (meshes, supports, sounds, research data) comes from the game. The
// builders read the base from the game's Assets folder and write new OVL
// pairs under an output folder laid out like Assets; they never write into
// the game's own files.

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace rct3mod {

namespace fs = std::filesystem;

// Square crop of a picture, in source pixels. Unset: the largest centred one.
struct Crop {
    int x = 0, y = 0, size = 0;
};

// A path type: Path/<name>/, a copy of Path/<base>/ with the picture on the
// walking surface and the ground pages, <title> in the menus and its own icon.
struct PathType {
    std::string         name;              // letters and digits: folder and symbol names
    std::string         title;             // shown in the game
    std::string         base = "Asphalt";  // a folder of Path/
    fs::path            texture;           // TGA, PNG, JPEG or BMP
    std::optional<Crop> crop;
    fs::path            icon;              // default: texture
    std::optional<Crop> icon_crop;         // default: crop
};

// A scenery item: a game item (its sid in a Style pair, the svd of its
// visual pair) under a new name, in <folder>/ (a folder under Style/).
struct SceneryItem {
    std::string                         name;
    std::string                         title;
    fs::path                            visual;  // pair holding the svd, relative to Assets (.common.ovl)
    std::string                         svd;     // its name, without ":svd"
    fs::path                            style;   // pair holding the sid, relative to Assets
    std::string                         sid;     // its name, without ":sid"
    fs::path                            folder;  // output folder, relative to Assets, under Style/
    int                                 theme = 0;  // StyleIndex: 0 Vanilla, 1 Wild West, 2 Spooky…
    std::optional<std::array<float, 3>> tint;       // recolour the textures (0-255 RGB)
    std::optional<int>                  cost;
};

// Each returns the folder it wrote (relative to out_assets). preview, when
// set, receives TGA copies of the textures it made.
fs::path build_path_type(const PathType& p, const fs::path& game_assets, const fs::path& out_assets,
                         const fs::path& preview = {});
fs::path build_scenery_item(const SceneryItem& s, const fs::path& game_assets, const fs::path& out_assets);

}  // namespace rct3mod
