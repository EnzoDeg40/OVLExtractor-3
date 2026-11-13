#include "ovl/TextOvl.hpp"

#include "ovl/Error.hpp"
#include "ovl/OvlBuilder.hpp"

#include <algorithm>
#include <cctype>

namespace ovl {

namespace {

constexpr std::size_t kBlockTarget = 16384;  // a text block closes once it reaches this

void skip_ws(const std::string& s, std::size_t& p) {
    while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
}

std::uint32_t hex4(const std::string& s, std::size_t p) {
    if (p + 4 > s.size()) throw OvlError("JSON: short \\u escape");
    return std::uint32_t(std::stoul(s.substr(p, 4), nullptr, 16));
}

// Parses a JSON string at s[p] == '"' into UTF-16, advancing p past it.
std::u16string json_string(const std::string& s, std::size_t& p) {
    if (p >= s.size() || s[p] != '"') throw OvlError("JSON: expected a string");
    ++p;
    std::u16string out;
    while (true) {
        if (p >= s.size()) throw OvlError("JSON: unterminated string");
        unsigned char c = static_cast<unsigned char>(s[p]);
        if (c == '"') { ++p; return out; }
        if (c == '\\') {
            char e = s.at(p + 1);
            p += 2;
            switch (e) {
                case '"': out += u'"'; break;
                case '\\': out += u'\\'; break;
                case '/': out += u'/'; break;
                case 'b': out += u'\b'; break;
                case 'f': out += u'\f'; break;
                case 'n': out += u'\n'; break;
                case 'r': out += u'\r'; break;
                case 't': out += u'\t'; break;
                case 'u': out += char16_t(hex4(s, p)); p += 4; break;
                default: throw OvlError("JSON: bad escape");
            }
            continue;
        }
        // UTF-8 -> code point -> UTF-16
        std::uint32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 3; }
        else throw OvlError("JSON: invalid UTF-8");
        ++p;
        for (int i = 0; i < n; ++i, ++p) {
            if (p >= s.size() || (static_cast<unsigned char>(s[p]) & 0xC0) != 0x80)
                throw OvlError("JSON: invalid UTF-8");
            cp = cp << 6 | (static_cast<unsigned char>(s[p]) & 0x3F);
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += char16_t(0xD800 + (cp >> 10));
            out += char16_t(0xDC00 + (cp & 0x3FF));
        } else {
            out += char16_t(cp);
        }
    }
}

std::string ascii(const std::u16string& u) {
    std::string s;
    for (auto c : u) {
        if (c > 0x7E || c < 0x20) throw OvlError("symbol names must be printable ASCII");
        s += char(c);
    }
    return s;
}

}  // namespace

std::vector<TextEntry> read_text_json(const std::string& s) {
    std::vector<TextEntry> out;
    std::size_t p = 0;
    skip_ws(s, p);
    if (p >= s.size() || s[p] != '{') throw OvlError("JSON: expected an object");
    ++p;
    skip_ws(s, p);
    if (p < s.size() && s[p] == '}') return out;
    while (true) {
        skip_ws(s, p);
        auto key = ascii(json_string(s, p));
        skip_ws(s, p);
        if (p >= s.size() || s[p] != ':') throw OvlError("JSON: expected ':'");
        ++p;
        skip_ws(s, p);
        out.push_back({key, json_string(s, p)});
        skip_ws(s, p);
        if (p < s.size() && s[p] == ',') { ++p; continue; }
        if (p < s.size() && s[p] == '}') return out;
        throw OvlError("JSON: expected ',' or '}'");
    }
}

std::array<OvlImage, 2> build_text_ovl(std::vector<TextEntry> entries, std::uint32_t version) {
    std::sort(entries.begin(), entries.end(),
              [](const TextEntry& a, const TextEntry& b) { return a.name < b.name; });
    for (std::size_t i = 1; i < entries.size(); ++i)
        if (entries[i].name == entries[i - 1].name)
            throw OvlError("duplicate text symbol: " + entries[i].name);

    OvlBuilder b(version);
    const auto txt = b.add_loader_type({"FGDK", "Text", 2, "txt"});

    // Pack the strings, remembering where each one landed.
    std::vector<Loc> where;
    Bytes cur;
    auto flush = [&] {
        if (cur.empty()) return;
        auto ref = b.add_block(OvlSide::Common, 0, std::move(cur));
        for (auto& w : where)
            if (w.offset & 0x80000000u) { w.block = ref; w.offset &= 0x7FFFFFFFu; }
        cur.clear();
    };
    for (auto& e : entries) {
        // Offsets inside the open block are tagged until the block exists.
        where.push_back(Loc{BlockRef{}, std::uint32_t(cur.size()) | 0x80000000u});
        for (char16_t c : e.text) { cur.push_back(std::uint8_t(c)); cur.push_back(std::uint8_t(c >> 8)); }
        cur.push_back(0);
        cur.push_back(0);
        if (cur.size() >= kBlockTarget) flush();
    }
    flush();

    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto sym = entries[i].name + ":txt";
        b.add_symbol(OvlSide::Common, sym, where[i]);
        OvlBuilder::LoaderRefSpec spec;
        spec.loader_type = txt;
        spec.data = where[i];
        spec.symbol = sym;
        if (i == 0) {
            spec.extra_chunks.push_back(Bytes{1, 0, 0, 0});
            spec.v5_extra.push_back(V5ExtraInfo{});
        }
        b.add_loader_ref(OvlSide::Common, std::move(spec));
    }
    return b.build();
}

}  // namespace ovl
