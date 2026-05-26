#include "OvlInventory.hpp"

#include "ovl/OvlParser.hpp"

namespace ovlgui {

const char* category_label(Category c) {
    switch (c) {
        case Category::Texture: return "Textures";
        case Category::Model:   return "3D Models";
        case Category::Sound:   return "Sounds";
        case Category::Atlas:   return "Atlas sprites";
        case Category::Other:   return "Other";
        case Category::_Count:  break;
    }
    return "?";
}

Category categorize(const std::string& tag) {
    if (tag == "ftx" || tag == "tex" || tag == "fts" || tag == "ftt") return Category::Texture;
    if (tag == "shs" || tag == "mms" || tag == "bsh") return Category::Model;
    if (tag == "snd")                                 return Category::Sound;
    if (tag == "gsi")                                 return Category::Atlas;
    return Category::Other;
}

std::size_t OvlInventory::total_resources() const {
    std::size_t n = 0;
    for (const auto& v : by_category) n += v.size();
    return n;
}

OvlInventory OvlInventory::build(const ovl::OvlParser& parser) {
    OvlInventory inv;
    inv.has_unique = parser.has_unique();
    inv.common_name = parser.side(ovl::OvlSide::Common).ovlname;
    if (inv.has_unique) inv.unique_name = parser.side(ovl::OvlSide::Unique).ovlname;

    for (std::size_t s = 0; s < (inv.has_unique ? 2u : 1u); ++s) {
        auto side = static_cast<ovl::OvlSide>(s);
        const auto& d = parser.side(side);
        for (const auto& lf : d.linkedfiles) {
            ResourceItem r;
            r.symbol      = parser.string_from_offset(lf.symbolresolve.stringpointer);
            ovl::Loader l = parser.loader_by_id(lf.loaderreference.loadernumber, side);
            r.tag         = l.tag;
            r.loader_name = l.name.empty() ? l.loader : l.name;
            r.side        = side;
            r.category    = categorize(r.tag);
            inv.by_category[static_cast<std::size_t>(r.category)].push_back(std::move(r));
        }
    }
    return inv;
}

}  // namespace ovlgui
