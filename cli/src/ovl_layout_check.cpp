// Check the declarative layouts (extractors/src/DataExtractor_*.cpp) against
// every object of the game: each struct's bytes all claimed by a field, each
// pointer / symbol-reference slot consumed, each Zero field zero.
//
//   ovl_layout_check <Assets dir> [tag ...] [--show N] [--dump tag]
//
// Prints, per tag, objects ok / with issues, the most frequent issues with an
// example, and how many fields are still named unk*. --dump writes the JSON of
// the first object of a tag to stdout.
#include "ovl/PairView.hpp"
#include "ovl/extract/Layout.hpp"

#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>

namespace fs = std::filesystem;
using namespace ovl;
using namespace ovl::layout;

namespace {

// Fields whose key starts with "unk", counted once per key path.
void count_unk(const Json& j, const std::string& path, std::set<std::string>& out) {
    if (j.is_object()) {
        for (auto& [k, v] : j.obj()) {
            const std::string p = path.empty() ? k : path + "." + k;
            if (k.rfind("unk", 0) == 0) out.insert(p);
            count_unk(v, p, out);
        }
    } else if (j.is_array()) {
        for (auto& e : j.arr()) count_unk(e, path + "[]", out);
    }
}

std::string strip_index(std::string s) {
    // "+0x1C[3]" -> "+0x1C[]" so the same issue groups across elements.
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '[') {
            auto j = s.find(']', i);
            if (j != std::string::npos) { o += "[]"; i = j; continue; }
        }
        o += s[i];
    }
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: ovl_layout_check <Assets> [tag...] [--show N] [--dump tag]\n";
        return 2;
    }
    register_all();
    std::set<std::string> want;
    std::string dump_tag;
    std::size_t show = 8;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--show" && i + 1 < argc) show = std::stoul(argv[++i]);
        else if (a == "--dump" && i + 1 < argc) dump_tag = argv[++i];
        else want.insert(a);
    }
    if (want.empty())
        for (auto& [t, _] : Registry::get().all()) want.insert(t);
    if (!dump_tag.empty()) want.insert(dump_tag);

    std::map<std::string, int> ok, bad;
    std::map<std::string, std::map<std::string, int>> issues;
    std::map<std::string, std::string> example;
    std::map<std::string, std::set<std::string>> unk;
    bool dumped = false;
    for (auto& e : fs::recursive_directory_iterator(argv[1])) {
        auto p = e.path().string();
        const std::string suf = ".common.ovl";
        if (p.size() < suf.size() || p.compare(p.size() - suf.size(), suf.size(), suf)) continue;
        std::optional<PairView> v;
        try { v.emplace(PairView::load(p)); } catch (...) { continue; }
        const std::string rel = p.substr(p.find("Assets/") + 7);
        for (auto& o : v->objects()) {
            if (!want.count(o.tag) || !Registry::get().find(o.tag)) continue;
            std::vector<Issue> is;
            Json j = decode_object(*v, o, &is);
            count_unk(j, "", unk[o.tag]);
            if (!dumped && o.tag == dump_tag) {
                std::cout << "// " << o.name << " in " << rel << "\n" << j.dump() << "\n";
                dumped = true;
            }
            if (is.empty()) { ++ok[o.tag]; continue; }
            ++bad[o.tag];
            std::set<std::string> seen;
            for (auto& i : is) {
                std::string key = strip_index(i.where) + ": " + i.what;
                if (!seen.insert(key).second) continue;
                ++issues[o.tag][key];
                if (!example.count(o.tag + key)) example[o.tag + key] = o.name + " in " + rel;
            }
        }
    }
    int nbad = 0;
    for (auto& t : want) {
        if (!Registry::get().find(t)) { std::cout << "---- " << t << ": no layout registered\n"; continue; }
        std::cout << "---- " << t << ": ok " << ok[t] << ", with issues " << bad[t]
                  << ", unk fields " << unk[t].size() << "\n";
        nbad += bad[t];
        std::vector<std::pair<int, std::string>> v;
        for (auto& [k, n] : issues[t]) v.push_back({n, k});
        std::sort(v.rbegin(), v.rend());
        for (std::size_t i = 0; i < v.size() && i < show; ++i)
            std::cout << "  " << v[i].first << "× " << v[i].second << "   e.g. " << example[t + v[i].second] << "\n";
        if (v.size() > show) std::cout << "  (+" << v.size() - show << " other issues)\n";
        std::size_t k = 0;
        for (auto& u : unk[t]) {
            if (k++ >= show) { std::cout << "  (+" << unk[t].size() - show << " other unk fields)\n"; break; }
            std::cout << "  unk: " << u << "\n";
        }
    }
    return nbad ? 1 : 0;
}
