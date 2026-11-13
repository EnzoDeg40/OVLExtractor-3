#pragma once

// Small helpers shared by the modkit builders (internal).

#include "rct3mod/Content.hpp"

#include "ovl/ImageCodec.hpp"
#include "ovl/OvlImage.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>

namespace rct3mod {

inline bool ends_with(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

inline std::string strip_suffix(const std::string& s, const std::string& suf) {
    return ends_with(s, suf) ? s.substr(0, s.size() - suf.size()) : s;
}

inline std::string replace_all(std::string s, const std::string& a, const std::string& b) {
    for (std::size_t p = 0; (p = s.find(a, p)) != std::string::npos; p += b.size()) s.replace(p, a.size(), b);
    return s;
}

// "Name:tag" -> "Name".
inline std::string base_name(const std::string& name_tag) { return name_tag.substr(0, name_tag.rfind(':')); }

// Names that end up in symbols and folder names: letters, digits, '_'.
inline void check_name(const std::string& s, const char* what) {
    if (s.empty() || !std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; }))
        throw std::runtime_error(std::string(what) + " \"" + s + "\": use letters, digits and _ only");
}

inline std::uint32_t rd32(const ovl::Bytes& e, std::size_t i) {
    return std::uint32_t(e[i] | e[i + 1] << 8 | e[i + 2] << 16 | std::uint32_t(e[i + 3]) << 24);
}

inline ovl::img::Image flipped_v(const ovl::img::Image& im) {
    ovl::img::Image o = ovl::img::make(im.w, im.h);
    for (int y = 0; y < im.h; ++y) std::copy_n(im.at(0, im.h - 1 - y), std::size_t(im.w) * 4, o.at(0, y));
    return o;
}

// Square crop: the given one, or the largest centred square.
inline ovl::img::Image square(const ovl::img::Image& im, const std::optional<Crop>& c) {
    if (c) {
        if (c->size <= 0) throw std::runtime_error("crop size must be > 0");
        return ovl::img::crop(im, c->x, c->y, c->size, c->size);
    }
    const int s = std::min(im.w, im.h);
    return ovl::img::crop(im, (im.w - s) / 2, (im.h - s) / 2, s, s);
}

// Windows-style path relative to Assets, without the .common.ovl suffix.
inline std::string game_path(const fs::path& rel_common) {
    auto s = strip_suffix(rel_common.generic_string(), ".common.ovl");
    std::replace(s.begin(), s.end(), '/', '\\');
    return s;
}

}  // namespace rct3mod
