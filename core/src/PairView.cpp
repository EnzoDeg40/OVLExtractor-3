#include "ovl/PairView.hpp"

#include "ovl/Error.hpp"

#include <cstring>

namespace ovl {

PairView::PairView(const OvlImage& common, const OvlImage& unique) : img_{common, unique} {
    std::uint32_t off = 0;
    for (auto& im : img_)
        for (auto& ch : im.chunks)
            for (auto& b : ch.blocks) {
                blocks_.push_back({off, &b});
                off += std::uint32_t(b.size());
            }
    for (auto& im : img_)
        for (auto r : im.relocations) relocs_.insert(r);

    // Tables: chunk 2 blocks [symbols, loaders, symbol refs] of each side.
    const bool v1 = img_[0].version == 1;
    const std::uint32_t symsize = v1 ? 12 : 16;
    std::uint32_t base = 0;
    std::array<std::array<std::uint32_t, 3>, 2> tab{};
    std::array<std::array<std::uint32_t, 3>, 2> tabsize{};
    for (std::size_t s = 0; s < 2; ++s) {
        for (std::size_t c = 0; c < 9; ++c) {
            for (std::size_t i = 0; i < img_[s].chunks[c].blocks.size(); ++i) {
                if (c == 2 && i < 3) {
                    tab[s][i] = base;
                    tabsize[s][i] = std::uint32_t(img_[s].chunks[c].blocks[i].size());
                }
                base += std::uint32_t(img_[s].chunks[c].blocks[i].size());
            }
        }
    }
    for (std::size_t s = 0; s < 2; ++s) {
        std::map<std::uint32_t, std::string> symname;
        for (std::uint32_t a = 0; a + symsize <= tabsize[s][0]; a += symsize)
            symname[tab[s][0] + a] = cstr(u32(tab[s][0] + a));
        const auto& loaders = img_[s].loaders;
        std::uint32_t nres = 0;
        // v4 and v5 subversion 1 put one 0 word before the extra-data chunks.
        std::size_t extra_pos =
            (img_[s].version == 4 || (img_[s].version == 5 && img_[s].v5_subversion == 1)) ? 4 : 0;
        const Bytes& post = img_[s].post_relocations;
        for (std::uint32_t a = 0; a + 20 <= tabsize[s][1]; a += 20) {
            const std::uint32_t L = tab[s][1] + a;
            Object o;
            o.side = OvlSide(s);
            std::uint32_t type = u32(L);
            o.tag = type < loaders.size() ? loaders[type].tag : "?";
            o.data = u32(L + 4);
            std::uint32_t ex = u32(L + 8);
            if (img_[s].version == 5) ex &= 0xFFFF;
            for (std::uint32_t k = 0; k < ex && extra_pos + 4 <= post.size(); ++k) {
                std::uint32_t sz;
                std::memcpy(&sz, post.data() + extra_pos, 4);
                o.extra.emplace_back(post.begin() + std::ptrdiff_t(extra_pos + 4),
                                     post.begin() + std::ptrdiff_t(extra_pos + 4 + sz));
                extra_pos += 4 + sz;
            }
            nres += u32(L + 16);
            auto it = symname.find(u32(L + 12));
            if (it != symname.end()) o.name = it->second;
            objects_.push_back(std::move(o));
        }
        // The common file only has a symbol-ref table when it has refs.
        if (tabsize[s][2] && (s == 1 || nres > 0)) {
            for (std::uint32_t a = 0; a + symsize <= tabsize[s][2]; a += symsize)
                symrefs_[u32(tab[s][2] + a)] = cstr(u32(tab[s][2] + a + 4));
        }
    }
}

PairView PairView::load(const std::filesystem::path& common_path) {
    auto s = common_path.string();
    const std::string suf = ".common.ovl";
    if (s.size() < suf.size() || s.compare(s.size() - suf.size(), suf.size(), suf))
        throw OvlError("expected a .common.ovl path");
    auto u = s.substr(0, s.size() - suf.size()) + ".unique.ovl";
    return PairView(read_image_file(s), read_image_file(u));
}

const Bytes& PairView::block_at(std::uint32_t a, std::uint32_t& off) const {
    // Last block starting at or before a, skipping empty blocks.
    for (auto it = blocks_.rbegin(); it != blocks_.rend(); ++it)
        if (it->start <= a && a < it->start + it->data->size()) {
            off = a - it->start;
            return *it->data;
        }
    throw OvlError("address outside the pair: " + std::to_string(a));
}

std::uint32_t PairView::block_remaining(std::uint32_t a) const {
    try {
        std::uint32_t off;
        const Bytes& b = block_at(a, off);
        return std::uint32_t(b.size()) - off;
    } catch (const OvlError&) {
        return 0;
    }
}

std::uint32_t PairView::block_start(std::uint32_t a) const {
    std::uint32_t off;
    block_at(a, off);
    return a - off;
}

std::uint32_t PairView::u32(std::uint32_t a) const {
    std::uint32_t off;
    const Bytes& b = block_at(a, off);
    if (off + 4 > b.size()) throw OvlError("u32 crosses a block end");
    std::uint32_t v;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}

std::uint16_t PairView::u16(std::uint32_t a) const {
    std::uint32_t off;
    const Bytes& b = block_at(a, off);
    if (off + 2 > b.size()) throw OvlError("u16 crosses a block end");
    return std::uint16_t(b[off] | b[off + 1] << 8);
}

float PairView::f32(std::uint32_t a) const {
    std::uint32_t v = u32(a);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

Bytes PairView::bytes(std::uint32_t a, std::uint32_t n) const {
    if (n == 0) return {};
    std::uint32_t off;
    const Bytes& b = block_at(a, off);
    if (off + n > b.size()) throw OvlError("byte range crosses a block end");
    return Bytes(b.begin() + off, b.begin() + off + n);
}

std::string PairView::cstr(std::uint32_t a) const {
    std::uint32_t off;
    const Bytes& b = block_at(a, off);
    std::string s;
    while (off < b.size() && b[off]) s += char(b[off++]);
    return s;
}

std::u16string PairView::wstr(std::uint32_t a) const {
    std::uint32_t off;
    const Bytes& b = block_at(a, off);
    std::u16string s;
    while (off + 1 < b.size()) {
        char16_t c = char16_t(b[off] | b[off + 1] << 8);
        if (!c) break;
        s += c;
        off += 2;
    }
    return s;
}

std::optional<std::string> PairView::symref(std::uint32_t slot) const {
    auto it = symrefs_.find(slot);
    if (it == symrefs_.end()) return std::nullopt;
    return it->second;
}

std::optional<std::uint32_t> PairView::ptr(std::uint32_t slot) const {
    if (!is_reloc(slot)) return std::nullopt;
    return u32(slot);
}

const PairView::Object* PairView::find(const std::string& name_tag) const {
    for (auto& o : objects_)
        if (o.name == name_tag) return &o;
    return nullptr;
}

std::vector<std::uint32_t> PairView::slots_in(std::uint32_t a, std::uint32_t n) const {
    std::set<std::uint32_t> out;
    for (auto it = relocs_.lower_bound(a); it != relocs_.end() && *it < a + n; ++it) out.insert(*it);
    for (auto it = symrefs_.lower_bound(a); it != symrefs_.end() && it->first < a + n; ++it)
        out.insert(it->first);
    return {out.begin(), out.end()};
}

}  // namespace ovl
