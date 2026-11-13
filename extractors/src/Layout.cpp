#include "ovl/extract/Layout.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace ovl::layout {

// ---- Json ---------------------------------------------------------------------
double Json::as_number() const {
    if (auto* i = std::get_if<std::int64_t>(&v_)) return double(*i);
    if (auto* d = std::get_if<double>(&v_)) return *d;
    return 0;
}

const Json* Json::get(const std::string& k) const {
    if (!is_object()) return nullptr;
    for (auto& [key, val] : obj())
        if (key == k) return &val;
    return nullptr;
}

namespace {

void esc(std::string& o, const std::string& s) {
    o += '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    o += '"';
}

bool flat(const Json& j) {
    if (j.is_array()) {
        if (j.arr().size() > 64) return false;
        for (auto& e : j.arr())
            if (e.is_array() || e.is_object()) return false;
        return true;
    }
    return !j.is_object();
}

}  // namespace

std::string Json::dump(int indent) const {
    std::string o;
    const std::string pad(std::size_t(indent) * 2, ' '), pad2(std::size_t(indent + 1) * 2, ' ');
    std::visit(
        [&](auto&& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) o += "null";
            else if constexpr (std::is_same_v<T, bool>) o += x ? "true" : "false";
            else if constexpr (std::is_same_v<T, std::int64_t>) o += std::to_string(x);
            else if constexpr (std::is_same_v<T, double>) {
                if (!std::isfinite(x)) { o += std::isnan(x) ? "\"NaN\"" : (x > 0 ? "\"Infinity\"" : "\"-Infinity\""); return; }
                char b[32];
                std::snprintf(b, sizeof b, "%.9g", x);
                o += b;
            } else if constexpr (std::is_same_v<T, std::string>) esc(o, x);
            else if constexpr (std::is_same_v<T, Array>) {
                if (x.empty()) { o += "[]"; return; }
                bool f1 = flat(*this);
                o += '[';
                for (std::size_t i = 0; i < x.size(); ++i) {
                    if (i) o += f1 ? ", " : ",";
                    if (!f1) o += "\n" + pad2;
                    o += x[i].dump(indent + 1);
                }
                if (!f1) o += "\n" + pad;
                o += ']';
            } else {
                if (x.empty()) { o += "{}"; return; }
                o += '{';
                for (std::size_t i = 0; i < x.size(); ++i) {
                    if (i) o += ',';
                    o += "\n" + pad2;
                    esc(o, x[i].first);
                    o += ": " + x[i].second.dump(indent + 1);
                }
                o += "\n" + pad + '}';
            }
        },
        v_);
    return o;
}

// ---- scalars -----------------------------------------------------------------
std::uint32_t scalar_size(K k) {
    switch (k) {
        case K::U8: case K::I8: return 1;
        case K::U16: case K::I16: return 2;
        default: return 4;
    }
}

Json Ctx::scalar(K k, std::uint32_t a) const {
    switch (k) {
        case K::U8: return Json(std::uint32_t(v_.bytes(a, 1)[0]));
        case K::I8: return Json(std::int64_t(std::int8_t(v_.bytes(a, 1)[0])));
        case K::U16: return Json(std::uint32_t(v_.u16(a)));
        case K::I16: return Json(std::int64_t(std::int16_t(v_.u16(a))));
        case K::I32: return Json(std::int64_t(v_.i32(a)));
        case K::F32: return Json(v_.f32(a));
        case K::Hex32: { char b[16]; std::snprintf(b, sizeof b, "0x%08X", v_.u32(a)); return Json(std::string(b)); }
        case K::Colour: { char b[16]; std::snprintf(b, sizeof b, "%08X", v_.u32(a)); return Json(std::string(b)); }
        default: return Json(v_.u32(a));
    }
}

std::optional<std::uint32_t> Ctx::pointer(std::uint32_t slot, const std::string& where) {
    touch(slot);
    if (v_.is_reloc(slot)) return v_.u32(slot);
    if (auto s = v_.symref(slot)) {
        issue(where, "symbol reference " + *s + " where a pointer to own data was expected");
        return std::nullopt;
    }
    if (std::uint32_t x = v_.u32(slot)) {
        char b[48];
        std::snprintf(b, sizeof b, "non-relocated pointer 0x%08X", x);
        issue(where, b);
    }
    return std::nullopt;
}

namespace {

std::string hex_bytes(const Bytes& b) {
    static const char* d = "0123456789abcdef";
    std::string o;
    for (auto c : b) { o += d[c >> 4]; o += d[c & 15]; }
    return o;
}

std::string at(const std::string& path, std::uint32_t off) {
    char b[24];
    std::snprintf(b, sizeof b, "+0x%X", off);
    return path + b;
}

// Name of the object whose struct starts at `a`, if any.
std::optional<std::string> object_at(const PairView& v, std::uint32_t a) {
    for (auto& o : v.objects())
        if (o.data == a) return o.name;
    return std::nullopt;
}

}  // namespace

Json Ctx::decode(const Struct& s, std::uint32_t a) {
    Json out = Json::object();
    if (++depth_ > 64) { issue(path, "nesting too deep"); --depth_; return out; }
    const std::string saved = path;
    path = path.empty() ? s.name : path + "/" + s.name;
    if (v_.block_remaining(a) < s.size) {
        issue(path, "struct of " + std::to_string(s.size) + " B crosses its block end (" +
                        std::to_string(v_.block_remaining(a)) + " B left)");
        path = saved;
        --depth_;
        return out;
    }
    std::vector<int> claim(s.size, 0);
    for (const Field& fd : s.fields) {
        const std::uint32_t addr = a + fd.off;
        const std::string where = at(path, fd.off);
        std::uint32_t width = fd.width;
        auto count = [&]() -> std::int64_t {
            const Count& c = fd.count;
            if (c.fn) return c.fn(*this, a, out);
            if (c.fixed >= 0) return c.fixed;
            const Json* j = out.get(c.field);
            if (!j || !j->is_int()) { issue(where, "count field '" + c.field + "' missing"); return 0; }
            return j->as_int() * c.mul + c.add;
        };
        Json val;
        try {
            switch (fd.k) {
                case K::U8: case K::I8: case K::U16: case K::I16: case K::U32: case K::I32:
                case K::F32: case K::Hex32: case K::Colour:
                    width = scalar_size(fd.k);
                    val = scalar(fd.k, addr);
                    break;
                case K::Zero:
                    width = 4;
                    if (std::uint32_t x = v_.u32(addr)) {
                        char b[40];
                        std::snprintf(b, sizeof b, "expected 0, found 0x%08X", x);
                        issue(where, b);
                    }
                    break;
                case K::ZeroBytes: {
                    width = std::uint32_t(fd.count.fixed);
                    for (auto c : v_.bytes(addr, width))
                        if (c) { issue(where, "expected zero bytes"); break; }
                    break;
                }
                case K::Raw:
                    width = std::uint32_t(fd.count.fixed);
                    val = hex_bytes(v_.bytes(addr, width));
                    break;
                case K::Str: case K::WStr: {
                    width = 4;
                    if (auto p = pointer(addr, where)) {
                        if (fd.k == K::Str) val = v_.cstr(*p);
                        else {
                            std::u16string w = v_.wstr(*p);
                            std::string u;
                            for (char16_t c : w) {
                                if (c < 0x80) u += char(c);
                                else if (c < 0x800) { u += char(0xC0 | c >> 6); u += char(0x80 | (c & 63)); }
                                else { u += char(0xE0 | c >> 12); u += char(0x80 | (c >> 6 & 63)); u += char(0x80 | (c & 63)); }
                            }
                            val = u;
                        }
                    }
                    break;
                }
                case K::Ref: {
                    width = 4;
                    touch(addr);
                    if (auto s2 = v_.symref(addr)) val = *s2;
                    else if (v_.is_reloc(addr)) {
                        auto n = object_at(v_, v_.u32(addr));
                        val = n ? Json(*n) : Json("<own data>");
                    } else if (std::uint32_t x = v_.u32(addr)) {
                        char b[48];
                        std::snprintf(b, sizeof b, "non-relocated reference 0x%08X", x);
                        issue(where, b);
                    }
                    break;
                }
                case K::Inline: {
                    const std::int64_t n = count();
                    width = std::uint32_t(n) * fd.sub->size;
                    if (fd.count.fixed == 1) val = decode(*fd.sub, addr);
                    else {
                        val = Json::array();
                        for (std::int64_t i = 0; i < n; ++i) val.push(decode(*fd.sub, addr + std::uint32_t(i) * fd.sub->size));
                    }
                    break;
                }
                case K::InlineScalars: {
                    const std::int64_t n = count();
                    width = std::uint32_t(n) * scalar_size(fd.elem);
                    val = Json::array();
                    for (std::int64_t i = 0; i < n; ++i) val.push(scalar(fd.elem, addr + std::uint32_t(i) * scalar_size(fd.elem)));
                    break;
                }
                case K::Ptr: {
                    width = 4;
                    if (auto s2 = v_.symref(addr)) { touch(addr); val = *s2; break; }
                    if (auto p = pointer(addr, where)) val = decode(*fd.sub, *p);
                    break;
                }
                case K::Array: case K::PtrArray: case K::RefArray: case K::StrArray: case K::Scalars: {
                    width = 4;
                    const std::int64_t n = count();
                    auto p = pointer(addr, where);
                    if (!p) {
                        if (n > 0) issue(where, "null pointer with count " + std::to_string(n));
                        val = n > 0 ? Json() : Json::array();
                        break;
                    }
                    if (n < 0 || n > 1000000) { issue(where, "implausible count " + std::to_string(n)); break; }
                    val = Json::array();
                    for (std::int64_t i = 0; i < n; ++i) {
                        if (fd.k == K::Array) { val.push(decode(*fd.sub, *p + std::uint32_t(i) * fd.sub->size)); continue; }
                        if (fd.k == K::Scalars) {
                            const std::uint32_t e = *p + std::uint32_t(i) * scalar_size(fd.elem);
                            if (scalar_size(fd.elem) == 4 && (v_.is_reloc(e) || v_.symref(e)))
                                issue(where, "pointer slot inside a scalar array");
                            val.push(scalar(fd.elem, e));
                            continue;
                        }
                        const std::uint32_t slot = *p + std::uint32_t(i) * 4;
                        const std::string w2 = where + "[" + std::to_string(i) + "]";
                        if (fd.k == K::RefArray) {
                            touch(slot);
                            if (auto s2 = v_.symref(slot)) val.push(*s2);
                            else if (v_.is_reloc(slot)) {
                                auto nm = object_at(v_, v_.u32(slot));
                                val.push(nm ? Json(*nm) : Json("<own data>"));
                            } else {
                                if (v_.u32(slot)) issue(w2, "non-relocated reference");
                                val.push(Json());
                            }
                        } else if (fd.k == K::StrArray) {
                            auto q = pointer(slot, w2);
                            val.push(q ? Json(v_.cstr(*q)) : Json());
                        } else {
                            if (auto s2 = v_.symref(slot)) { touch(slot); val.push(*s2); continue; }
                            auto q = pointer(slot, w2);
                            val.push(q ? decode(*fd.sub, *q) : Json());
                        }
                    }
                    break;
                }
                case K::Custom:
                    val = fd.custom(*this, addr, a, out);
                    break;
            }
        } catch (const std::exception& e) {
            issue(where, std::string("read error: ") + e.what());
        }
        // Claim the field's bytes.
        if (fd.off + width > s.size) issue(where, "field '" + fd.name + "' runs past the struct end");
        for (std::uint32_t i = fd.off; i < fd.off + width && i < s.size; ++i)
            if (claim[i]++) { issue(where, "field '" + fd.name + "' overlaps another"); break; }
        // A scalar must not sit on a pointer or reference slot.
        if (fd.k != K::Custom && fd.k != K::Inline)
            for (std::uint32_t sl : v_.slots_in(addr, width))
                if (!touched_.count(sl)) issue(at(path, sl - a), "pointer / reference slot read as data by '" + fd.name + "'");
        if (fd.name != "_zero" && fd.k != K::Zero && fd.k != K::ZeroBytes) out.set(fd.name, std::move(val));
    }
    // Every byte claimed.
    for (std::uint32_t i = 0; i < s.size;) {
        if (claim[i]) { ++i; continue; }
        std::uint32_t j = i;
        while (j < s.size && !claim[j]) ++j;
        bool zero = true;
        for (auto c : v_.bytes(a + i, j - i)) zero = zero && !c;
        issue(at(path, i), "bytes +0x" + [&] { char b[16]; std::snprintf(b, sizeof b, "%X", i); return std::string(b); }() +
                               "..+0x" + [&] { char b[16]; std::snprintf(b, sizeof b, "%X", j); return std::string(b); }() +
                               " not claimed" + (zero ? " (zero here)" : ""));
        i = j;
    }
    for (std::uint32_t sl : v_.slots_in(a, s.size))
        if (!touched_.count(sl)) issue(at(path, sl - a), "pointer / reference slot not decoded");
    path = saved;
    --depth_;
    return out;
}

// ---- registry ----------------------------------------------------------------
Registry& Registry::get() {
    static Registry r;
    return r;
}

const TagLayout* Registry::find(const std::string& tag) const {
    auto it = tags_.find(tag);
    return it == tags_.end() ? nullptr : &it->second;
}

Struct& Registry::make(std::string name, std::uint32_t size, std::vector<Field> fields) {
    structs_.push_back(std::make_unique<Struct>(Struct{std::move(name), size, std::move(fields)}));
    return *structs_.back();
}

Json decode_object(const PairView& v, const PairView::Object& o, std::vector<Issue>* issues) {
    Json out = Json::object();
    out.set("symbol", o.name);
    const TagLayout* t = Registry::get().find(o.tag);
    if (!t) return out;
    const Struct* s = t->choose(v, o.data);
    Ctx ctx(v);
    ctx.object = &o;
    if (!s) ctx.issue(o.tag, "unsupported variant");
    else {
        Json body = ctx.decode(*s, o.data);
        for (auto& [k, val] : body.obj()) out.set(k, val);
    }
    if (issues)
        for (auto& i : ctx.issues()) issues->push_back(i);
    return out;
}

}  // namespace ovl::layout
