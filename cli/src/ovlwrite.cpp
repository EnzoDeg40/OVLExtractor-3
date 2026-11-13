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
//                       --title <display name> --image <picture.tga>
//                       [--crop x,y,size] [--overwrite 1] [--preview <dir>]
//       Copies the path type Path/<From>/ to Path/<NewName>/ under new symbol
//       names, with the picture on the walking surface (the pieces' ftx sheet)
//       and on the ground pages (tex), and <title> as its name in the menus.
#include "ovl/ImageCodec.hpp"
#include "ovl/OvlImage.hpp"
#include "ovl/PairClone.hpp"
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

// ---- path-clone ---------------------------------------------------------------
// A new path type from a game one: every pair of Path/<From>/ is re-encoded
// under the new name (clone_pair renames each symbol, string and file
// reference that contains <From>), and the textures are replaced by an image.
// The game finds path types by walking Path\* and loading Path\X\X_Stub
// (decompile FUN_1006108bc), then Path\X\X_Texture for the ground pages
// (FUN_1006... "_Texture"), so a new folder is all it takes.

std::string replace_all(std::string s, const std::string& a, const std::string& b) {
    for (std::size_t p = 0; (p = s.find(a, p)) != std::string::npos; p += b.size()) s.replace(p, a.size(), b);
    return s;
}

img::Image tinted(const img::Image& im, float r, float g, float b) {
    img::Image o = im;
    for (std::size_t i = 0; i < o.px.size(); i += 4) {
        const float l = (0.299f * o.px[i] + 0.587f * o.px[i + 1] + 0.114f * o.px[i + 2]) / 255.f;
        o.px[i] = std::uint8_t(std::min(255.f, l * r));
        o.px[i + 1] = std::uint8_t(std::min(255.f, l * g));
        o.px[i + 2] = std::uint8_t(std::min(255.f, l * b));
    }
    return o;
}

img::Image flipped_v(const img::Image& im) {
    img::Image o = img::make(im.w, im.h);
    for (int y = 0; y < im.h; ++y) std::copy_n(im.at(0, im.h - 1 - y), std::size_t(im.w) * 4, o.at(0, y));
    return o;
}

int path_clone_cmd(int argc, char** argv) {
    std::map<std::string, std::string> opt;
    for (int i = 2; i + 1 < argc; i += 2) opt[argv[i]] = argv[i + 1];
    for (auto k : {"--assets", "--from", "--name", "--title", "--image"})
        if (!opt.count(k)) throw std::runtime_error(std::string("missing ") + k);
    const fs::path assets = opt["--assets"];
    const std::string from = opt["--from"], nn = opt["--name"];
    const fs::path src = assets / "Path" / from, dst = assets / "Path" / nn;
    if (!fs::is_directory(src)) throw std::runtime_error("no path type folder " + src.string());
    if (fs::exists(dst) && !opt.count("--overwrite"))
        throw std::runtime_error(dst.string() + " exists (pass --overwrite 1 to replace it)");

    // The photo: a square crop (--crop x,y,size in source pixels; default the
    // centred largest square).
    img::Image photo = img::read_tga(opt["--image"]);
    {
        int cx = 0, cy = 0, cs = std::min(photo.w, photo.h);
        cx = (photo.w - cs) / 2;
        cy = (photo.h - cs) / 2;
        if (opt.count("--crop") && std::sscanf(opt["--crop"].c_str(), "%d,%d,%d", &cx, &cy, &cs) != 3)
            throw std::runtime_error("--crop x,y,size");
        photo = img::crop(photo, cx, cy, cs, cs);
    }

    // Pairs of the folder and the symbols they define.
    std::vector<fs::path> pairs;
    for (auto& e : fs::directory_iterator(src)) {
        auto n = e.path().filename().string();
        const std::string suf = ".common.ovl";
        if (n.size() > suf.size() && n.compare(n.size() - suf.size(), suf.size(), suf) == 0)
            pairs.push_back(e.path());
    }
    std::sort(pairs.begin(), pairs.end());
    std::set<std::string> defined;
    for (auto& p : pairs) {
        const PairView pv = PairView::load(p);  // objects() refers into it
        for (auto& o : pv.objects())
            if (!o.name.empty()) defined.insert(o.name);
    }

    const std::string type_name = "Name_PathType_" + from + ":txt";
    auto rename = [&](const std::string& s) {
        if (s.find(':') != std::string::npos) {
            // Symbols: ours when the folder defines them (or the path type's
            // name, which we write); the game's otherwise (icons, research
            // categories, piece names in the text files, shared textures).
            if (defined.count(s) || s == type_name) return replace_all(s, from, nn);
            return s;
        }
        return replace_all(s, from, nn);  // piece names, ovl paths, tex names
    };

    fs::create_directories(dst);
    int written = 0;
    for (auto& p : pairs) {
        const std::string stem = p.filename().string().substr(0, p.filename().string().size() - 11);
        const fs::path up = p.parent_path() / (stem + ".unique.ovl");
        std::array<OvlImage, 2> im{read_image_file(p), read_image_file(up)};
        PairView v(im[0], im[1]);
        const std::string out_stem = replace_all(stem, from, nn);

        // ftx: the top surface of the pieces samples u 0-0.5, v 0.5-1 of the
        // sheet (Asphalt_Flat's up-facing vertices): the photo goes there, a
        // tinted copy in the quadrant above it; the rest (kerbs, wood, metal)
        // stays.
        struct Patch { std::uint32_t addr; Bytes data; };
        std::vector<Patch> patches;
        for (auto& o : v.objects()) {
            if (o.tag != "ftx") continue;
            Ftx f = decode_ftx(v, o.data);
            if (f.frames.size() != 1) continue;
            const int w = int(f.width()), h = int(f.height());
            const auto& fr = f.frames[0];
            img::Image sheet = img::make(w, h);
            for (int i = 0; i < w * h; ++i) {
                const std::uint8_t* c = &fr.palette[4 * std::size_t(fr.texture[std::size_t(i)])];
                std::uint8_t* d = &sheet.px[4 * std::size_t(i)];
                d[0] = c[2]; d[1] = c[1]; d[2] = c[0]; d[3] = 255;
            }
            img::paste(sheet, img::resize(photo, w / 2, h / 2), 0, h / 2);
            img::paste(sheet, tinted(img::resize(photo, w / 2, h / 2), 255, 120, 230), 0, 0);
            auto q = img::quantise(sheet);
            const std::uint32_t frame = *v.ptr(o.data + 0x20);
            patches.push_back({*v.ptr(frame + 0x10), q.palette_bgra});
            patches.push_back({*v.ptr(frame + 0x14), q.indices});
            if (opt.count("--preview")) img::write_tga(sheet, fs::path(opt["--preview"]) / (rename(base(o.name)) + ".tga"));
        }

        CloneHooks h;
        h.rename = rename;
        h.rename_reference = [&](const std::string& r) { return replace_all(r, from, nn); };
        h.patch_block = [&](std::uint32_t start, Bytes& data) {
            for (auto& pt : patches)
                if (pt.addr >= start && pt.addr + pt.data.size() <= start + data.size())
                    std::copy(pt.data.begin(), pt.data.end(), data.begin() + std::ptrdiff_t(pt.addr - start));
        };
        // tex ground pages (inline v2 flics): new colours, same alpha mask, every
        // mip level; pages are stored bottom-up.
        int page = 0;
        h.patch_extra = [&](OvlSide, std::uint32_t, std::vector<Bytes>& chunks) {
            if (chunks.size() != 1 || chunks[0].size() < 32) return;
            Bytes& e = chunks[0];
            auto r32 = [&](std::size_t i) { return std::uint32_t(e[i] | e[i + 1] << 8 | e[i + 2] << 16 | std::uint32_t(e[i + 3]) << 24); };
            const std::uint32_t fmt = r32(0), mips = r32(12);
            if (fmt != 0x13 || r32(4) == 0 || mips == 0 || mips > 16) return;
            const img::Image src_page = page++ % 2 ? tinted(photo, 200, 255, 140) : photo;
            std::size_t q = 16;
            for (std::uint32_t m = 0; m < mips && q + 16 <= e.size(); ++m) {
                const int w = int(r32(q)), hh = int(r32(q + 4));
                const std::size_t n = std::size_t(r32(q + 8)) * r32(q + 12);
                if (w > 0 && hh > 0 && q + 16 + n <= e.size()) {
                    img::Image old = img::decode_dxt3(&e[q + 16], w, hh);
                    img::Image col = flipped_v(img::resize(src_page, w, hh));
                    for (std::size_t i = 3; i < col.px.size(); i += 4) col.px[i] = old.px[i];
                    auto enc = img::encode_dxt3(col);
                    if (enc.size() == n) std::copy(enc.begin(), enc.end(), e.begin() + std::ptrdiff_t(q + 16));
                }
                q += 16 + n;
            }
        };

        OvlBuilder b(5);
        try {
            b = clone_pair(im, h);
        } catch (const std::exception& ex) {
            // A pair with no data at all (Path/<From>/<From>: header only).
            if (v.objects().empty()) {
                fs::copy_file(p, dst / (out_stem + ".common.ovl"), fs::copy_options::overwrite_existing);
                fs::copy_file(up, dst / (out_stem + ".unique.ovl"), fs::copy_options::overwrite_existing);
                ++written;
                continue;
            }
            throw std::runtime_error(p.filename().string() + ": " + ex.what());
        }
        if (v.find(from + ":ptd")) add_texts(b, {{"Name_PathType_" + nn, utf16(opt["--title"])}});
        auto out = b.build();
        write_image_file(out[0], dst / (out_stem + ".common.ovl"));
        write_image_file(out[1], dst / (out_stem + ".unique.ovl"));
        ++written;
    }
    std::cout << written << " pairs -> " << dst.string() << "\n";
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
