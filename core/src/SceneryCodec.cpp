#include "ovl/SceneryCodec.hpp"

#include "ovl/Error.hpp"

#include <algorithm>
#include <cstring>
#include <map>

namespace ovl {

namespace {

std::uint32_t rd(const Bytes& b, std::uint32_t off) {
    std::uint32_t v;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}
void wr(Bytes& b, std::uint32_t off, std::uint32_t v) { std::memcpy(b.data() + off, &v, 4); }
void zero(Bytes& b, std::uint32_t off) { wr(b, off, 0); }

// Slot readers: they mark the slot as consumed and zero it in `raw` (whose
// first byte sits at address `base`).
struct Reader {
    const PairView& v;
    Touched*        t;

    std::optional<std::uint32_t> ptr(std::uint32_t slot, Bytes* raw = nullptr,
                                     std::uint32_t base = 0) const {
        auto p = v.ptr(slot);
        if (p) {
            if (t) t->insert(slot);
            if (raw) zero(*raw, slot - base);
        }
        return p;
    }
    std::uint32_t need(std::uint32_t slot, Bytes* raw, std::uint32_t base, const char* what) const {
        auto p = ptr(slot, raw, base);
        if (!p) throw OvlError(std::string("missing pointer: ") + what);
        return *p;
    }
    std::optional<std::string> ref(std::uint32_t slot, Bytes* raw = nullptr,
                                   std::uint32_t base = 0) const {
        auto r = v.symref(slot);
        if (r) {
            if (t) t->insert(slot);
            if (raw) zero(*raw, slot - base);
        }
        return r;
    }
    std::optional<std::string> str(std::uint32_t slot, Bytes* raw, std::uint32_t base) const {
        auto p = ptr(slot, raw, base);
        if (!p) return std::nullopt;
        return v.cstr(*p);
    }
};

// Builder-side helpers.
std::uint32_t object(OvlBuilder& b, OvlSide side, const std::string& tag, const std::string& name,
                     const Loc& data) {
    const std::string sym = name + ":" + tag;
    b.add_symbol(side, sym, data);
    OvlBuilder::LoaderRefSpec spec;
    spec.loader_type = loader_type(b, tag);
    spec.data = data;
    spec.symbol = sym;
    return b.add_loader_ref(side, std::move(spec));
}

void put(Bytes& dst, std::uint32_t off, const Bytes& src) {
    if (off + src.size() > dst.size()) throw OvlError("encoder overflow");
    std::memcpy(dst.data() + off, src.data(), src.size());
}

}  // namespace

std::uint32_t loader_type(OvlBuilder& b, const std::string& tag) {
    if (auto t = b.find_loader_type(tag)) return *t;
    static const std::map<std::string, ImageLoader> known = {
        {"ftx", {"FGDK", "FlexiTexture", 1, "ftx"}},
        {"shs", {"RCT3", "StaticShape", 1, "shs"}},
        {"svd", {"RCT3", "SceneryItemVisual", 1, "svd"}},
        {"sid", {"RCT3", "SceneryItem", 1, "sid"}},
        {"txt", {"FGDK", "Text", 2, "txt"}},
    };
    auto it = known.find(tag);
    if (it == known.end()) throw OvlError("no loader header known for tag " + tag);
    return b.add_loader_type(it->second);
}

// ============================================================================
// ftx
// ============================================================================

std::uint32_t Ftx::width() const { return rd(raw, 4); }
std::uint32_t Ftx::height() const { return rd(raw, 8); }

Ftx decode_ftx(const PairView& v, std::uint32_t a, Touched* t) {
    Reader r{v, t};
    Ftx f;
    f.raw = v.bytes(a, 36);
    const std::uint32_t n = v.u32(a + 0x14);
    auto op = r.ptr(a + 0x18, &f.raw, a);
    f.has_offsets = op.has_value();
    if (op)
        for (std::uint32_t i = 0; i < n; ++i) f.offsets.push_back(v.u32(*op + 4 * i));
    const std::uint32_t m = v.u32(a + 0x1C);
    const std::uint32_t fp = r.need(a + 0x20, &f.raw, a, "ftx frames");
    for (std::uint32_t k = 0; k < m; ++k) {
        const std::uint32_t fa = fp + 28 * k;
        FtxFrame fr;
        fr.raw = v.bytes(fa, 28);
        const std::uint32_t w = v.u32(fa + 4), h = v.u32(fa + 8);
        fr.palette = v.bytes(r.need(fa + 0x10, &fr.raw, fa, "ftx palette"), 1024);
        fr.texture = v.bytes(r.need(fa + 0x14, &fr.raw, fa, "ftx texture"), w * h);
        if (auto ap = r.ptr(fa + 0x18, &fr.raw, fa)) fr.alpha = v.bytes(*ap, w * h);
        f.frames.push_back(std::move(fr));
    }
    return f;
}

void encode_ftx(OvlBuilder& b, const std::string& name, const Ftx& f) {
    const auto side = OvlSide::Common;
    const std::uint32_t n = f.has_offsets ? std::uint32_t(f.offsets.size()) : 0;
    const std::uint32_t m = std::uint32_t(f.frames.size());
    const std::uint32_t o_off = 36, o_frames = o_off + 4 * n, o_pal = o_frames + 28 * m;
    auto blk = b.add_block(side, 4, Bytes(o_pal + 1024 * m));
    {
        Bytes& d = b.block(blk);
        put(d, 0, f.raw);
        wr(d, 0x14, f.has_offsets ? n : rd(f.raw, 0x14));
        wr(d, 0x1C, m);
        for (std::uint32_t i = 0; i < n; ++i) wr(d, o_off + 4 * i, f.offsets[i]);
        for (std::uint32_t k = 0; k < m; ++k) {
            put(d, o_frames + 28 * k, f.frames[k].raw);
            put(d, o_pal + 1024 * k, f.frames[k].palette);
        }
    }
    if (f.has_offsets) b.set_pointer({blk, 0x18}, {blk, o_off});
    b.set_pointer({blk, 0x20}, {blk, o_frames});
    for (std::uint32_t k = 0; k < m; ++k) {
        const std::uint32_t fa = o_frames + 28 * k;
        b.set_pointer({blk, fa + 0x10}, {blk, o_pal + 1024 * k});
        auto tex = b.add_block(side, 0, f.frames[k].texture);
        b.set_pointer({blk, fa + 0x14}, {tex, 0});
        if (f.frames[k].alpha) {
            auto al = b.add_block(side, 0, *f.frames[k].alpha);
            b.set_pointer({blk, fa + 0x18}, {al, 0});
        }
    }
    object(b, side, "ftx", name, {blk, 0});
}

// ============================================================================
// shs
// ============================================================================

Shs decode_shs(const PairView& v, std::uint32_t a, Touched* t) {
    Reader r{v, t};
    Shs s;
    s.raw = v.bytes(a, 0x38);
    const std::uint32_t count = v.u32(a + 0x24);
    auto shp = r.ptr(a + 0x28, &s.raw, a);  // absent on empty shapes
    if (!shp && count) throw OvlError("missing pointer: shs meshes");
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t ma = r.need(*shp + 4 * i, nullptr, 0, "shs mesh");
        ShsMesh m;
        m.raw = v.bytes(ma, 40);
        m.ftx = r.ref(ma + 4, &m.raw, ma);
        m.txs = r.ref(ma + 8, &m.raw, ma);
        const std::uint32_t vc = v.u32(ma + 0x18), ic = v.u32(ma + 0x1C);
        const std::uint32_t transparency = v.u32(ma + 0xC);
        const std::uint32_t words = ic * (transparency ? 3 : 1);
        if (auto vp = r.ptr(ma + 0x20, &m.raw, ma)) m.vertices = v.bytes(*vp, vc * 36);
        if (auto ip = r.ptr(ma + 0x24, &m.raw, ma)) m.indices = v.bytes(*ip, words * 4);
        s.meshes.push_back(std::move(m));
    }
    const std::uint32_t ec = v.u32(a + 0x2C);
    auto pp = r.ptr(a + 0x30, &s.raw, a);
    auto np = r.ptr(a + 0x34, &s.raw, a);
    for (std::uint32_t i = 0; i < ec; ++i) {
        if (pp) s.effect_matrices.push_back(v.bytes(*pp + 64 * i, 64));
        if (np) s.effect_names.push_back(v.cstr(r.need(*np + 4 * i, nullptr, 0, "effect name")));
    }
    return s;
}

void encode_shs(OvlBuilder& b, const std::string& name, const Shs& s) {
    const std::uint32_t count = std::uint32_t(s.meshes.size());
    const std::uint32_t ec = std::uint32_t(std::max(s.effect_matrices.size(), s.effect_names.size()));
    // unique chunk 2: StaticShape, mesh pointer array, meshes
    const std::uint32_t o_arr = 0x38, o_mesh = o_arr + 4 * count;
    auto st = b.add_block(OvlSide::Unique, 2, Bytes(o_mesh + 40 * count));
    // common chunk 4: geometry, then effect name pointers, matrices, names
    std::uint32_t geo = 0;
    std::vector<std::uint32_t> vo, io;
    for (auto& m : s.meshes) {
        vo.push_back(geo);
        geo += std::uint32_t(m.vertices.size());
        io.push_back(geo);
        geo += std::uint32_t(m.indices.size());
    }
    const std::uint32_t o_names = geo, o_mats = o_names + 4 * ec;
    std::uint32_t o_str = o_mats + 64 * ec, end = o_str;
    std::vector<std::uint32_t> so;
    for (auto& n : s.effect_names) {
        so.push_back(end);
        end += (std::uint32_t(n.size()) + 1 + 3) & ~3u;
    }
    std::optional<BlockRef> gb;
    if (end) gb = b.add_block(OvlSide::Common, 4, Bytes(end));
    {
        Bytes& d = b.block(st);
        put(d, 0, s.raw);
        wr(d, 0x24, count);
        wr(d, 0x2C, ec);
        for (std::uint32_t i = 0; i < count; ++i) put(d, o_mesh + 40 * i, s.meshes[i].raw);
    }
    if (gb) {
        Bytes& g = b.block(*gb);
        for (std::uint32_t i = 0; i < count; ++i) {
            put(g, vo[i], s.meshes[i].vertices);
            put(g, io[i], s.meshes[i].indices);
        }
        for (std::uint32_t i = 0; i < s.effect_matrices.size(); ++i)
            put(g, o_mats + 64 * i, s.effect_matrices[i]);
        for (std::uint32_t i = 0; i < s.effect_names.size(); ++i)
            put(g, so[i], Bytes(s.effect_names[i].begin(), s.effect_names[i].end()));
    }
    if (count) b.set_pointer({st, 0x28}, {st, o_arr});
    auto ldr = object(b, OvlSide::Unique, "shs", name, {st, 0});
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t ma = o_mesh + 40 * i;
        b.set_pointer({st, o_arr + 4 * i}, {st, ma});
        const auto& m = s.meshes[i];
        if (m.ftx) b.add_symbol_ref(OvlSide::Unique, ldr, {st, ma + 4}, *m.ftx);
        if (m.txs) b.add_symbol_ref(OvlSide::Unique, ldr, {st, ma + 8}, *m.txs);
        if (!m.vertices.empty()) b.set_pointer({st, ma + 0x20}, {*gb, vo[i]});
        if (!m.indices.empty()) b.set_pointer({st, ma + 0x24}, {*gb, io[i]});
    }
    if (!s.effect_names.empty()) {
        b.set_pointer({st, 0x34}, {*gb, o_names});
        for (std::uint32_t i = 0; i < s.effect_names.size(); ++i)
            b.set_pointer({*gb, o_names + 4 * i}, {*gb, so[i]});
    }
    if (!s.effect_matrices.empty()) b.set_pointer({st, 0x30}, {*gb, o_mats});
}

// ============================================================================
// svd
// ============================================================================

namespace {
std::uint32_t svd_size(std::uint32_t flags) {
    if (flags & 0x02000000u) return 0x3C;  // Wild
    if (flags & 0x01000000u) return 0x38;  // Soaked
    return 0x34;
}
}  // namespace

Svd decode_svd(const PairView& v, std::uint32_t a, Touched* t) {
    Reader r{v, t};
    Svd s;
    const std::uint32_t size = svd_size(v.u32(a));
    s.raw = v.bytes(a, size);
    if (size > 0x34) s.proxy = r.ref(a + 0x34, &s.raw, a);
    const std::uint32_t n = v.u32(a + 0x14);
    const std::uint32_t lp = r.need(a + 0x18, &s.raw, a, "svd lods");
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::uint32_t la = r.need(lp + 4 * i, nullptr, 0, "svd lod");
        SvdLod l;
        l.raw = v.bytes(la, 0x48);
        l.name = r.str(la + 4, &l.raw, la).value_or("");
        l.shs = r.ref(la + 0x08, &l.raw, la);
        l.bsh = r.ref(la + 0x10, &l.raw, la);
        l.ftx = r.ref(la + 0x18, &l.raw, la);
        l.txs = r.ref(la + 0x1C, &l.raw, la);
        const std::uint32_t ac = v.u32(la + 0x3C);
        if (auto ap = r.ptr(la + 0x44, &l.raw, la))
            for (std::uint32_t k = 0; k < ac; ++k) {
                const std::uint32_t slot = r.need(*ap + 4 * k, nullptr, 0, "ban slot");
                l.animations.push_back(r.ref(slot).value_or(""));
            }
        s.lods.push_back(std::move(l));
    }
    return s;
}

void encode_svd(OvlBuilder& b, const std::string& name, const Svd& s) {
    const std::uint32_t size = std::uint32_t(s.raw.size());
    const std::uint32_t n = std::uint32_t(s.lods.size());
    std::uint32_t total = size + 4 * n;
    for (auto& l : s.lods) total += 0x48 + 8 * std::uint32_t(l.animations.size());
    auto blk = b.add_block(OvlSide::Unique, 2, Bytes(total));
    put(b.block(blk), 0, s.raw);
    wr(b.block(blk), 0x14, n);
    auto ldr = object(b, OvlSide::Unique, "svd", name, {blk, 0});
    if (s.proxy) b.add_symbol_ref(OvlSide::Unique, ldr, {blk, 0x34}, *s.proxy);
    b.set_pointer({blk, 0x18}, {blk, size});
    std::uint32_t at = size + 4 * n;
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto& l = s.lods[i];
        const std::uint32_t la = at;
        at += 0x48;
        put(b.block(blk), la, l.raw);
        wr(b.block(blk), la + 0x3C, std::uint32_t(l.animations.size()));
        b.set_pointer({blk, size + 4 * i}, {blk, la});
        b.set_string_pointer({blk, la + 4}, l.name);
        if (l.shs) b.add_symbol_ref(OvlSide::Unique, ldr, {blk, la + 0x08}, *l.shs);
        if (l.bsh) b.add_symbol_ref(OvlSide::Unique, ldr, {blk, la + 0x10}, *l.bsh);
        if (l.ftx) b.add_symbol_ref(OvlSide::Unique, ldr, {blk, la + 0x18}, *l.ftx);
        if (l.txs) b.add_symbol_ref(OvlSide::Unique, ldr, {blk, la + 0x1C}, *l.txs);
        if (!l.animations.empty()) {
            const std::uint32_t arr = at, slots = at + 4 * std::uint32_t(l.animations.size());
            at = slots + 4 * std::uint32_t(l.animations.size());
            b.set_pointer({blk, la + 0x44}, {blk, arr});
            for (std::uint32_t k = 0; k < l.animations.size(); ++k) {
                b.set_pointer({blk, arr + 4 * k}, {blk, slots + 4 * k});
                b.add_symbol_ref(OvlSide::Unique, ldr, {blk, slots + 4 * k}, l.animations[k]);
            }
        }
    }
}

// ============================================================================
// sid
// ============================================================================

std::int32_t Sid::cost() const { return std::int32_t(rd(raw, 0x3C)); }
void Sid::set_cost(std::int32_t c) { wr(raw, 0x3C, std::uint32_t(c)); }
std::int32_t Sid::removal_cost() const { return std::int32_t(rd(raw, 0x40)); }
void Sid::set_removal_cost(std::int32_t c) { wr(raw, 0x40, std::uint32_t(c)); }
std::uint32_t Sid::type() const { return rd(raw, 0x48); }

namespace {
std::uint32_t sid_size(std::uint16_t version) {
    switch (version) {
        case 0: return 0xD4;
        case 1: return 0xE4;
        case 2: return 0xEC;
        default: throw OvlError("unknown sid structure version " + std::to_string(version));
    }
}
std::uint32_t bit_words(std::int32_t lo, std::int32_t hi) {
    const std::int32_t h = hi - lo;
    return h > 0 ? std::uint32_t((h + 31) / 32) : 0;
}
}  // namespace

Sid decode_sid(const PairView& v, std::uint32_t a, Touched* t) {
    Reader r{v, t};
    Sid s;
    const std::uint32_t size = sid_size(v.u16(a + 0xA));
    s.raw = v.bytes(a, size);
    const std::uint32_t nsq = v.u32(a + 0x10) * v.u32(a + 0x14);
    if (auto dp = r.ptr(a + 0x18, &s.raw, a))
        for (std::uint32_t k = 0; k < nsq; ++k) {
            const std::uint32_t qa = *dp + 20 * k;
            SidSquare q;
            q.raw = v.bytes(qa, 20);
            if (auto bp = r.ptr(qa + 0xC, &q.raw, qa)) {
                const std::uint32_t n = bit_words(v.i32(qa + 4), v.i32(qa + 8));
                for (std::uint32_t w = 0; w < n; ++w) q.bits.push_back(v.u32(*bp + 4 * w));
            }
            s.squares.push_back(std::move(q));
        }
    if (auto tp = r.ptr(a + 0x38, &s.raw, a)) s.tracks = v.bytes(*tp, 88 * v.u32(a + 0x34));
    s.supports = r.str(a + 0x4C, &s.raw, a);
    const std::uint32_t nsvd = v.u32(a + 0x50);
    if (auto sp = r.ptr(a + 0x54, &s.raw, a))
        for (std::uint32_t i = 0; i < nsvd; ++i) s.svds.push_back(r.ref(*sp + 4 * i).value_or(""));
    s.icon = r.ref(a + 0x58, &s.raw, a);
    s.group_icon = r.ref(a + 0x5C, &s.raw, a);
    s.group_name = r.ref(a + 0x60, &s.raw, a);
    s.ovl_path = r.str(a + 0x64, &s.raw, a);
    const std::uint32_t np = v.u32(a + 0x68);
    if (auto pp = r.ptr(a + 0x6C, &s.raw, a))
        for (std::uint32_t i = 0; i < np; ++i)
            s.params.emplace_back(r.str(*pp + 8 * i, nullptr, 0).value_or(""),
                                  r.str(*pp + 8 * i + 4, nullptr, 0).value_or(""));
    if (v.u32(a + 0x70) || v.ptr(a + 0x74)) throw OvlError("sid with sounds: not supported yet");
    s.name = r.ref(a + 0x78, &s.raw, a);
    const std::uint32_t na = v.u32(a + 0xB8);
    if (auto ap = r.ptr(a + 0xBC, &s.raw, a))
        for (std::uint32_t i = 0; i < na; ++i) s.anr_names.push_back(r.str(*ap + 4 * i, nullptr, 0).value_or(""));
    if (size > 0xD4 && v.ptr(a + 0xD4)) throw OvlError("sid with extra sounds: not supported yet");
    return s;
}

void encode_sid(OvlBuilder& b, const std::string& name, const Sid& s) {
    const std::uint32_t size = std::uint32_t(s.raw.size());
    const std::uint32_t nsvd = std::uint32_t(s.svds.size());
    const std::uint32_t o_svd = size, o_tracks = o_svd + 4 * nsvd;
    auto st = b.add_block(OvlSide::Unique, 2, Bytes(o_tracks + std::uint32_t(s.tracks.size())));
    {
        Bytes& d = b.block(st);
        put(d, 0, s.raw);
        wr(d, 0x50, nsvd);
        wr(d, 0x68, std::uint32_t(s.params.size()));
        wr(d, 0xB8, std::uint32_t(s.anr_names.size()));
        put(d, o_tracks, s.tracks);
    }
    // common chunk 2: squares, block bits, params, anr name pointers
    std::uint32_t nsq = std::uint32_t(s.squares.size());
    std::uint32_t o_bits = 20 * nsq, at = o_bits;
    std::vector<std::uint32_t> bo;
    for (auto& q : s.squares) {
        bo.push_back(at);
        at += 4 * std::uint32_t(q.bits.size());
    }
    const std::uint32_t o_params = at, o_anr = o_params + 8 * std::uint32_t(s.params.size());
    const std::uint32_t cend = o_anr + 4 * std::uint32_t(s.anr_names.size());
    std::optional<BlockRef> cb;
    if (cend) {
        cb = b.add_block(OvlSide::Common, 2, Bytes(cend));
        Bytes& c = b.block(*cb);
        for (std::uint32_t k = 0; k < nsq; ++k) {
            put(c, 20 * k, s.squares[k].raw);
            for (std::uint32_t w = 0; w < s.squares[k].bits.size(); ++w) wr(c, bo[k] + 4 * w, s.squares[k].bits[w]);
        }
    }
    auto ldr = object(b, OvlSide::Unique, "sid", name, {st, 0});
    if (nsq) b.set_pointer({st, 0x18}, {*cb, 0});
    for (std::uint32_t k = 0; k < nsq; ++k)
        if (!s.squares[k].bits.empty()) b.set_pointer({*cb, 20 * k + 0xC}, {*cb, bo[k]});
    if (!s.tracks.empty()) b.set_pointer({st, 0x38}, {st, o_tracks});
    if (s.supports) b.set_string_pointer({st, 0x4C}, *s.supports);
    if (nsvd) b.set_pointer({st, 0x54}, {st, o_svd});
    for (std::uint32_t i = 0; i < nsvd; ++i) b.add_symbol_ref(OvlSide::Unique, ldr, {st, o_svd + 4 * i}, s.svds[i]);
    if (s.icon) b.add_symbol_ref(OvlSide::Unique, ldr, {st, 0x58}, *s.icon);
    if (s.group_icon) b.add_symbol_ref(OvlSide::Unique, ldr, {st, 0x5C}, *s.group_icon);
    if (s.group_name) b.add_symbol_ref(OvlSide::Unique, ldr, {st, 0x60}, *s.group_name);
    if (s.ovl_path) b.set_string_pointer({st, 0x64}, *s.ovl_path);
    if (!s.params.empty()) {
        b.set_pointer({st, 0x6C}, {*cb, o_params});
        for (std::uint32_t i = 0; i < s.params.size(); ++i) {
            b.set_string_pointer({*cb, o_params + 8 * i}, s.params[i].first);
            b.set_string_pointer({*cb, o_params + 8 * i + 4}, s.params[i].second);
        }
    }
    if (s.name) b.add_symbol_ref(OvlSide::Unique, ldr, {st, 0x78}, *s.name);
    if (!s.anr_names.empty()) {
        b.set_pointer({st, 0xBC}, {*cb, o_anr});
        for (std::uint32_t i = 0; i < s.anr_names.size(); ++i)
            b.set_string_pointer({*cb, o_anr + 4 * i}, s.anr_names[i]);
    }
}

}  // namespace ovl
