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
#include "ovl/PairClone.hpp"

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
            auto out = clone_pair(im).build();
            bool same = write_image(out[0]) == in[0] && write_image(out[1]) == in[1];
            if (same) { ++ok[key]; continue; }
            // Was it only the symbol order?
            CloneHooks keep;
            keep.keep_order = true;
            auto out2 = clone_pair(im, keep).build();
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
