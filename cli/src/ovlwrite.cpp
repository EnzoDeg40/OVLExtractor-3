// Write OVL pairs from editable sources.
//
//   ovlwrite text <strings.json> <out>
//       txt OVL from {"Name": "text", ...} -> <out>.common.ovl + <out>.unique.ovl
//
//   ovlwrite scenery-clone --assets <Assets> --visual <rel.common.ovl> --svd <name>
//                          --style <rel.common.ovl> --sid <name>
//                          --out <rel dir> --name <NewName> --title <display name>
//                          [--style-index N] [--tint R,G,B] [--cost N]
//       Re-encodes a game scenery item (sid + svd + its shs / ftx) under a new
//       name, into <Assets>/<out>/: <NewName>.common/unique.ovl (visual) and
//       Style.common/unique.ovl (item, display name, StyleIndex). <out> must be
//       a folder under Style/. Paths are relative to <Assets>.
//
//   ovlwrite path-clone --assets <Assets> --from <PathType> --name <NewName>
//                       --title <display name> --image <picture: tga/png/jpg/bmp>
//                       [--crop x,y,size] [--icon-crop x,y,size]
//                       [--overwrite 1] [--preview <dir>]
//       Copies the path type Path/<From>/ to Path/<NewName>/ under new symbol
//       names, with the picture on the walking surface (the pieces' ftx sheet)
//       and on the ground pages (tex), <title> as its name in the menus and
//       the picture (--icon-crop, default --crop) as its icon.
//
// scenery-clone and path-clone write straight into <Assets>; they are the
// modkit builders (rct3mod::build_scenery_item / build_path_type) behind
// flags. For mods, prefer `rct3mod` and a mod.toml.
#include "ovl/OvlImage.hpp"
#include "ovl/TextOvl.hpp"
#include "rct3mod/Content.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;
using namespace ovl;

namespace {

using Opts = std::map<std::string, std::string>;

Opts options(int argc, char** argv, std::initializer_list<const char*> required) {
    Opts opt;
    for (int i = 2; i + 1 < argc; i += 2) opt[argv[i]] = argv[i + 1];
    for (auto k : required)
        if (!opt.count(k)) throw std::runtime_error(std::string("missing ") + k);
    return opt;
}

std::optional<rct3mod::Crop> crop_opt(const Opts& opt, const char* key) {
    auto it = opt.find(key);
    if (it == opt.end()) return std::nullopt;
    rct3mod::Crop c;
    if (std::sscanf(it->second.c_str(), "%d,%d,%d", &c.x, &c.y, &c.size) != 3)
        throw std::runtime_error(std::string(key) + " x,y,size");
    return c;
}

int text_cmd(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: ovlwrite text <strings.json> <out base name>\n";
        return 2;
    }
    std::ifstream f(argv[2], std::ios::binary);
    if (!f) throw std::runtime_error(std::string("cannot open ") + argv[2]);
    std::stringstream ss;
    ss << f.rdbuf();
    auto entries = read_text_json(ss.str());
    auto im = build_text_ovl(entries);
    std::string out = argv[3];
    write_image_file(im[0], out + ".common.ovl");
    write_image_file(im[1], out + ".unique.ovl");
    std::cout << entries.size() << " strings -> " << out << ".common.ovl / .unique.ovl\n";
    return 0;
}

int clone_cmd(int argc, char** argv) {
    auto opt = options(argc, argv, {"--assets", "--visual", "--svd", "--style", "--sid", "--out", "--name", "--title"});
    rct3mod::SceneryItem s;
    s.name = opt["--name"];
    s.title = opt["--title"];
    s.visual = opt["--visual"];
    s.svd = opt["--svd"];
    s.style = opt["--style"];
    s.sid = opt["--sid"];
    s.folder = opt["--out"];
    if (opt.count("--style-index")) s.theme = std::stoi(opt["--style-index"]);
    if (opt.count("--tint")) {
        std::array<float, 3> t{};
        if (std::sscanf(opt["--tint"].c_str(), "%f,%f,%f", &t[0], &t[1], &t[2]) != 3)
            throw std::runtime_error("--tint R,G,B (0-255)");
        s.tint = t;
    }
    if (opt.count("--cost")) s.cost = std::stoi(opt["--cost"]);
    const fs::path assets = opt["--assets"];
    auto rel = rct3mod::build_scenery_item(s, assets, assets);
    std::cout << "wrote " << (assets / rel).string() << "/" << s.name << " + Style\n";
    return 0;
}

int path_clone_cmd(int argc, char** argv) {
    auto opt = options(argc, argv, {"--assets", "--from", "--name", "--title", "--image"});
    rct3mod::PathType p;
    p.name = opt["--name"];
    p.title = opt["--title"];
    p.base = opt["--from"];
    p.texture = opt["--image"];
    p.crop = crop_opt(opt, "--crop");
    p.icon_crop = crop_opt(opt, "--icon-crop");
    const fs::path assets = opt["--assets"];
    const fs::path dst = assets / "Path" / p.name;
    if (fs::exists(dst) && !opt.count("--overwrite"))
        throw std::runtime_error(dst.string() + " exists (pass --overwrite 1 to replace it)");
    auto rel = rct3mod::build_path_type(p, assets, assets, opt.count("--preview") ? fs::path(opt["--preview"]) : fs::path());
    std::size_t n = 0;
    for (auto& e : fs::directory_iterator(assets / rel))
        if (e.path().filename().string().find(".common.ovl") != std::string::npos) ++n;
    std::cout << n << " pairs -> " << (assets / rel).string() << "\n";
    return 0;
}

}  // namespace
int main(int argc, char** argv) {
    try {
        std::string cmd = argc > 1 ? argv[1] : "";
        if (cmd == "text") return text_cmd(argc, argv);
        if (cmd == "scenery-clone") return clone_cmd(argc, argv);
        if (cmd == "path-clone") return path_clone_cmd(argc, argv);
        std::cerr << "usage: ovlwrite text ... | scenery-clone ... | path-clone ...  (see the header of ovlwrite.cpp)\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
