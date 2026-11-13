#include "ovl/OvlBuilder.hpp"

#include "ovl/Error.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <tuple>

namespace ovl {

namespace {

void put32(Bytes& o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back(std::uint8_t(v >> (8 * i)));
}
void put16(Bytes& o, std::uint16_t v) {
    o.push_back(std::uint8_t(v));
    o.push_back(std::uint8_t(v >> 8));
}
void poke32(Bytes& b, std::uint32_t off, std::uint32_t v) {
    if (off + 4 > b.size()) throw OvlError("pointer slot outside its block");
    for (int i = 0; i < 4; ++i) b[off + i] = std::uint8_t(v >> (8 * i));
}
std::string tag_of(const std::string& name_tag) {
    auto p = name_tag.rfind(':');
    return p == std::string::npos ? std::string() : name_tag.substr(p + 1);
}
std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

OvlBuilder::OvlBuilder(std::uint32_t version) : version_(version) {
    if (version != 1 && version != 4 && version != 5)
        throw OvlError("OvlBuilder: version must be 1, 4 or 5");
}

std::uint32_t OvlBuilder::add_loader_type(const ImageLoader& l) {
    loader_types_.push_back(l);
    return std::uint32_t(loader_types_.size() - 1);
}

void OvlBuilder::add_file_reference(OvlSide side, const std::string& ref) {
    side_[idx(side)].references.push_back(ref);
}

BlockRef OvlBuilder::add_block(OvlSide side, std::uint32_t chunk, Bytes data) {
    if (chunk > 8) throw OvlError("chunk index out of range");
    auto& v = side_[idx(side)].blocks[chunk];
    v.push_back(std::move(data));
    return BlockRef{side, chunk, std::uint32_t(v.size() - 1)};
}

Bytes& OvlBuilder::block(const BlockRef& b) {
    auto& v = side_[idx(b.side)].blocks.at(b.chunk);
    if (b.index >= v.size()) throw OvlError("bad block reference");
    return v[b.index];
}

void OvlBuilder::set_pointer(const Loc& slot, const Loc& target) {
    pointers_[{slot.block, slot.offset}] = target;
}

void OvlBuilder::set_string_pointer(const Loc& slot, const std::string& s) {
    add_string(s);
    spointers_[{slot.block, slot.offset}] = s;
}

void OvlBuilder::add_string(const std::string& s) { strings_.push_back(s); }

void OvlBuilder::add_symbol(OvlSide side, const std::string& name_tag, const Loc& data,
                            std::optional<std::uint16_t> unknown) {
    side_[idx(side)].symbols.push_back(Symbol{name_tag, data, 0, unknown});
}

void OvlBuilder::add_data_symbol(OvlSide side, const std::string& name_tag, std::uint32_t value,
                                 std::optional<std::uint16_t> unknown) {
    side_[idx(side)].symbols.push_back(Symbol{name_tag, std::nullopt, value, unknown});
}

std::uint32_t OvlBuilder::add_loader_ref(OvlSide side, LoaderRefSpec spec) {
    if (spec.loader_type >= loader_types_.size()) throw OvlError("unknown loader type");
    auto& v = side_[idx(side)].loaders;
    v.push_back(std::move(spec));
    return std::uint32_t(v.size() - 1);
}

void OvlBuilder::add_symbol_ref(OvlSide side, std::uint32_t loader_ref, const Loc& slot,
                                const std::string& name_tag) {
    auto& s = side_[idx(side)];
    if (loader_ref >= s.loaders.size()) throw OvlError("symbol ref on unknown loader");
    s.symrefs.push_back(SymRef{loader_ref, slot, name_tag});
}

std::array<OvlImage, 2> OvlBuilder::build() const {
    const bool v1 = version_ == 1;
    const std::uint32_t symsize = v1 ? 12 : 16;

    // ---- 1. string table (common, chunk 0, block 0) -----------------------
    std::vector<std::string> strs;
    {
        std::vector<std::string> all = strings_;
        for (auto& sd : side_) {
            for (auto& s : sd.symbols) all.push_back(s.name);
            for (auto& r : sd.symrefs) all.push_back(r.name);
        }
        if (string_order_ == StringOrder::Alphabetical) {
            std::sort(all.begin(), all.end());
            all.erase(std::unique(all.begin(), all.end()), all.end());
            strs = std::move(all);
        } else {
            std::set<std::string> seen;
            for (auto& s : all)
                if (seen.insert(s).second) strs.push_back(s);
        }
    }
    Bytes strtab;
    std::map<std::string, std::uint32_t> stroff;
    for (auto& s : strs) {
        stroff[s] = std::uint32_t(strtab.size());
        strtab.insert(strtab.end(), s.begin(), s.end());
        strtab.push_back(0);
    }

    // ---- 2. per-side order of symbols and symbol refs ----------------------
    std::map<std::string, std::uint32_t> type_of_tag;
    for (std::uint32_t i = 0; i < loader_types_.size(); ++i)
        type_of_tag.emplace(loader_types_[i].tag, i);

    std::array<std::vector<const Symbol*>, 2> syms;
    std::array<std::vector<const SymRef*>, 2> refs;
    for (std::size_t s = 0; s < 2; ++s) {
        for (auto& sy : side_[s].symbols) syms[s].push_back(&sy);
        if (symbol_order_ == SymbolOrder::Default) {
            auto key = [&](const Symbol* x) {
                auto h = symbol_hash(x->name);
                if (version_ == 1) return std::make_tuple(0u, lower(x->name), 0u, 0u);
                if (version_ == 4) return std::make_tuple(0u, std::string(), 0u, h);
                // Loader numbers follow the alphabetical order of the loader
                // tags (as in libOVLng), so "by loader, then hash" is "by tag,
                // then hash"; symbols whose tag has no loader come last.
                const bool data = type_of_tag.find(tag_of(x->name)) == type_of_tag.end();
                return std::make_tuple(data ? 1u : 0u, tag_of(x->name), 0u, h);
            };
            std::stable_sort(syms[s].begin(), syms[s].end(),
                             [&](auto* a, auto* b) { return key(a) < key(b); });
        }
        // Symbol refs: grouped by loader, in loader order (libOVLng), unless
        // the caller's order is kept.
        if (symref_order_ == SymRefOrder::AsAdded) {
            for (auto& r : side_[s].symrefs) refs[s].push_back(&r);
        } else {
            for (std::uint32_t l = 0; l < side_[s].loaders.size(); ++l)
                for (auto& r : side_[s].symrefs)
                    if (r.loader == l) refs[s].push_back(&r);
        }
    }

    // ---- 3. layout: one address space, common chunks 0..8 then unique -----
    std::array<std::array<std::vector<Bytes>, 9>, 2> blocks;   // final, per side/chunk
    std::array<std::array<std::uint32_t, 9>, 2> prefix{};      // builder blocks before caller's
    std::array<bool, 2> has_symref_tab{};
    for (std::size_t s = 0; s < 2; ++s) {
        if (s == 0) { blocks[s][0].push_back(strtab); prefix[s][0] = 1; }
        blocks[s][2].push_back(Bytes(syms[s].size() * symsize));
        blocks[s][2].push_back(Bytes(side_[s].loaders.size() * 20));
        has_symref_tab[s] = s == 1 || !refs[s].empty() || common_symref_always_;
        if (has_symref_tab[s]) blocks[s][2].push_back(Bytes(refs[s].size() * symsize));
        prefix[s][2] = has_symref_tab[s] ? 3 : 2;
        for (std::size_t c = 0; c < 9; ++c)
            for (auto& b : side_[s].blocks[c]) blocks[s][c].push_back(b);
    }
    std::array<std::array<std::vector<std::uint32_t>, 9>, 2> start;
    std::uint32_t off = 0;
    for (std::size_t s = 0; s < 2; ++s)
        for (std::size_t c = 0; c < 9; ++c)
            for (auto& b : blocks[s][c]) {
                start[s][c].push_back(off);
                off += std::uint32_t(b.size());
            }

    auto final_index = [&](const BlockRef& b) {
        auto s = idx(b.side);
        auto i = b.index + prefix[s][b.chunk];
        if (i >= blocks[s][b.chunk].size()) throw OvlError("bad block reference");
        return i;
    };
    auto gaddr = [&](const Loc& l) {
        return start[idx(l.block.side)][l.block.chunk][final_index(l.block)] + l.offset;
    };
    auto str_addr = [&](const std::string& s) {
        auto it = stroff.find(s);
        if (it == stroff.end()) throw OvlError("string not in table: " + s);
        return start[0][0][0] + it->second;
    };

    std::array<std::vector<std::uint32_t>, 2> relocs;
    // Pointer fields in the caller's blocks.
    auto patch = [&](const BlockRef& b, std::uint32_t at, std::uint32_t value) {
        auto s = idx(b.side);
        auto i = final_index(b);
        poke32(blocks[s][b.chunk][i], at, value);
        relocs[s].push_back(start[s][b.chunk][i] + at);
    };
    for (auto& [slot, target] : pointers_) patch(slot.first, slot.second, gaddr(target));
    for (auto& [slot, s] : spointers_) patch(slot.first, slot.second, str_addr(s));

    // ---- 4. tables ---------------------------------------------------------
    for (std::size_t s = 0; s < 2; ++s) {
        const auto sym0 = start[s][2][0], ldr0 = start[s][2][1];
        auto& symtab = blocks[s][2][0];
        auto& ldrtab = blocks[s][2][1];
        std::map<std::string, std::uint32_t> symaddr;
        for (std::uint32_t k = 0; k < syms[s].size(); ++k) {
            const auto& y = *syms[s][k];
            const std::uint32_t at = k * symsize;
            symaddr[y.name] = sym0 + at;
            poke32(symtab, at, str_addr(y.name));
            relocs[s].push_back(sym0 + at);
            if (y.data) {
                poke32(symtab, at + 4, gaddr(*y.data));
                relocs[s].push_back(sym0 + at + 4);
            } else {
                poke32(symtab, at + 4, y.value);
            }
            const std::uint32_t isptr = y.data ? 1 : 0;
            if (version_ == 5) {
                std::uint16_t unk = y.unknown.value_or(0xFFFF);
                symtab[at + 8] = std::uint8_t(isptr);
                symtab[at + 9] = 0;
                symtab[at + 10] = std::uint8_t(unk);
                symtab[at + 11] = std::uint8_t(unk >> 8);
            } else {
                poke32(symtab, at + 8, isptr);
            }
            if (!v1) poke32(symtab, at + 12, symbol_hash(y.name));
        }
        std::map<const SymRef*, std::uint32_t> row;
        for (std::uint32_t k = 0; k < refs[s].size(); ++k) row[refs[s][k]] = k;
        for (std::uint32_t k = 0; k < side_[s].loaders.size(); ++k) {
            const auto& l = side_[s].loaders[k];
            const std::uint32_t at = k * 20;
            std::uint32_t nres = 0;
            for (auto* x : refs[s]) nres += x->loader == k;
            poke32(ldrtab, at, l.loader_type);
            if (l.data) {
                poke32(ldrtab, at + 4, gaddr(*l.data));
                relocs[s].push_back(ldr0 + at + 4);
            }
            poke32(ldrtab, at + 8,
                   std::uint32_t(l.extra_chunks.size()) | std::uint32_t(l.extra_high) << 16);
            if (!l.symbol.empty()) {
                auto it = symaddr.find(l.symbol);
                if (it == symaddr.end()) throw OvlError("loader symbol not defined: " + l.symbol);
                poke32(ldrtab, at + 12, it->second);
                relocs[s].push_back(ldr0 + at + 12);
            }
            poke32(ldrtab, at + 16, nres);
            for (auto* x : refs[s]) {
                if (x->loader != k) continue;
                auto& rt = blocks[s][2][2];
                const std::uint32_t rat = row.at(x) * symsize, g = start[s][2][2] + rat;
                poke32(rt, rat, gaddr(x->slot));
                poke32(rt, rat + 4, str_addr(x->name));
                poke32(rt, rat + 8, ldr0 + at);
                if (!v1) poke32(rt, rat + 12, symbol_hash(x->name));
                relocs[s].push_back(g);
                relocs[s].push_back(g + 4);
                relocs[s].push_back(g + 8);
            }
        }
    }

    // ---- 5. images -----------------------------------------------------------
    std::array<OvlImage, 2> out;
    for (std::size_t s = 0; s < 2; ++s) {
        auto& im = out[s];
        const auto& sd = side_[s];
        im.version = version_;
        im.headerversion = v1 ? 0 : 2;
        im.references = sd.references;
        im.loaders = loader_types_;
        if (version_ == 5) {
            // One {loader type, symbols of that tag in this file} pair per
            // loader type, in the alphabetical order of the tags.
            std::vector<std::uint32_t> order(loader_types_.size());
            for (std::uint32_t t = 0; t < order.size(); ++t) order[t] = t;
            std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
                return loader_types_[a].tag < loader_types_[b].tag;
            });
            for (auto t : order) {
                std::uint32_t n = 0;
                for (auto& y : sd.symbols) n += y.data && tag_of(y.name) == loader_types_[t].tag;
                im.v5_loader_info.push_back({t, n});
            }
        }
        for (std::size_t c = 0; c < 9; ++c) {
            im.chunks[c].blocks = std::move(blocks[s][c]);
            im.chunks[c].count2 = v1 ? 0 : std::uint32_t(im.chunks[c].blocks.size());
        }
        if (version_ == 5) {
            Bytes x;
            std::uint32_t n = 0;
            for (std::uint32_t k = 0; k < sd.loaders.size(); ++k)
                for (auto& e : sd.loaders[k].v5_extra) {
                    put32(x, k);
                    put16(x, e.a);
                    put16(x, e.b);
                    put32(x, e.c);
                    put16(x, e.d);
                    ++n;
                }
            put32(im.v5_extra_struct, n);
            if (n) {
                im.v5_extra_struct.insert(im.v5_extra_struct.end(), x.begin(), x.end());
                put32(im.v5_extra_struct, 0);
            }
        }
        std::sort(relocs[s].begin(), relocs[s].end());
        im.relocations = relocs[s];
        if (version_ == 4) put32(im.post_relocations, 0);
        for (auto& l : sd.loaders)
            for (auto& e : l.extra_chunks) {
                put32(im.post_relocations, std::uint32_t(e.size()));
                im.post_relocations.insert(im.post_relocations.end(), e.begin(), e.end());
            }
        im.post_relocations.insert(im.post_relocations.end(), sd.trailer.begin(), sd.trailer.end());
    }
    return out;
}

}  // namespace ovl
