#include "ovl/extract/TextureIndex.hpp"

#include <fstream>
#include <string>
#include <utility>

namespace ovl {

namespace {

// Read a JSON string starting at `pos` (which must point AT the opening
// quote). Advances `pos` past the closing quote. Handles common escape
// sequences (\", \\, \/, \b, \f, \n, \r, \t); unknown escapes pass through
// the second character. Returns empty string + leaves pos unchanged on
// malformed input.
std::string read_json_string(const std::string& s, std::size_t& pos) {
    if (pos >= s.size() || s[pos] != '"') return "";
    ++pos;
    std::string out;
    while (pos < s.size()) {
        char c = s[pos];
        if (c == '"') { ++pos; return out; }
        if (c == '\\' && pos + 1 < s.size()) {
            char n = s[pos + 1];
            switch (n) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                default:   out += n;    break;
            }
            pos += 2;
        } else {
            out += c;
            ++pos;
        }
    }
    return "";  // unterminated
}

// Locate `"<field>":"` in `body` and return the next string value; empty
// string if the field isn't found.
std::string extract_field(const std::string& body, const char* field) {
    std::string pat = "\"";
    pat += field;
    pat += "\":";
    std::size_t p = body.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) ++p;
    return read_json_string(body, p);
}

}  // namespace

bool TextureIndex::load(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) return false;

    std::string line;
    while (std::getline(f, line)) {
        // Each entry sits on its own line. Find the key (first quoted string)
        // and the value object body. Lines like `{` and `}` are skipped.
        std::size_t i = line.find('"');
        if (i == std::string::npos) continue;
        std::string key = read_json_string(line, i);
        if (key.empty()) continue;

        std::size_t obj_start = line.find('{', i);
        if (obj_start == std::string::npos) continue;
        std::size_t obj_end = line.rfind('}');
        if (obj_end == std::string::npos || obj_end <= obj_start) continue;
        std::string body = line.substr(obj_start + 1, obj_end - obj_start - 1);

        TextureIndexEntry e;
        e.symbol = extract_field(body, "symbol");
        e.ovl    = extract_field(body, "ovl");
        e.side   = extract_field(body, "side");
        e.tag    = extract_field(body, "tag");
        if (e.ovl.empty() || e.side.empty()) continue;
        entries_[key] = std::move(e);
    }
    return !entries_.empty();
}

const TextureIndexEntry*
TextureIndex::lookup(const std::string& symbol_lc) const {
    auto it = entries_.find(symbol_lc);
    return it == entries_.end() ? nullptr : &it->second;
}

std::filesystem::path
TextureIndex::resolve_ovl_path(const TextureIndexEntry& e) const {
    auto base = assets_root_ / e.ovl;
    base += (e.side == "unique") ? ".unique.ovl" : ".common.ovl";
    return base;
}

}  // namespace ovl
