#pragma once

// Declarative struct layouts for the data-only loader types (path types,
// carried items, research categories, animations...), decoded straight to
// JSON on top of PairView and checked for coverage:
//
//   - every byte of every struct instance is claimed by a field;
//   - every relocated pointer slot and every symbol-reference slot inside it
//     is consumed by a field that follows it (string, array, symbol...);
//   - fields declared Zero are 0 in every object of the game.
//
// `ovl_layout_check <Assets>` runs this on every object of every registered
// tag; `ovlextract --types data` writes <pair>.data.json. Layouts are
// registered per group in extractors/src/DataExtractor_*.cpp and documented
// in docs/RCT3_OVL_FORMAT.md.

#include "ovl/PairView.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ovl::layout {

// ---- a small JSON value -----------------------------------------------------
class Json {
public:
    using Array  = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;  // keeps field order

    Json() : v_(nullptr) {}
    Json(std::nullptr_t) : v_(nullptr) {}
    Json(bool b) : v_(b) {}
    Json(int x) : v_(static_cast<std::int64_t>(x)) {}
    Json(std::int64_t x) : v_(x) {}
    Json(std::uint32_t x) : v_(static_cast<std::int64_t>(x)) {}
    Json(std::int32_t x, int) : v_(static_cast<std::int64_t>(x)) {}
    Json(double d) : v_(d) {}
    Json(float f) : v_(static_cast<double>(f)) {}
    Json(const char* s) : v_(std::string(s)) {}
    Json(std::string s) : v_(std::move(s)) {}
    Json(Array a) : v_(std::move(a)) {}
    Json(Object o) : v_(std::move(o)) {}

    static Json array() { return Json(Array{}); }
    static Json object() { return Json(Object{}); }

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(v_); }
    bool is_int() const { return std::holds_alternative<std::int64_t>(v_); }
    bool is_array() const { return std::holds_alternative<Array>(v_); }
    bool is_object() const { return std::holds_alternative<Object>(v_); }
    std::int64_t as_int() const { return std::get<std::int64_t>(v_); }
    double as_number() const;
    const std::string& as_string() const { return std::get<std::string>(v_); }
    Array& arr() { return std::get<Array>(v_); }
    const Array& arr() const { return std::get<Array>(v_); }
    Object& obj() { return std::get<Object>(v_); }
    const Object& obj() const { return std::get<Object>(v_); }

    void push(Json j) { arr().push_back(std::move(j)); }
    void set(std::string k, Json j) { obj().emplace_back(std::move(k), std::move(j)); }
    const Json* get(const std::string& k) const;

    // Compact for scalars and short arrays, one key per line for objects.
    std::string dump(int indent = 0) const;

private:
    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object> v_;
};

// ---- layouts ----------------------------------------------------------------
enum class K {
    U8, I8, U16, I16, U32, I32, F32,
    Hex32,     // u32 shown as "0x…" (flags, colours, hashes)
    Colour,    // u32 0xAARRGGBB shown as "AARRGGBB"
    Zero,      // u32 that is 0 in every object (reserved / runtime slot)
    ZeroBytes, // n bytes (count.fixed) that are all 0
    Raw,       // n bytes (count.fixed) kept as hex: opaque, documented as such
    Str,       // pointer to a C string (null pointer -> null)
    WStr,      // pointer to a UTF-16LE string
    Ref,       // symbol-reference slot -> "name:tag" (a pointer to own data -> {"ptr": ...})
    Inline,    // `count` instances of `sub` stored in place
    Ptr,       // pointer to one instance of `sub` (null -> null)
    Array,     // pointer to `count` instances of `sub` (stride sub->size)
    PtrArray,  // pointer to `count` pointers, each to one `sub` (null entries -> null)
    RefArray,  // pointer to `count` symbol-reference slots
    StrArray,  // pointer to `count` string pointers
    Scalars,   // pointer to `count` scalars of kind `elem` (U8..F32)
    InlineScalars, // `count` scalars of kind `elem` in place
    Custom,    // decoded by `custom`
};

struct Struct;
class Ctx;

// Element count of an array field: a constant, the value of an earlier field
// of the same struct (optionally times / plus a constant), or a function.
struct Count {
    int fixed = -1;
    std::string field;
    int mul = 1, add = 0;
    std::function<std::int64_t(const Ctx&, std::uint32_t base, const Json& so_far)> fn;
    static Count n(int k) { Count c; c.fixed = k; return c; }
    static Count of(std::string f, int mul = 1, int add = 0) { Count c; c.field = std::move(f); c.mul = mul; c.add = add; return c; }
};

using CustomFn = std::function<Json(Ctx&, std::uint32_t field_addr, std::uint32_t base, const Json& so_far)>;

struct Field {
    std::uint32_t off = 0;
    std::string   name;
    K             k = K::U32;
    const Struct* sub = nullptr;  // Inline / Ptr / Array / PtrArray
    Count         count;
    K             elem = K::U32;  // Scalars / InlineScalars
    std::uint32_t width = 0;      // bytes claimed in the struct (computed when 0)
    CustomFn      custom;         // Custom: decodes, covers and touches by itself
};

struct Struct {
    std::string        name;
    std::uint32_t      size = 0;
    std::vector<Field> fields;
};

// ---- decoding context --------------------------------------------------------
struct Issue {
    std::string where;  // "Path.ptd+0x14"
    std::string what;
};

class Ctx {
public:
    explicit Ctx(const PairView& v) : v_(v) {}
    const PairView& view() const { return v_; }

    // Decodes one instance of `s` at `a`; checks its bytes and slots.
    Json decode(const Struct& s, std::uint32_t a);

    // For Custom fields and array payloads.
    void cover(std::uint32_t a, std::uint32_t n) { covered_.push_back({a, n}); }
    void touch(std::uint32_t slot) { touched_.insert(slot); }
    // Pointer at a relocated slot (touches it); nullopt when the slot is 0
    // and not relocated; an issue when it is non-zero and not relocated.
    std::optional<std::uint32_t> pointer(std::uint32_t slot, const std::string& where);
    void issue(std::string where, std::string what) { issues_.push_back({std::move(where), std::move(what)}); }
    Json scalar(K k, std::uint32_t a) const;

    const std::vector<Issue>& issues() const { return issues_; }
    std::string path;  // current struct path for messages
    // The object being decoded (its symbol name and extra-data chunks), for
    // Custom fields whose data lives outside the struct (modelanim, mdl).
    const PairView::Object* object = nullptr;

private:
    const PairView& v_;
    std::set<std::uint32_t> touched_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> covered_;
    std::vector<Issue> issues_;
    int depth_ = 0;
};

// ---- registry ----------------------------------------------------------------
struct TagLayout {
    std::string tag;
    std::string title;    // "PathType"
    std::string section;  // docs heading, for messages
    // Chooses the struct for one object (versions, flags); nullptr = unsupported.
    std::function<const Struct*(const PairView&, std::uint32_t a)> choose;
};

class Registry {
public:
    static Registry& get();
    void add(TagLayout t) { tags_[t.tag] = std::move(t); }
    const TagLayout* find(const std::string& tag) const;
    const std::map<std::string, TagLayout>& all() const { return tags_; }
    // Keeps Struct objects alive for the program's lifetime.
    Struct& make(std::string name, std::uint32_t size, std::vector<Field> fields);

private:
    std::map<std::string, TagLayout> tags_;
    std::vector<std::unique_ptr<Struct>> structs_;
};

// Decodes one object (symbol + fields); issues go to `issues`.
Json decode_object(const PairView& v, const PairView::Object& o, std::vector<Issue>* issues);

// Field helpers, to keep layout tables short.
inline Field f(std::uint32_t off, std::string name, K k = K::U32) { Field x; x.off = off; x.name = std::move(name); x.k = k; return x; }
inline Field zero(std::uint32_t off, std::uint32_t n = 4) {
    Field x; x.off = off; x.name = "_zero"; x.k = n == 4 ? K::Zero : K::ZeroBytes; x.count = Count::n(int(n)); return x;
}
inline Field raw(std::uint32_t off, std::string name, std::uint32_t n) { Field x; x.off = off; x.name = std::move(name); x.k = K::Raw; x.count = Count::n(int(n)); return x; }
inline Field sub(std::uint32_t off, std::string name, K k, const Struct& s, Count c = Count::n(1)) {
    Field x; x.off = off; x.name = std::move(name); x.k = k; x.sub = &s; x.count = std::move(c); return x;
}
inline Field arr(std::uint32_t off, std::string name, K k, Count c, K elem = K::U32) {
    Field x; x.off = off; x.name = std::move(name); x.k = k; x.count = std::move(c); x.elem = elem; return x;
}
inline Field custom(std::uint32_t off, std::string name, std::uint32_t width, CustomFn fn) {
    Field x; x.off = off; x.name = std::move(name); x.k = K::Custom; x.width = width; x.custom = std::move(fn); return x;
}

std::uint32_t scalar_size(K k);

// Registration entry points, one per group file (DataExtractor_*.cpp).
void register_all();

}  // namespace ovl::layout
