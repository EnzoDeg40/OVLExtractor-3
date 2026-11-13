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
#include "ovl/OvlImage.hpp"
#include "ovl/PairView.hpp"
#include "ovl/SceneryCodec.hpp"
#include "ovl/TextOvl.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using namespace ovl;

namespace {

std::u16string utf16(const std::string& utf8) {
    // Reuse the JSON reader's UTF-8 decoding.
    std::string esc;
    for (char c : utf8) {
        if (c == '"' || c == '\\') esc += '\\';
        esc += c;
    }
    return read_text_json("{\"k\":\"" + esc + "\"}").at(0).text;
}

std::string base(const std::string& name_tag) { return name_tag.substr(0, name_tag.rfind(':')); }

// Windows-style path of `p` relative to Assets, without the .common.ovl suffix.
std::string game_path(const fs::path& rel_common) {
    auto s = rel_common.generic_string();
    s = s.substr(0, s.size() - std::string(".common.ovl").size());
    std::replace(s.begin(), s.end(), '/', '\\');
    return s;
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
    std::map<std::string, std::string> opt;
    for (int i = 2; i + 1 < argc; i += 2) opt[argv[i]] = argv[i + 1];
    for (auto k : {"--assets", "--visual", "--svd", "--style", "--sid", "--out", "--name", "--title"})
        if (!opt.count(k)) throw std::runtime_error(std::string("missing ") + k);
    const fs::path assets = opt["--assets"];
    const fs::path visual_rel = opt["--visual"], style_rel = opt["--style"], out_rel = opt["--out"];
    const std::string nn = opt["--name"];

    auto V = PairView::load(assets / visual_rel);
    auto S = PairView::load(assets / style_rel);
    auto* so = S.find(opt["--sid"] + ":sid");
    auto* vo = V.find(opt["--svd"] + ":svd");
    if (!so || !vo) throw std::runtime_error("sid or svd not found");

    // Two pairs, as the game does: <NewName> holds the visual (ftx, shs, svd);
    // Style holds the item (sid), its name and the StyleIndex. The game scans
    // Style/ for files named Style.common.ovl and loads the visual pair through
    // the sid's ovl_path when the item is used; pointing ovl_path at the Style
    // pair itself makes the game load it twice ("Detected cycle").
    OvlBuilder b(5), sb(5);
    std::map<std::string, std::string> renamed;  // old name:tag -> new name:tag
    auto rename = [&](const std::string& old) {
        auto it = renamed.find(old);
        return it == renamed.end() ? old : it->second;
    };

    // Textures and shapes that live in the visual pair get new names; anything
    // else (shared atlases, txs styles) stays a reference to the game's own.
    Svd svd = decode_svd(V, vo->data);
    std::vector<std::pair<std::string, Shs>> shapes;
    for (auto& l : svd.lods) {
        if (!l.shs) continue;
        auto* o = V.find(*l.shs);
        if (!o) continue;
        Shs s = decode_shs(V, o->data);
        for (auto& m : s.meshes)
            if (m.ftx && V.find(*m.ftx) && !renamed.count(*m.ftx))
                renamed[*m.ftx] = nn + "_" + base(*m.ftx) + ":ftx";
        renamed[*l.shs] = nn + "_" + base(*l.shs) + ":shs";
        shapes.emplace_back(*l.shs, std::move(s));
    }
    for (auto& [old, nw] : renamed) {
        if (old.substr(old.size() - 4) != ":ftx") continue;
        Ftx f = decode_ftx(V, V.find(old)->data);
        if (opt.count("--tint")) {
            float t[3];
            if (std::sscanf(opt["--tint"].c_str(), "%f,%f,%f", &t[0], &t[1], &t[2]) != 3)
                throw std::runtime_error("--tint R,G,B (0-255)");
            for (auto& fr : f.frames)
                for (std::size_t i = 0; i < 256; ++i) {
                    auto* p = &fr.palette[4 * i];  // B, G, R, 0
                    const float lum = (0.114f * p[0] + 0.587f * p[1] + 0.299f * p[2]) / 255.f;
                    for (int c = 0; c < 3; ++c)
                        p[c] = std::uint8_t(std::lround(std::min(255.f, lum * t[2 - c])));
                }
        }
        encode_ftx(b, base(nw), f);
    }
    for (auto& [old, s] : shapes) {
        for (auto& m : s.meshes)
            if (m.ftx) m.ftx = rename(*m.ftx);
        encode_shs(b, base(rename(old)), s);
    }
    for (auto& l : svd.lods)
        if (l.shs) l.shs = rename(*l.shs);
    encode_svd(b, nn, svd);

    Sid sid = decode_sid(S, so->data);
    sid.svds = {nn + ":svd"};
    sid.name = nn + "_Name:txt";
    sid.ovl_path = game_path(out_rel / (nn + ".common.ovl"));
    if (opt.count("--cost")) {
        sid.set_cost(std::stoi(opt["--cost"]));
        sid.set_removal_cost(-sid.cost() * 9 / 10);
    }
    encode_sid(sb, nn, sid);
    add_texts(sb, {{nn + "_Name", utf16(opt["--title"])}});
    // A Style file tells the menus which theme its items belong to (0 Vanilla,
    // 1 Wild West, 2 Spooky, 3 Space, 4 Adventure, 5 Atlantis, 6 Island
    // Paradise, 7 Jungle, 8 Prehistoric); without it the items are not listed.
    sb.add_data_symbol(OvlSide::Common, "StyleIndex:int",
                       opt.count("--style-index") ? std::stoul(opt["--style-index"]) : 0);

    // File references (the OVLs the pair depends on: icons, shared textures…)
    // of both source pairs, re-anchored to the new folder. The game writes them
    // with backslashes: "..\\..\\SceneryIcons".
    auto add_refs = [&](OvlBuilder& dst, const PairView& src, const fs::path& src_rel) {
        const fs::path dir = (assets / src_rel).parent_path();
        for (auto& ref : src.references(OvlSide::Unique)) {
            std::string r = ref;
            std::replace(r.begin(), r.end(), '\\', '/');
            auto target = (dir / r).lexically_normal();
            auto rel = target.lexically_relative(assets / out_rel).generic_string();
            std::replace(rel.begin(), rel.end(), '/', '\\');
            dst.add_file_reference(OvlSide::Unique, rel);
        }
    };
    add_refs(sb, S, style_rel);
    add_refs(b, V, visual_rel);

    fs::create_directories(assets / out_rel);
    auto save = [&](OvlBuilder& bb, const std::string& stem) {
        auto im = bb.build();
        const auto outbase = (assets / out_rel / stem).string();
        write_image_file(im[0], outbase + ".common.ovl");
        write_image_file(im[1], outbase + ".unique.ovl");
        std::cout << "wrote " << outbase << ".common.ovl / .unique.ovl\n";
    };
    save(b, nn);
    save(sb, "Style");
    std::cout << "sid path " << *sid.ovl_path << ", cost " << sid.cost() << "\n";
    for (auto& [o, n] : renamed) std::cout << "  " << o << " -> " << n << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::string cmd = argc > 1 ? argv[1] : "";
        if (cmd == "text") return text_cmd(argc, argv);
        if (cmd == "scenery-clone") return clone_cmd(argc, argv);
        std::cerr << "usage: ovlwrite text ... | scenery-clone ...  (see the header of ovlwrite.cpp)\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
