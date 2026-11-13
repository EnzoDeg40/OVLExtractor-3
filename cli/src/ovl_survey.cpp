// Survey the objects of one or more loader tags across a game install, to lay
// out a struct before writing its decoder:
//   - how many objects, in how many pairs, and the size of the data block
//     each one's struct starts (histogram);
//   - for every 4-byte word of the struct: how often it is a relocated
//     pointer, a symbol reference (and to which tags), zero, a plausible
//     float, and a few example values;
//   - for every pointer word: the sizes of the blocks it points into.
//
//   ovl_survey <Assets dir> <tag> [tag ...] [--words N]
#include "ovl/Error.hpp"
#include "ovl/PairView.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ovl;

namespace {

struct Word {
    int n = 0, reloc = 0, zero = 0, floaty = 0, small = 0;
    std::map<std::string, int> symref_tags;
    std::map<std::uint32_t, int> values;
    std::map<std::uint32_t, int> target_sizes;  // block remaining at pointer target
    std::map<std::uint32_t, int> target_off;    // offset of target inside its block
};

struct Tag {
    int n = 0;
    std::set<std::string> pairs;
    std::map<std::uint32_t, int> sizes;
    std::map<std::uint32_t, int> start_off;  // struct offset inside its block
    std::map<std::size_t, int> extra;        // extra-data chunk counts
    std::vector<Word> words;
    std::vector<std::string> examples;
};

bool plausible_float(std::uint32_t v) {
    if (v == 0) return false;
    float f;
    std::memcpy(&f, &v, 4);
    if (!std::isfinite(f)) return false;
    float a = std::fabs(f);
    return a > 1e-4f && a < 1e6f;
}

std::string tag_of(const std::string& s) {
    auto c = s.rfind(':');
    return c == std::string::npos ? "?" : s.substr(c + 1);
}

template <class M>
std::string top(const M& m, std::size_t k, bool hex) {
    std::vector<std::pair<int, typename M::key_type>> v;
    for (auto& [a, b] : m) v.push_back({b, a});
    std::sort(v.rbegin(), v.rend());
    std::string o;
    for (std::size_t i = 0; i < v.size() && i < k; ++i) {
        char b[64];
        if constexpr (std::is_same_v<typename M::key_type, std::string>)
            std::snprintf(b, sizeof b, "%s×%d ", v[i].second.c_str(), v[i].first);
        else if (hex)
            std::snprintf(b, sizeof b, "%X×%d ", unsigned(v[i].second), v[i].first);
        else
            std::snprintf(b, sizeof b, "%u×%d ", unsigned(v[i].second), v[i].first);
        o += b;
    }
    if (v.size() > k) o += "(+" + std::to_string(v.size() - k) + ")";
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: ovl_survey <Assets> <tag> [tag...] [--words N]\n";
        return 2;
    }
    std::set<std::string> want;
    std::size_t max_words = 96;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--words" && i + 1 < argc) max_words = std::stoul(argv[++i]);
        else want.insert(a);
    }
    std::map<std::string, Tag> tags;
    for (auto& e : fs::recursive_directory_iterator(argv[1])) {
        auto p = e.path().string();
        const std::string suf = ".common.ovl";
        if (p.size() < suf.size() || p.compare(p.size() - suf.size(), suf.size(), suf)) continue;
        std::optional<PairView> v;
        try { v.emplace(PairView::load(p)); } catch (...) { continue; }
        const std::string rel = p.substr(p.find("Assets/") + 7);
        for (auto& o : v->objects()) {
            if (!want.count(o.tag)) continue;
            Tag& t = tags[o.tag];
            ++t.n;
            t.pairs.insert(rel);
            ++t.extra[o.extra.size()];
            const std::uint32_t size = v->block_remaining(o.data);
            ++t.sizes[size];
            try { ++t.start_off[o.data - v->block_start(o.data)]; } catch (...) {}
            if (t.examples.size() < 4) t.examples.push_back(o.name + " in " + rel);
            const std::size_t nw = std::min<std::size_t>(size / 4, max_words);
            if (t.words.size() < nw) t.words.resize(nw);
            for (std::size_t w = 0; w < nw; ++w) {
                const std::uint32_t a = o.data + std::uint32_t(4 * w);
                Word& W = t.words[w];
                ++W.n;
                std::uint32_t val = v->u32(a);
                if (auto s = v->symref(a)) { ++W.symref_tags[tag_of(*s)]; continue; }
                if (v->is_reloc(a)) {
                    ++W.reloc;
                    ++W.target_sizes[v->block_remaining(val)];
                    try { ++W.target_off[val - v->block_start(val)]; } catch (...) {}
                    continue;
                }
                if (val == 0) ++W.zero;
                else if (plausible_float(val)) ++W.floaty;
                if (val < 0x10000) ++W.small;
                if (W.values.size() < 4000) ++W.values[val];
            }
        }
    }
    for (auto& [tag, t] : tags) {
        std::cout << "=== " << tag << ": " << t.n << " objects in " << t.pairs.size() << " pairs\n";
        std::cout << "  block size: " << top(t.sizes, 10, false) << "\n";
        std::cout << "  start in block: " << top(t.start_off, 6, false) << "\n";
        std::cout << "  extra chunks: " << top(t.extra, 6, false) << "\n";
        for (auto& e : t.examples) std::cout << "  e.g. " << e << "\n";
        for (std::size_t w = 0; w < t.words.size(); ++w) {
            const Word& W = t.words[w];
            char head[64];
            std::snprintf(head, sizeof head, "  +0x%03zX n=%-5d ", w * 4, W.n);
            std::cout << head;
            if (!W.symref_tags.empty()) std::cout << "SYMREF " << top(W.symref_tags, 6, false) << " ";
            if (W.reloc) std::cout << "PTR×" << W.reloc << " -> size " << top(W.target_sizes, 5, false)
                                   << " at " << top(W.target_off, 3, false) << " ";
            int rest = W.n - W.reloc;
            for (auto& [_, c] : W.symref_tags) rest -= c;
            if (rest > 0) {
                std::cout << "zero " << W.zero << " float " << W.floaty << " small " << W.small
                          << " | " << top(W.values, 6, true);
            }
            std::cout << "\n";
        }
    }
    return 0;
}
