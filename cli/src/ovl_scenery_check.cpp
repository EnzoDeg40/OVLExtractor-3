// Check the scenery codecs (ftx, shs, svd, sid) against every object of the game:
//   1. coverage: every relocated / symbol-ref slot of the object's struct is
//      one the decoder consumed;
//   2. round trip: decode -> encode into a fresh pair -> decode -> equal.
//
//   ovl_scenery_check <Assets dir> [tag ...]
#include "ovl/Error.hpp"
#include "ovl/PairView.hpp"
#include "ovl/SceneryCodec.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>

namespace fs = std::filesystem;
using namespace ovl;

namespace {

std::uint32_t struct_size(const PairView& v, const std::string& tag, std::uint32_t a) {
    if (tag == "ftx") return 36;
    if (tag == "shs") return 0x38;
    if (tag == "svd") {
        auto f = v.u32(a);
        return (f & 0x02000000u) ? 0x3C : (f & 0x01000000u) ? 0x38 : 0x34;
    }
    auto ver = v.u16(a + 0xA);
    return ver == 0 ? 0xD4 : ver == 1 ? 0xE4 : 0xEC;
}

template <class T, class Dec, class Enc>
std::string check(const PairView& v, const PairView::Object& o, Dec dec, Enc enc) {
    Touched t;
    T x = dec(v, o.data, &t);
    for (auto slot : v.slots_in(o.data, struct_size(v, o.tag, o.data)))
        if (!t.count(slot)) return "slot +" + std::to_string(slot - o.data) + " not decoded";
    OvlBuilder b(5);
    const std::string name = o.name.substr(0, o.name.rfind(':'));
    enc(b, name, x);
    auto im = b.build();
    PairView w(im[0], im[1]);
    auto* o2 = w.find(o.name);
    if (!o2) return "object missing after encode";
    T y = dec(w, o2->data, nullptr);
    if (!(x == y)) return "round trip differs";
    return "";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: ovl_scenery_check <Assets> [tag...]\n"; return 2; }
    std::set<std::string> tags{"ftx", "shs", "svd", "sid"};
    if (argc > 2) tags = std::set<std::string>(argv + 2, argv + argc);
    std::map<std::string, int> ok;
    std::map<std::string, std::map<std::string, int>> bad;
    std::map<std::string, std::string> example;
    for (auto& e : fs::recursive_directory_iterator(argv[1])) {
        auto p = e.path().string();
        const std::string suf = ".common.ovl";
        if (p.size() < suf.size() || p.compare(p.size() - suf.size(), suf.size(), suf)) continue;
        std::optional<PairView> v;
        try { v.emplace(PairView::load(p)); } catch (...) { continue; }
        for (auto& o : v->objects()) {
            if (!tags.count(o.tag)) continue;
            std::string err;
            try {
                if (o.tag == "ftx") err = check<Ftx>(*v, o, decode_ftx, encode_ftx);
                else if (o.tag == "shs") err = check<Shs>(*v, o, decode_shs, encode_shs);
                else if (o.tag == "svd") err = check<Svd>(*v, o, decode_svd, encode_svd);
                else if (o.tag == "sid") err = check<Sid>(*v, o, decode_sid, encode_sid);
            } catch (const std::exception& ex) {
                err = std::string("error: ") + ex.what();
            }
            if (err.empty()) { ++ok[o.tag]; continue; }
            ++bad[o.tag][err];
            auto key = o.tag + " " + err;
            if (!example.count(key)) example[key] = o.name + " in " + p.substr(p.find("Assets/") + 7);
        }
    }
    int nbad = 0;
    for (auto& [t, n] : ok) std::cout << "ok   " << t << ": " << n << "\n";
    for (auto& [t, m] : bad)
        for (auto& [err, n] : m) {
            std::cout << "FAIL " << t << ": " << err << ": " << n << "   e.g. " << example[t + " " + err] << "\n";
            nbad += n;
        }
    return nbad ? 1 : 0;
}
