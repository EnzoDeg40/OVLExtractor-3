#include "ovl/OvlImage.hpp"

#include "ovl/Error.hpp"
#include "ovl/OvlTypes.hpp"

#include <cctype>
#include <fstream>
#include <iterator>

namespace ovl {

namespace {

class Cursor {
public:
    explicit Cursor(const Bytes& b) : b_(b) {}
    std::uint32_t u32() {
        need(4);
        std::uint32_t v = std::uint32_t(b_[p_]) | std::uint32_t(b_[p_ + 1]) << 8 |
                          std::uint32_t(b_[p_ + 2]) << 16 | std::uint32_t(b_[p_ + 3]) << 24;
        p_ += 4;
        return v;
    }
    std::uint16_t u16() {
        need(2);
        std::uint16_t v = std::uint16_t(b_[p_] | b_[p_ + 1] << 8);
        p_ += 2;
        return v;
    }
    Bytes bytes(std::size_t n) {
        need(n);
        Bytes out(b_.begin() + static_cast<std::ptrdiff_t>(p_),
                  b_.begin() + static_cast<std::ptrdiff_t>(p_ + n));
        p_ += n;
        return out;
    }
    std::string str16() {
        auto n = u16();
        auto v = bytes(n);
        return std::string(v.begin(), v.end());
    }
    std::size_t pos() const { return p_; }
    std::size_t left() const { return b_.size() - p_; }

private:
    void need(std::size_t n) const {
        if (p_ + n > b_.size()) throw OvlError("OVL image truncated");
    }
    const Bytes& b_;
    std::size_t  p_ = 0;
};

void put32(Bytes& o, std::uint32_t v) {
    o.push_back(std::uint8_t(v));
    o.push_back(std::uint8_t(v >> 8));
    o.push_back(std::uint8_t(v >> 16));
    o.push_back(std::uint8_t(v >> 24));
}
void put16(Bytes& o, std::uint16_t v) {
    o.push_back(std::uint8_t(v));
    o.push_back(std::uint8_t(v >> 8));
}
void putstr16(Bytes& o, const std::string& s) {
    if (s.size() > 0xFFFF) throw OvlError("OVL string too long: " + s.substr(0, 40));
    put16(o, std::uint16_t(s.size()));
    o.insert(o.end(), s.begin(), s.end());
}
void append(Bytes& o, const Bytes& b) { o.insert(o.end(), b.begin(), b.end()); }

}  // namespace

std::uint32_t symbol_hash(const std::string& s) {
    std::uint32_t h = 5381;
    for (unsigned char c : s) h = h * 33u + std::uint32_t(std::tolower(c));
    return h;
}

OvlImage read_image(const Bytes& file) {
    Cursor c(file);
    OvlImage im;
    if (c.u32() != kOvlMagic) throw OvlError("not an OVL (bad magic)");
    im.headerversion = c.u32();
    im.version = c.u32();
    const auto v = im.version;
    if (v != 1 && v != 4 && v != 5) throw OvlError("unsupported OVL version " + std::to_string(v));
    if (im.headerversion == 2) {
        if (c.u32() != 16) throw OvlError("bad header v2 extra");
    }
    if (v == 5) {
        im.v5_subversion = c.u32();
        if (im.v5_subversion == 1) {
            // 3 words, then a NUL-terminated string padded to 4 bytes.
            auto start = c.pos();
            c.bytes(12);
            std::size_t n = 0;
            while (c.bytes(1)[0] != 0) ++n;
            ++n;
            if (n % 4) c.bytes(4 - n % 4);
            auto end = c.pos();
            im.header_v5sub1.assign(file.begin() + static_cast<std::ptrdiff_t>(start),
                                    file.begin() + static_cast<std::ptrdiff_t>(end));
        } else if (im.v5_subversion != 0) {
            throw OvlError("unsupported v5 subversion");
        }
    }
    std::uint32_t nref = c.u32();
    for (std::uint32_t i = 0; i < nref; ++i) im.references.push_back(c.str16());

    im.loader_header_unknown = c.u32();
    std::uint32_t nl = c.u32();
    for (std::uint32_t i = 0; i < nl; ++i) {
        ImageLoader l;
        l.loader = c.str16();
        l.name = c.str16();
        l.type = c.u32();
        l.tag = c.str16();
        im.loaders.push_back(std::move(l));
    }

    std::array<std::uint32_t, 9> counts{};
    std::array<std::vector<std::uint32_t>, 9> sizes;
    if (v == 5) {
        for (std::uint32_t i = 0; i < nl; ++i) {
            auto a = c.u32();
            auto b = c.u32();
            im.v5_loader_info.push_back({a, b});
        }
    }
    for (std::size_t i = 0; i < 9; ++i) {
        counts[i] = c.u32();
        if (v == 1) continue;  // v1: block sizes are inline with the data
        im.chunks[i].count2 = c.u32();
        if (v == 5 && im.v5_subversion == 1) im.chunks[i].v5sub1_unknown = c.u32();
        for (std::uint32_t j = 0; j < counts[i]; ++j) sizes[i].push_back(c.u32());
    }
    if (v == 5) {
        std::uint32_t sz = c.u32();
        im.v5_extra_struct = c.bytes(sz);
        std::uint32_t n = c.u32();
        for (std::uint32_t i = 0; i < n; ++i) im.v5_after_blocks.push_back(c.u32());
    }
    if (v == 4) {
        im.v4_unknown[0] = c.u32();
        im.v4_unknown[1] = c.u32();
    }
    for (std::size_t i = 0; i < 9; ++i)
        for (std::uint32_t j = 0; j < counts[i]; ++j)
            im.chunks[i].blocks.push_back(c.bytes(v == 1 ? c.u32() : sizes[i][j]));
    auto nrel = c.u32();
    for (std::uint32_t i = 0; i < nrel; ++i) im.relocations.push_back(c.u32());
    im.post_relocations = c.bytes(c.left());
    return im;
}

OvlImage read_image_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw OvlError("cannot open " + path.string());
    Bytes b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return read_image(b);
}

Bytes write_image(const OvlImage& im) {
    const auto v = im.version;
    if (v != 1 && v != 4 && v != 5) throw OvlError("unsupported OVL version " + std::to_string(v));
    Bytes o;
    put32(o, kOvlMagic);
    put32(o, im.headerversion);
    put32(o, v);
    if (im.headerversion == 2) put32(o, 16);
    if (v == 5) {
        put32(o, im.v5_subversion);
        if (im.v5_subversion == 1) append(o, im.header_v5sub1);
    }
    put32(o, std::uint32_t(im.references.size()));
    for (auto& r : im.references) putstr16(o, r);

    put32(o, im.loader_header_unknown);
    put32(o, std::uint32_t(im.loaders.size()));
    for (auto& l : im.loaders) {
        putstr16(o, l.loader);
        putstr16(o, l.name);
        put32(o, l.type);
        putstr16(o, l.tag);
    }
    if (v == 5) {
        if (im.v5_loader_info.size() != im.loaders.size())
            throw OvlError("v5 needs one loader-info pair per loader");
        for (auto& p : im.v5_loader_info) { put32(o, p[0]); put32(o, p[1]); }
    }
    for (auto& ch : im.chunks) {
        put32(o, std::uint32_t(ch.blocks.size()));
        if (v == 1) continue;
        put32(o, ch.count2);
        if (v == 5 && im.v5_subversion == 1) put32(o, ch.v5sub1_unknown);
        for (auto& b : ch.blocks) put32(o, std::uint32_t(b.size()));
    }
    if (v == 5) {
        put32(o, std::uint32_t(im.v5_extra_struct.size()));
        append(o, im.v5_extra_struct);
        put32(o, std::uint32_t(im.v5_after_blocks.size()));
        for (auto w : im.v5_after_blocks) put32(o, w);
    }
    if (v == 4) { put32(o, im.v4_unknown[0]); put32(o, im.v4_unknown[1]); }
    for (auto& ch : im.chunks)
        for (auto& b : ch.blocks) {
            if (v == 1) put32(o, std::uint32_t(b.size()));
            append(o, b);
        }
    put32(o, std::uint32_t(im.relocations.size()));
    for (auto r : im.relocations) put32(o, r);
    append(o, im.post_relocations);
    return o;
}

void write_image_file(const OvlImage& image, const std::filesystem::path& path) {
    auto b = write_image(image);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw OvlError("cannot write " + path.string());
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

}  // namespace ovl
