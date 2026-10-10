#include "ovl/extract/GltfWriter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ovl;

namespace {

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
           (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
}

std::uint32_t crc32(const std::uint8_t* p, std::size_t n) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return ~c;
}

// Minimal inflater for the single fixed-Huffman block encode_png emits —
// enough to round-trip the encoder without a zlib dependency.
struct BitReader {
    const std::uint8_t* p;
    std::size_t pos = 0;
    std::uint32_t bit() { std::uint32_t b = (p[pos >> 3] >> (pos & 7)) & 1u; ++pos; return b; }
    std::uint32_t bits(int n) { std::uint32_t v = 0; for (int i = 0; i < n; ++i) v |= bit() << i; return v; }
};

int fixed_symbol(BitReader& br) {
    std::uint32_t code = 0;
    for (int len = 1; len <= 9; ++len) {
        code = (code << 1) | br.bit();
        if (len == 7 && code <= 0x17) return static_cast<int>(256 + code);
        if (len == 8 && code >= 0x30 && code <= 0xBF) return static_cast<int>(code - 0x30);
        if (len == 8 && code >= 0xC0 && code <= 0xC7) return static_cast<int>(280 + code - 0xC0);
        if (len == 9 && code >= 0x190) return static_cast<int>(144 + code - 0x190);
    }
    return -1;
}

std::vector<std::uint8_t> inflate_fixed(const std::uint8_t* data) {
    static const int lb[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const int le[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const int db[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const int de[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    BitReader br{data};
    std::vector<std::uint8_t> out;
    REQUIRE(br.bits(1) == 1);   // BFINAL
    REQUIRE(br.bits(2) == 1);   // fixed Huffman
    for (;;) {
        int s = fixed_symbol(br);
        REQUIRE(s >= 0);
        if (s < 256) { out.push_back(static_cast<std::uint8_t>(s)); continue; }
        if (s == 256) break;
        int li = s - 257;
        int len = lb[li] + static_cast<int>(br.bits(le[li]));
        std::uint32_t dcode = 0;
        for (int i = 0; i < 5; ++i) dcode = (dcode << 1) | br.bit();
        int dist = db[dcode] + static_cast<int>(br.bits(de[dcode]));
        REQUIRE(static_cast<std::size_t>(dist) <= out.size());
        for (int i = 0; i < len; ++i) out.push_back(out[out.size() - static_cast<std::size_t>(dist)]);
    }
    return out;
}

}  // namespace

TEST_CASE("encode_png writes valid chunks that round-trip", "[gltf][png]") {
    const std::uint32_t w = 37, h = 23;   // odd sizes, repetitive + noisy rows
    std::vector<std::uint8_t> rgba(w * h * 4);
    for (std::size_t i = 0; i < rgba.size(); ++i)
        rgba[i] = static_cast<std::uint8_t>((i / 4) % 7 == 0 ? (i * 131) & 0xFF : (i % 4) * 60);
    auto png = gltf::encode_png(w, h, rgba);

    const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    REQUIRE(std::memcmp(png.data(), sig, 8) == 0);

    std::vector<std::uint8_t> idat;
    std::size_t at = 8;
    bool saw_iend = false;
    while (at + 12 <= png.size()) {
        std::uint32_t len = be32(&png[at]);
        std::string type(reinterpret_cast<const char*>(&png[at + 4]), 4);
        REQUIRE(be32(&png[at + 8 + len]) == crc32(&png[at + 4], len + 4));
        if (type == "IHDR") {
            CHECK(be32(&png[at + 8]) == w);
            CHECK(be32(&png[at + 12]) == h);
            CHECK(png[at + 16] == 8);   // bit depth
            CHECK(png[at + 17] == 6);   // RGBA
        }
        if (type == "IDAT") idat.assign(&png[at + 8], &png[at + 8 + len]);
        if (type == "IEND") saw_iend = true;
        at += 12 + len;
    }
    REQUIRE(saw_iend);
    REQUIRE(idat.size() > 6);
    CHECK(idat[0] == 0x78);
    CHECK(((idat[0] << 8) | idat[1]) % 31 == 0);   // zlib header check bits

    auto raw = inflate_fixed(idat.data() + 2);
    REQUIRE(raw.size() == (w * 4 + 1) * h);
    for (std::uint32_t y = 0; y < h; ++y) {
        CHECK(raw[y * (w * 4 + 1)] == 0);   // filter type none
        CHECK(std::memcmp(&raw[y * (w * 4 + 1) + 1], &rgba[y * w * 4], w * 4) == 0);
    }
}

TEST_CASE("write_glb produces a well-formed GLB container", "[gltf]") {
    gltf::Scene sc;
    sc.name = "tri";
    gltf::Mesh m;
    m.name = "tri";
    gltf::Primitive p;
    p.positions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    p.indices = {0, 1, 2};
    m.primitives.push_back(p);
    sc.meshes.push_back(m);
    gltf::Node n;
    n.name = "tri";
    n.mesh = 0;
    sc.nodes.push_back(n);

    auto path = std::filesystem::temp_directory_path() / "ovl_test_tri.glb";
    std::string err;
    REQUIRE(gltf::write_glb(path, sc, err));
    std::ifstream f(path, std::ios::binary);
    std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(f)), {});
    std::filesystem::remove(path);

    auto le32 = [&](std::size_t o) {
        return std::uint32_t{b[o]} | (std::uint32_t{b[o + 1]} << 8) |
               (std::uint32_t{b[o + 2]} << 16) | (std::uint32_t{b[o + 3]} << 24);
    };
    REQUIRE(b.size() >= 28);
    CHECK(le32(0) == 0x46546C67u);   // "glTF"
    CHECK(le32(4) == 2);
    CHECK(le32(8) == b.size());
    std::uint32_t json_len = le32(12);
    CHECK(le32(16) == 0x4E4F534Au);   // "JSON"
    CHECK(json_len % 4 == 0);
    std::string json(reinterpret_cast<const char*>(&b[20]), json_len);
    CHECK(json.find("\"version\":\"2.0\"") != std::string::npos);
    CHECK(json.find("\"POSITION\"") != std::string::npos);
    std::size_t bin = 20 + json_len;
    CHECK(le32(bin + 4) == 0x004E4942u);   // "BIN\0"
    CHECK(le32(bin) % 4 == 0);
    CHECK(bin + 8 + le32(bin) == b.size());
}
