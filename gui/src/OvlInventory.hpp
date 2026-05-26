#pragma once

#include "ovl/OvlTypes.hpp"

#include <QString>
#include <array>
#include <string>
#include <vector>

namespace ovl { class OvlParser; }

namespace ovlgui {

enum class Category {
    Texture,
    Model,
    Sound,
    Atlas,
    Other,
    _Count,
};

const char* category_label(Category c);
Category    categorize(const std::string& loader_tag);

struct ResourceItem {
    std::string  symbol;       // linked-file symbol, e.g. "Dice:ftx"
    std::string  tag;           // loader tag, e.g. "ftx", "shs", "snd"
    std::string  loader_name;   // human-readable loader name
    ovl::OvlSide side = ovl::OvlSide::Common;
    Category     category = Category::Other;
};

// Flat snapshot of what's inside an OVL, suitable for rendering in a tree.
// Built once at parse time; no further parser access required.
struct OvlInventory {
    std::string                                                       common_name;
    std::string                                                       unique_name;
    bool                                                              has_unique = false;
    std::array<std::vector<ResourceItem>, static_cast<std::size_t>(Category::_Count)>
                                                                      by_category;

    std::size_t total_resources() const;
    std::size_t count(Category c) const {
        return by_category[static_cast<std::size_t>(c)].size();
    }

    static OvlInventory build(const ovl::OvlParser& parser);
};

}  // namespace ovlgui
