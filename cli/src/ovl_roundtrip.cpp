// Re-encode every OVL under a directory and compare with the original bytes.
//
//   ovl_roundtrip <dir>            image level: read_image -> write_image
//   ovl_roundtrip --rebuild <dir>  builder level: decompose each pair into
//                                  blocks + pointers, symbols, loaders and
//                                  symbol refs, rebuild it with OvlBuilder
//
// The rebuild keeps only what a type writer would provide: block payloads and
// which fields are pointers (from the relocation table), the string order and
// the loader-reference order (the game does not sort them). The builder must
// regenerate everything else — tables, layout, relocations, hashes, symbol
// order, v5 tables — and land on the same bytes.
#include "ovl/Error.hpp"
#include "ovl/OvlBuilder.hpp"
#include "ovl/OvlImage.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>

namespace fs = std::filesystem;
using namespace ovl;

namespace {

Bytes slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::uint32_t rd32(const Bytes& b, std::size_t o) {
    if (o + 4 > b.size()) throw OvlError("read past block");
    return std::uint32_t(b[o]) | std::uint32_t(b[o + 1]) << 8 | std::uint32_t(b[o + 2]) << 16 |
           std::uint32_t(b[o + 3]) << 24;
}
std::uint16_t rd16(const Bytes& b, std::size_t o) {
    return std::uint16_t(b[o] | b[o + 1] << 8);
}

struct Placed {
    std::size_t   side, chunk, index;  // final index in the image
    std::uint32_t start, size;
};

// Fails with a short reason when a pair uses something the builder can't
// express; the reason is the histogram key.
std::array<OvlImage, 2> rebuild(const std::array<OvlImage, 2>& im, bool sort_symbols) {
    const auto v = im[0].version;
    if (im[1].version != v) throw OvlError("sides differ in version");
    if (v == 5 && (im[0].v5_subversion || im[1].v5_subversion)) throw OvlError("v5.1");
    const std::uint32_t symsize = v == 1 ? 12 : 16;

    std::vector<Placed> placed;
    std::uint32_t off = 0;
    for (std::size_t s = 0; s < 2; ++s)
        for (std::size_t c = 0; c < 9; ++c)
            for (std::size_t i = 0; i < im[s].chunks[c].blocks.size(); ++i) {
                auto sz = std::uint32_t(im[s].chunks[c].blocks[i].size());
                placed.push_back({s, c, i, off, sz});
                off += sz;
            }
    auto blk = [&](std::size_t s, std::size_t c, std::size_t i) -> const Placed& {
        for (auto& p : placed)
            if (p.side == s && p.chunk == c && p.index == i) return p;
        throw OvlError("missing table block");
    };
    auto bytes_of = [&](const Placed& p) -> const Bytes& {
        return im[p.side].chunks[p.chunk].blocks[p.index];
    };

    if (im[0].chunks[0].blocks.empty()) throw OvlError("no string table block");
    if (im[0].chunks[2].blocks.size() < 2 || im[1].chunks[2].blocks.size() < 3)
        throw OvlError("missing table blocks");
    const Placed& strp = blk(0, 0, 0);
    const Bytes& strtab = bytes_of(strp);
    std::map<std::uint32_t, std::string> strings;  // global address -> string
    std::vector<std::string> string_order;
    for (std::uint32_t p = 0; p < strtab.size();) {
        auto e = std::find(strtab.begin() + p, strtab.end(), 0);
        std::string s(strtab.begin() + p, e);
        strings[strp.start + p] = s;
        string_order.push_back(s);
        p = std::uint32_t(e - strtab.begin()) + 1;
    }
    auto str_at = [&](std::uint32_t a) {
        auto it = strings.find(a);
        if (it == strings.end()) throw OvlError("pointer into the middle of a string");
        return it->second;
    };

    // Table blocks and the user blocks after them.
    std::array<std::size_t, 2> prefix2{};
    std::array<std::uint32_t, 2> nres{};
    for (std::size_t s = 0; s < 2; ++s) {
        const Bytes& lt = bytes_of(blk(s, 2, 1));
        for (std::size_t k = 0; k + 20 <= lt.size(); k += 20) nres[s] += rd32(lt, k + 16);
    }
    bool common_symref_always = false;
    prefix2[1] = 3;
    if (nres[0] > 0) prefix2[0] = 3;
    else if (im[0].chunks[2].blocks.size() > 2 && im[0].chunks[2].blocks[2].empty()) {
        prefix2[0] = 3;
        common_symref_always = true;
    } else prefix2[0] = 2;
    auto is_table = [&](const Placed& p) {
        return (p.side == 0 && p.chunk == 0 && p.index == 0) ||
               (p.chunk == 2 && p.index < prefix2[p.side]);
    };

    std::set<std::uint32_t> relocs;
    for (auto& i : im)
        for (auto r : i.relocations) relocs.insert(r);

    OvlBuilder b(v);
    b.set_string_order(OvlBuilder::StringOrder::AsAdded);
    if (!sort_symbols) {
        b.set_symbol_order(OvlBuilder::SymbolOrder::AsAdded);
        b.set_symref_order(OvlBuilder::SymRefOrder::AsAdded);
    }
    b.set_common_symref_table_always(common_symref_always);
    for (auto& s : string_order) b.add_string(s);
    if (im[0].loaders.size() != im[1].loaders.size()) throw OvlError("loader lists differ");
    for (auto& l : im[0].loaders) b.add_loader_type(l);
    for (std::size_t s = 0; s < 2; ++s)
        for (auto& r : im[s].references) b.add_file_reference(OvlSide(s), r);

    std::map<std::size_t, BlockRef> user;  // index in `placed` -> builder block
    for (std::size_t k = 0; k < placed.size(); ++k) {
        auto& p = placed[k];
        if (is_table(p)) continue;
        user[k] = b.add_block(OvlSide(p.side), std::uint32_t(p.chunk), bytes_of(p));
    }
    auto locate = [&](std::uint32_t a, bool slot) -> Loc {
        // Prefer the block that contains `a`; a target may also sit exactly at
        // the end of a block.
        for (int pass = 0; pass < 2; ++pass)
            for (std::size_t k = 0; k < placed.size(); ++k) {
                auto& p = placed[k];
                bool in = pass == 0 ? (a >= p.start && a < p.start + p.size)
                                    : (!slot && p.size && a == p.start + p.size);
                if (!in) continue;
                if (is_table(p)) throw OvlError(slot ? "slot in table" : "pointer into a table");
                return Loc{user.at(k), a - p.start};
            }
        throw OvlError("pointer outside the file");
    };

    // Pointer fields of user blocks.
    std::set<std::uint32_t> table_slots;
    for (std::size_t s = 0; s < 2; ++s) {
        auto& sp = blk(s, 2, 0);
        for (std::uint32_t a = 0; a < sp.size; a += symsize) {
            table_slots.insert(sp.start + a);
            table_slots.insert(sp.start + a + 4);
        }
        auto& lp = blk(s, 2, 1);
        for (std::uint32_t a = 0; a < lp.size; a += 20) {
            table_slots.insert(lp.start + a + 4);
            table_slots.insert(lp.start + a + 12);
        }
        if (prefix2[s] == 3) {
            auto& rp = blk(s, 2, 2);
            for (std::uint32_t a = 0; a < rp.size; a += symsize)
                for (std::uint32_t f = 0; f < 12; f += 4) table_slots.insert(rp.start + a + f);
        }
    }
    for (auto r : relocs) {
        if (table_slots.count(r)) continue;
        Loc slot = locate(r, true);
        std::size_t pk = 0;
        for (std::size_t k = 0; k < placed.size(); ++k)
            if (r >= placed[k].start && r < placed[k].start + placed[k].size) pk = k;
        auto& p = placed[pk];
        std::uint32_t t = rd32(bytes_of(p), r - p.start);
        if (t >= strp.start && t < strp.start + strp.size) b.set_string_pointer(slot, str_at(t));
        else b.set_pointer(slot, locate(t, false));
    }

    // Symbols, loader references, symbol references.
    for (std::size_t s = 0; s < 2; ++s) {
        auto side = OvlSide(s);
        auto& sp = blk(s, 2, 0);
        auto& lp = blk(s, 2, 1);
        const Bytes& st = bytes_of(sp);
        const Bytes& lt = bytes_of(lp);
        std::map<std::uint32_t, std::string> symname;
        for (std::uint32_t a = 0; a + symsize <= st.size(); a += symsize) {
            auto name = str_at(rd32(st, a));
            symname[sp.start + a] = name;
            std::optional<std::uint16_t> unk;
            std::uint32_t isptr = v == 5 ? rd16(st, a + 8) : rd32(st, a + 8);
            if (v == 5) unk = rd16(st, a + 10);
            bool reloc = relocs.count(sp.start + a + 4) > 0;
            if (reloc != (isptr != 0)) throw OvlError("isptr != relocated");
            if (v != 1 && rd32(st, a + 12) != symbol_hash(name)) throw OvlError("hash mismatch");
            if (reloc) b.add_symbol(side, name, locate(rd32(st, a + 4), false), unk);
            else b.add_data_symbol(side, name, rd32(st, a + 4), unk);
        }
        // v5 extra-data records, by loader index.
        std::map<std::uint32_t, std::vector<V5ExtraInfo>> v5x;
        if (v == 5) {
            const Bytes& x = im[s].v5_extra_struct;
            std::uint32_t n = rd32(x, 0);
            for (std::uint32_t k = 0; k < n; ++k) {
                std::size_t o = 4 + k * 14;
                V5ExtraInfo e{rd16(x, o + 4), rd16(x, o + 6), rd32(x, o + 8), rd16(x, o + 12)};
                v5x[rd32(x, o)].push_back(e);
            }
        }
        // Extra-data stream.
        const Bytes& post = im[s].post_relocations;
        std::size_t pp = v == 4 ? 4 : 0;
        if (v == 4 && rd32(post, 0) != 0) throw OvlError("v4 post word != 0");
        for (std::uint32_t k = 0; k * 20 < lt.size(); ++k) {
            std::uint32_t a = k * 20;
            OvlBuilder::LoaderRefSpec spec;
            spec.loader_type = rd32(lt, a);
            if (relocs.count(lp.start + a + 4)) spec.data = locate(rd32(lt, a + 4), false);
            else if (rd32(lt, a + 4)) throw OvlError("loader data not relocated");
            std::uint32_t ex = rd32(lt, a + 8);
            std::uint32_t nchunks = v == 5 ? (ex & 0xFFFF) : ex;
            spec.extra_high = v == 5 ? std::uint16_t(ex >> 16) : 0;
            if (relocs.count(lp.start + a + 12)) {
                auto it = symname.find(rd32(lt, a + 12));
                if (it == symname.end()) throw OvlError("loader symbol not in table");
                spec.symbol = it->second;
            } else if (rd32(lt, a + 12)) throw OvlError("loader symbol not relocated");
            for (std::uint32_t c = 0; c < nchunks; ++c) {
                std::uint32_t sz = rd32(post, pp);
                spec.extra_chunks.emplace_back(post.begin() + std::ptrdiff_t(pp + 4),
                                               post.begin() + std::ptrdiff_t(pp + 4 + sz));
                pp += 4 + sz;
            }
            spec.v5_extra = v5x[k];
            b.add_loader_ref(side, std::move(spec));
        }
        b.set_trailer(side, Bytes(post.begin() + std::ptrdiff_t(pp), post.end()));
        if (prefix2[s] == 3) {
            auto& rp = blk(s, 2, 2);
            const Bytes& rt = bytes_of(rp);
            for (std::uint32_t a = 0; a + symsize <= rt.size(); a += symsize) {
                std::uint32_t l = rd32(rt, a + 8);
                if (l < lp.start || (l - lp.start) % 20) throw OvlError("symref loader pointer");
                b.add_symbol_ref(side, (l - lp.start) / 20, locate(rd32(rt, a), true),
                                 str_at(rd32(rt, a + 4)));
            }
        }
    }
    return b.build();
}

// First differing part, as a histogram key.
std::string diff(const OvlImage& a, const OvlImage& b) {
    if (a.loaders.size() != b.loaders.size()) return "loaders";
    if (a.v5_loader_info != b.v5_loader_info) return "v5 loader info";
    for (std::size_t c = 0; c < 9; ++c) {
        if (a.chunks[c].blocks.size() != b.chunks[c].blocks.size())
            return "chunk " + std::to_string(c) + " block count";
        if (a.chunks[c].count2 != b.chunks[c].count2) return "count2";
        for (std::size_t i = 0; i < a.chunks[c].blocks.size(); ++i)
            if (a.chunks[c].blocks[i] != b.chunks[c].blocks[i])
                return "chunk " + std::to_string(c) + " block " +
                       (c == 2 && i < 3 ? std::to_string(i) + " (table)" : "data");
    }
    if (a.v5_extra_struct != b.v5_extra_struct) return "v5 extra struct";
    if (a.v5_after_blocks != b.v5_after_blocks) return "v5 after blocks";
    if (a.v4_unknown != b.v4_unknown) return "v4 unknown";
    if (a.relocations != b.relocations) return "relocations";
    if (a.post_relocations != b.post_relocations) return "post";
    return "header";
}

}  // namespace

int main(int argc, char** argv) {
    bool rebuild_mode = argc > 2 && std::string(argv[1]) == "--rebuild";
    if (argc < 2 || (std::string(argv[1]) == "--rebuild" && argc < 3)) {
        std::cerr << "usage: ovl_roundtrip [--rebuild] <dir>\n";
        return 2;
    }
    fs::path dir = argv[rebuild_mode ? 2 : 1];
    std::map<std::string, int> ok, bad;
    int shown = 0;
    for (auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file() || e.path().extension() != ".ovl") continue;
        const std::string name = e.path().filename().string();
        if (!rebuild_mode) {
            Bytes in = slurp(e.path());
            std::string key;
            try {
                auto im = read_image(in);
                key = "v" + std::to_string(im.version) +
                      (im.version == 5 ? "." + std::to_string(im.v5_subversion) : "");
                if (write_image(im) == in) { ++ok[key]; continue; }
            } catch (const std::exception& ex) {
                key = std::string("error: ") + ex.what();
            }
            ++bad[key];
            if (shown++ < 5) std::cerr << "MISMATCH " << key << " " << e.path() << "\n";
            continue;
        }
        const std::string suffix = ".common.ovl";
        if (name.size() < suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix))
            continue;
        fs::path up = e.path().parent_path() / (name.substr(0, name.size() - suffix.size()) + ".unique.ovl");
        if (!fs::exists(up)) continue;
        std::array<Bytes, 2> in{slurp(e.path()), slurp(up)};
        std::string key;
        try {
            std::array<OvlImage, 2> im{read_image(in[0]), read_image(in[1])};
            key = "v" + std::to_string(im[0].version);
            auto out = rebuild(im, true);
            bool same = write_image(out[0]) == in[0] && write_image(out[1]) == in[1];
            if (same) { ++ok[key]; continue; }
            // Was it only the symbol order?
            auto out2 = rebuild(im, false);
            if (write_image(out2[0]) == in[0] && write_image(out2[1]) == in[1]) {
                key += " symbol / symref order only";
            } else {
                for (std::size_t s = 0; s < 2; ++s)
                    if (write_image(out2[s]) != in[s]) {
                        key += std::string(s ? " unique: " : " common: ") + diff(out2[s], im[s]);
                        break;
                    }
            }
        } catch (const std::exception& ex) {
            key += std::string(" error: ") + ex.what();
        }
        ++bad[key];
        if (shown++ < 8 || std::getenv("OVL_VERBOSE")) std::cerr << "MISMATCH " << key << " " << e.path() << "\n";
    }
    int nbad = 0, nok = 0;
    for (auto& [k, n] : ok) { std::cout << "ok   " << k << ": " << n << "\n"; nok += n; }
    for (auto& [k, n] : bad) { std::cout << "FAIL " << k << ": " << n << "\n"; nbad += n; }
    std::cout << nok << " ok, " << nbad << " failed\n";
    return nbad ? 1 : 0;
}
