#include "ovl/extract/DataExtractor.hpp"

#include "ovl/OvlParser.hpp"
#include "ovl/PairView.hpp"
#include "ovl/extract/Layout.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

namespace ovl {

namespace layout {
// One per group file (extractors/src/DataExtractor_<group>.cpp).
void register_paths(Registry&);
void register_items(Registry&);
void register_anims(Registry&);
void register_scenery(Registry&);
void register_people(Registry&);

void register_all() {
    static std::once_flag once;
    std::call_once(once, [] {
        auto& r = Registry::get();
        register_paths(r);
        register_items(r);
        register_anims(r);
        register_scenery(r);
        register_people(r);
    });
}
}  // namespace layout

ExtractResult DataExtractor::extract(const OvlParser& parser, const ExtractContext& ctx) {
    using namespace layout;
    register_all();
    ExtractResult res{};
    const std::filesystem::path common = parser.side(OvlSide::Common).ovlname;
    std::optional<PairView> v;
    try {
        v.emplace(PairView::load(common));
    } catch (const std::exception& e) {
        ctx.log(std::string("data: cannot open pair: ") + e.what());
        return res;
    }
    std::map<std::string, Json> by_tag;
    std::size_t n = 0;
    for (auto& o : v->objects()) {
        if (!Registry::get().find(o.tag)) continue;
        std::vector<Issue> issues;
        Json j = decode_object(*v, o, &issues);
        for (auto& i : issues) ctx.log("data: " + o.name + ": " + i.where + ": " + i.what);
        if (!issues.empty()) ++res.errors;
        auto it = by_tag.find(o.tag);
        if (it == by_tag.end()) it = by_tag.emplace(o.tag, Json::array()).first;
        it->second.push(std::move(j));
        ++n;
    }
    if (by_tag.empty()) return res;
    std::string stem = common.filename().string();
    stem = stem.substr(0, stem.size() - std::string(".common.ovl").size());
    std::filesystem::create_directories(ctx.output_dir);
    auto out_path = ctx.output_dir / (stem + ".data.json");
    if (!ctx.overwrite && std::filesystem::exists(out_path)) {
        ctx.log("skip (exists): " + out_path.filename().string());
        return res;
    }
    Json root = Json::object();
    for (auto& [tag, arr] : by_tag) root.set(tag, std::move(arr));
    std::ofstream(out_path, std::ios::binary) << root.dump() << "\n";
    ++res.files_written;
    ctx.log("wrote " + out_path.filename().string() + " (" + std::to_string(n) + " objects)");
    return res;
}

}  // namespace ovl
