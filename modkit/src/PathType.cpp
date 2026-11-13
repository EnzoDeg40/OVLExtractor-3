// Path types. Every pair of Path/<base>/ is re-encoded under the new name
// (clone_pair renames each symbol, string and file reference that contains
// <base>) and the textures are replaced by the picture. The game finds path
// types by walking Path\* and loading Path\X\X_Stub (decompile
// FUN_1006108bc), then Path\X\X_Texture for the ground pages, so a new folder
// is all it takes (docs/LIBOVLNG_WRITER.md §6.1).
#include "Util.hpp"
#include "rct3mod/Content.hpp"

#include "ovl/ImageCodec.hpp"
#include "ovl/OvlImage.hpp"
#include "ovl/PairClone.hpp"
#include "ovl/PairView.hpp"
#include "ovl/SceneryCodec.hpp"
#include "ovl/TextOvl.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace rct3mod {

using namespace ovl;

fs::path build_path_type(const PathType& p, const fs::path& game_assets, const fs::path& out_assets,
                         const fs::path& preview) {
    check_name(p.name, "path name");
    check_name(p.base, "path base");
    if (p.title.empty()) throw std::runtime_error("path " + p.name + ": no title");
    const std::string& from = p.base;
    const std::string& nn = p.name;
    const fs::path src = game_assets / "Path" / from;
    const fs::path rel = fs::path("Path") / nn;
    const fs::path dst = out_assets / rel;
    if (!fs::is_directory(src)) throw std::runtime_error("no path type folder " + src.string());

    const img::Image picture = img::read_image(p.texture);
    const img::Image photo = square(picture, p.crop);

    // Pairs of the folder and the symbols they define.
    std::vector<fs::path> pairs;
    for (auto& e : fs::directory_iterator(src))
        if (ends_with(e.path().filename().string(), ".common.ovl")) pairs.push_back(e.path());
    std::sort(pairs.begin(), pairs.end());
    std::set<std::string> defined;
    for (auto& pp : pairs) {
        const PairView pv = PairView::load(pp);  // objects() refers into it
        for (auto& o : pv.objects())
            if (!o.name.empty()) defined.insert(o.name);
    }

    const std::string type_name = "Name_PathType_" + from + ":txt";
    const std::string icon_name = "Icon_PathType_" + from + ":gsi";
    auto rename = [&](const std::string& s) {
        if (s.find(':') != std::string::npos) {
            // Symbols: ours when the folder defines them (or the path type's
            // name, which we write); the game's otherwise (icons, research
            // categories, piece names in the text files, shared textures).
            if (defined.count(s) || s == type_name || s == icon_name) return replace_all(s, from, nn);
            return s;
        }
        return replace_all(s, from, nn);  // piece names, ovl paths, tex names
    };

    fs::create_directories(dst);
    for (auto& pp : pairs) {
        const std::string stem = strip_suffix(pp.filename().string(), ".common.ovl");
        const fs::path up = pp.parent_path() / (stem + ".unique.ovl");
        std::array<OvlImage, 2> im{read_image_file(pp), read_image_file(up)};
        PairView v(im[0], im[1]);
        const std::string out_stem = replace_all(stem, from, nn);

        // ftx: the top surface of the pieces samples u 0-0.5, v 0.5-1 of the
        // sheet (Asphalt_Flat's up-facing vertices): the picture goes there and
        // in the quadrant above it (sides); the rest (kerbs, wood, metal) stays.
        struct Patch {
            std::uint32_t addr;
            Bytes         data;
        };
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
                d[0] = c[2];
                d[1] = c[1];
                d[2] = c[0];
                d[3] = 255;
            }
            img::paste(sheet, img::resize(photo, w / 2, h / 2), 0, h / 2);
            img::paste(sheet, img::resize(photo, w / 2, h / 2), 0, 0);
            auto q = img::quantise(sheet);
            const std::uint32_t frame = *v.ptr(o.data + 0x20);
            patches.push_back({*v.ptr(frame + 0x10), q.palette_bgra});
            patches.push_back({*v.ptr(frame + 0x14), q.indices});
            if (!preview.empty()) img::write_tga(sheet, preview / (base_name(rename(o.name)) + ".tga"));
        }

        CloneHooks h;
        h.rename = rename;
        h.rename_reference = [&](const std::string& r) {
            // The stub's icon now comes from our own icon pair, beside it.
            if (r == "../PathIcons" || r == "..\\PathIcons") return nn + "Icons";
            return replace_all(r, from, nn);
        };
        h.patch_block = [&](std::uint32_t start, Bytes& data) {
            for (auto& pt : patches)
                if (pt.addr >= start && pt.addr + pt.data.size() <= start + data.size())
                    std::copy(pt.data.begin(), pt.data.end(), data.begin() + std::ptrdiff_t(pt.addr - start));
        };
        // tex ground pages (inline v2 flics): new colours, same alpha mask, every
        // mip level; pages are stored bottom-up.
        h.patch_extra = [&](OvlSide, std::uint32_t, std::vector<Bytes>& chunks) {
            if (chunks.size() != 1 || chunks[0].size() < 32) return;
            Bytes& e = chunks[0];
            const std::uint32_t fmt = rd32(e, 0), mips = rd32(e, 12);
            if (fmt != 0x13 || rd32(e, 4) == 0 || mips == 0 || mips > 16) return;
            std::size_t q = 16;
            for (std::uint32_t m = 0; m < mips && q + 16 <= e.size(); ++m) {
                const int w = int(rd32(e, q)), hh = int(rd32(e, q + 4));
                const std::size_t n = std::size_t(rd32(e, q + 8)) * rd32(e, q + 12);
                if (w > 0 && hh > 0 && q + 16 + n <= e.size()) {
                    img::Image old = img::decode_dxt3(&e[q + 16], w, hh);
                    img::Image col = flipped_v(img::resize(photo, w, hh));
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
                fs::copy_file(pp, dst / (out_stem + ".common.ovl"), fs::copy_options::overwrite_existing);
                fs::copy_file(up, dst / (out_stem + ".unique.ovl"), fs::copy_options::overwrite_existing);
                continue;
            }
            throw std::runtime_error(pp.filename().string() + ": " + ex.what());
        }
        if (v.find(from + ":ptd")) add_texts(b, {{"Name_PathType_" + nn, utf16(p.title)}});
        auto out = b.build();
        write_image_file(out[0], dst / (out_stem + ".common.ovl"));
        write_image_file(out[1], dst / (out_stem + ".unique.ovl"));
    }

    // The menu icon: Path/PathIcons holds every path type's icon (40 px discs
    // in one 256² DXT3 bitmap-table page, PathIcons:tex, one gsi rect each).
    // Clone it beside the stub as <Name>Icons with every symbol renamed (the
    // base's icon becomes Icon_PathType_<Name>, the others <Name>Icon_* so
    // nothing collides), and paint the picture into that icon's rect,
    // keeping its round alpha.
    {
        const fs::path ip = game_assets / "Path" / "PathIcons.common.ovl";
        std::array<OvlImage, 2> im{read_image_file(ip), read_image_file(game_assets / "Path" / "PathIcons.unique.ovl")};
        PairView v(im[0], im[1]);
        const auto* g = v.find(icon_name);
        if (!g) throw std::runtime_error("no " + icon_name + " in Path/PathIcons");
        const std::uint32_t rect = *v.ptr(g->data + 8);
        const int l = int(v.u32(rect)), t = int(v.u32(rect + 4)), r = int(v.u32(rect + 8)),
                  bo = int(v.u32(rect + 12));
        const img::Image icon_src = p.icon.empty() ? picture : img::read_image(p.icon);
        const img::Image face = img::resize(square(icon_src, p.icon_crop ? p.icon_crop : p.crop), r - l, bo - t);

        CloneHooks h;
        h.rename = [&](const std::string& s) {
            if (s == icon_name) return "Icon_PathType_" + nn + ":gsi";
            if (s.rfind("Icon_PathType_", 0) == 0) return nn + "Icon_" + s.substr(14);
            if (s == "PathIcons:tex") return nn + "Icons:tex";
            return s;
        };
        h.patch_extra = [&](OvlSide, std::uint32_t, std::vector<Bytes>& chunks) {
            // btbl: {0, 0, FlicHeader{format, w, h, mips}} + the mip chain.
            if (chunks.size() != 2 || chunks[0].size() != 24) return;
            const int w = int(rd32(chunks[0], 12)), hh = int(rd32(chunks[0], 16)), mips = int(rd32(chunks[0], 20));
            if (rd32(chunks[0], 8) != 0x13) throw std::runtime_error("PathIcons page is not DXT3");
            Bytes& px = chunks[1];
            img::Image page = flipped_v(img::decode_dxt3(px.data(), w, hh));  // upright
            for (int y = 0; y < face.h; ++y)
                for (int x = 0; x < face.w; ++x) std::copy_n(face.at(x, y), 3, page.at(l + x, t + y));
            std::size_t o = 0;
            img::Image lvl = page;
            for (int m = 0; m < mips; ++m) {
                auto enc = img::encode_dxt3(flipped_v(lvl));
                if (o + enc.size() > px.size()) break;
                std::copy(enc.begin(), enc.end(), px.begin() + std::ptrdiff_t(o));
                o += enc.size();
                lvl = img::half(lvl);
            }
            if (!preview.empty()) img::write_tga(page, preview / (nn + "Icons.tga"));
        };
        auto out = clone_pair(im, h).build();
        write_image_file(out[0], dst / (nn + "Icons.common.ovl"));
        write_image_file(out[1], dst / (nn + "Icons.unique.ovl"));
    }
    return rel;
}

}  // namespace rct3mod
