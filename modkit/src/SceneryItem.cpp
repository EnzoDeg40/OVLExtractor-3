// Scenery items: a game item (sid + svd + its shs / ftx) re-encoded under a
// new name (docs/LIBOVLNG_WRITER.md §6).
#include "Util.hpp"
#include "rct3mod/Content.hpp"

#include "ovl/OvlImage.hpp"
#include "ovl/PairView.hpp"
#include "ovl/SceneryCodec.hpp"
#include "ovl/TextOvl.hpp"

#include <cmath>
#include <map>
#include <stdexcept>

namespace rct3mod {

using namespace ovl;

fs::path build_scenery_item(const SceneryItem& s, const fs::path& game_assets, const fs::path& out_assets) {
    check_name(s.name, "scenery name");
    if (s.title.empty()) throw std::runtime_error("scenery " + s.name + ": no title");
    if (s.folder.empty() || *s.folder.begin() != "Style")
        throw std::runtime_error("scenery " + s.name + ": folder must be under Style/");
    const std::string& nn = s.name;

    auto V = PairView::load(game_assets / s.visual);
    auto S = PairView::load(game_assets / s.style);
    auto* so = S.find(s.sid + ":sid");
    auto* vo = V.find(s.svd + ":svd");
    if (!so) throw std::runtime_error("no " + s.sid + ":sid in " + s.style.string());
    if (!vo) throw std::runtime_error("no " + s.svd + ":svd in " + s.visual.string());

    // Two pairs, as the game does: <name> holds the visual (ftx, shs, svd);
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
        Shs sh = decode_shs(V, o->data);
        for (auto& m : sh.meshes)
            if (m.ftx && V.find(*m.ftx) && !renamed.count(*m.ftx)) renamed[*m.ftx] = nn + "_" + base_name(*m.ftx) + ":ftx";
        renamed[*l.shs] = nn + "_" + base_name(*l.shs) + ":shs";
        shapes.emplace_back(*l.shs, std::move(sh));
    }
    for (auto& [old, nw] : renamed) {
        if (!ends_with(old, ":ftx")) continue;
        Ftx f = decode_ftx(V, V.find(old)->data);
        if (s.tint) {
            const auto& t = *s.tint;
            for (auto& fr : f.frames)
                for (std::size_t i = 0; i < 256; ++i) {
                    auto* p = &fr.palette[4 * i];  // B, G, R, 0
                    const float lum = (0.114f * p[0] + 0.587f * p[1] + 0.299f * p[2]) / 255.f;
                    for (int c = 0; c < 3; ++c) p[c] = std::uint8_t(std::lround(std::min(255.f, lum * t[std::size_t(2 - c)])));
                }
        }
        encode_ftx(b, base_name(nw), f);
    }
    for (auto& [old, sh] : shapes) {
        for (auto& m : sh.meshes)
            if (m.ftx) m.ftx = rename(*m.ftx);
        encode_shs(b, base_name(rename(old)), sh);
    }
    for (auto& l : svd.lods)
        if (l.shs) l.shs = rename(*l.shs);
    encode_svd(b, nn, svd);

    Sid sid = decode_sid(S, so->data);
    sid.svds = {nn + ":svd"};
    sid.name = nn + "_Name:txt";
    sid.ovl_path = game_path(s.folder / (nn + ".common.ovl"));
    if (s.cost) {
        sid.set_cost(*s.cost);
        sid.set_removal_cost(-sid.cost() * 9 / 10);
    }
    encode_sid(sb, nn, sid);
    add_texts(sb, {{nn + "_Name", utf16(s.title)}});
    // A Style file tells the menus which theme its items belong to (0 Vanilla,
    // 1 Wild West, 2 Spooky, 3 Space, 4 Adventure, 5 Atlantis, 6 Island
    // Paradise, 7 Jungle, 8 Prehistoric); without it the items are not listed.
    sb.add_data_symbol(OvlSide::Common, "StyleIndex:int", std::uint32_t(s.theme));

    // File references (the OVLs the pair depends on: icons, shared textures…)
    // of both source pairs, re-anchored to the new folder. The game writes them
    // with backslashes: "..\\..\\SceneryIcons".
    auto add_refs = [&](OvlBuilder& dst, const PairView& src, const fs::path& src_rel) {
        const fs::path dir = (game_assets / src_rel).parent_path();
        for (auto& ref : src.references(OvlSide::Unique)) {
            std::string r = ref;
            std::replace(r.begin(), r.end(), '\\', '/');
            auto target = (dir / r).lexically_normal();
            auto rel = target.lexically_relative(game_assets / s.folder).generic_string();
            std::replace(rel.begin(), rel.end(), '/', '\\');
            dst.add_file_reference(OvlSide::Unique, rel);
        }
    };
    add_refs(sb, S, s.style);
    add_refs(b, V, s.visual);

    fs::create_directories(out_assets / s.folder);
    auto save = [&](OvlBuilder& bb, const std::string& stem) {
        auto im = bb.build();
        const auto outbase = (out_assets / s.folder / stem).string();
        write_image_file(im[0], outbase + ".common.ovl");
        write_image_file(im[1], outbase + ".unique.ovl");
    };
    save(b, nn);
    save(sb, "Style");
    return s.folder;
}

}  // namespace rct3mod
