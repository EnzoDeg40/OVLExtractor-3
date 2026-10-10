#include "ovl/extract/GltfWriter.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace ovl::gltf {

namespace {

// ---------------------------------------------------------------- PNG / zlib

std::uint32_t crc32(const std::uint8_t* p, std::size_t n, std::uint32_t c = 0) {
    static std::uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t k = i;
            for (int j = 0; j < 8; ++j) k = (k & 1) ? 0xEDB88320u ^ (k >> 1) : k >> 1;
            table[i] = k;
        }
        init = true;
    }
    c = ~c;
    for (std::size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

// LSB-first bit writer for deflate.
struct BitWriter {
    std::vector<std::uint8_t>& out;
    std::uint32_t acc = 0;
    int nbits = 0;
    void put(std::uint32_t bits, int n) {          // bits LSB-first
        acc |= bits << nbits;
        nbits += n;
        while (nbits >= 8) { out.push_back(static_cast<std::uint8_t>(acc)); acc >>= 8; nbits -= 8; }
    }
    void put_huff(std::uint32_t code, int n) {     // Huffman code, MSB-first
        std::uint32_t rev = 0;
        for (int i = 0; i < n; ++i) rev |= ((code >> i) & 1u) << (n - 1 - i);
        put(rev, n);
    }
    void flush() { if (nbits > 0) out.push_back(static_cast<std::uint8_t>(acc)); acc = 0; nbits = 0; }
};

void put_literal(BitWriter& bw, int lit) {
    if (lit < 144)      bw.put_huff(0x30 + lit, 8);
    else if (lit < 256) bw.put_huff(0x190 + (lit - 144), 9);
    else if (lit < 280) bw.put_huff(lit - 256, 7);
    else                bw.put_huff(0xC0 + (lit - 280), 8);
}

const int kLenBase[29]  = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
const int kLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const int kDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const int kDistExtra[30]= {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

void put_match(BitWriter& bw, int len, int dist) {
    int li = 28;
    while (kLenBase[li] > len) --li;
    put_literal(bw, 257 + li);
    if (kLenExtra[li]) bw.put(static_cast<std::uint32_t>(len - kLenBase[li]), kLenExtra[li]);
    int di = 29;
    while (kDistBase[di] > dist) --di;
    bw.put_huff(static_cast<std::uint32_t>(di), 5);
    if (kDistExtra[di]) bw.put(static_cast<std::uint32_t>(dist - kDistBase[di]), kDistExtra[di]);
}

// zlib stream: one fixed-Huffman deflate block with greedy hash-chain LZ77.
std::vector<std::uint8_t> zlib_compress(const std::vector<std::uint8_t>& in) {
    std::vector<std::uint8_t> out = {0x78, 0x01};
    BitWriter bw{out};
    bw.put(1, 1);   // BFINAL
    bw.put(1, 2);   // BTYPE = fixed Huffman
    constexpr int kWindow = 32768, kHashBits = 15, kMaxChain = 32;
    std::vector<int> head(1 << kHashBits, -1), prev(in.size(), -1);
    auto hash = [&](std::size_t i) {
        return ((in[i] << 10) ^ (in[i + 1] << 5) ^ in[i + 2]) & ((1 << kHashBits) - 1);
    };
    std::size_t i = 0;
    const std::size_t n = in.size();
    auto insert = [&](std::size_t p) {
        if (p + 2 >= n) return;
        int h = hash(p);
        prev[p] = head[h];
        head[h] = static_cast<int>(p);
    };
    while (i < n) {
        int best_len = 0, best_dist = 0;
        if (i + 2 < n) {
            int cand = head[hash(i)];
            for (int chain = 0; cand >= 0 && chain < kMaxChain; ++chain) {
                int dist = static_cast<int>(i) - cand;
                if (dist > kWindow) break;
                int len = 0;
                int max_len = static_cast<int>(std::min<std::size_t>(258, n - i));
                while (len < max_len && in[cand + len] == in[i + len]) ++len;
                if (len > best_len) { best_len = len; best_dist = dist; if (len == 258) break; }
                cand = prev[cand];
            }
        }
        if (best_len >= 3) {
            put_match(bw, best_len, best_dist);
            for (int k = 0; k < best_len; ++k) insert(i + k);
            i += best_len;
        } else {
            put_literal(bw, in[i]);
            insert(i);
            ++i;
        }
    }
    put_literal(bw, 256);
    bw.flush();
    std::uint32_t a = 1, b = 0;
    for (auto c : in) { a = (a + c) % 65521; b = (b + a) % 65521; }
    std::uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::uint8_t>(adler >> s));
    return out;
}

void put_be32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<std::uint8_t>(x >> s));
}

void png_chunk(std::vector<std::uint8_t>& png, const char* type,
               const std::vector<std::uint8_t>& data) {
    put_be32(png, static_cast<std::uint32_t>(data.size()));
    std::size_t start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    put_be32(png, crc32(png.data() + start, png.size() - start));
}

// -------------------------------------------------------------------- JSON

std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (static_cast<unsigned char>(c) < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            o += buf;
        } else o += c;
    }
    return o + "\"";
}

std::string jnum(float f) {
    if (!std::isfinite(f)) return "0";
    std::ostringstream os;
    os << std::setprecision(9) << f;
    return os.str();
}

template <class It>
std::string jarr(It b, It e) {
    std::string o = "[";
    for (It it = b; it != e; ++it) { if (it != b) o += ","; o += jnum(*it); }
    return o + "]";
}

// Binary buffer builder: one buffer, one bufferView per accessor/image.
struct Bin {
    std::vector<std::uint8_t> data;
    std::vector<std::string>  views;      // JSON for each bufferView
    std::vector<std::string>  accessors;  // JSON for each accessor

    int add_view(const void* p, std::size_t n, int target) {
        while (data.size() % 4) data.push_back(0);
        std::size_t off = data.size();
        const auto* b = static_cast<const std::uint8_t*>(p);
        data.insert(data.end(), b, b + n);
        std::string v = "{\"buffer\":0,\"byteOffset\":" + std::to_string(off) +
                        ",\"byteLength\":" + std::to_string(n);
        if (target) v += ",\"target\":" + std::to_string(target);
        views.push_back(v + "}");
        return static_cast<int>(views.size()) - 1;
    }

    // Float accessor of `comps` components (VEC2/VEC3/VEC4/SCALAR/MAT4).
    int add_floats(const std::vector<float>& v, int comps, const char* type,
                   int target, bool minmax) {
        int view = add_view(v.data(), v.size() * sizeof(float), target);
        std::size_t count = v.size() / comps;
        std::string a = "{\"bufferView\":" + std::to_string(view) +
                        ",\"componentType\":5126,\"count\":" + std::to_string(count) +
                        ",\"type\":\"" + type + "\"";
        if (minmax && count) {
            std::vector<float> mn(comps, std::numeric_limits<float>::max());
            std::vector<float> mx(comps, std::numeric_limits<float>::lowest());
            for (std::size_t i = 0; i < count; ++i)
                for (int c = 0; c < comps; ++c) {
                    mn[c] = std::min(mn[c], v[i * comps + c]);
                    mx[c] = std::max(mx[c], v[i * comps + c]);
                }
            a += ",\"min\":" + jarr(mn.begin(), mn.end()) + ",\"max\":" + jarr(mx.begin(), mx.end());
        }
        accessors.push_back(a + "}");
        return static_cast<int>(accessors.size()) - 1;
    }

    int add_u16(const std::vector<std::uint16_t>& v, int comps, const char* type, int target) {
        int view = add_view(v.data(), v.size() * 2, target);
        accessors.push_back("{\"bufferView\":" + std::to_string(view) +
                            ",\"componentType\":5123,\"count\":" + std::to_string(v.size() / comps) +
                            ",\"type\":\"" + type + "\"}");
        return static_cast<int>(accessors.size()) - 1;
    }

    int add_indices(const std::vector<std::uint32_t>& v) {
        int view = add_view(v.data(), v.size() * 4, 34963);
        accessors.push_back("{\"bufferView\":" + std::to_string(view) +
                            ",\"componentType\":5125,\"count\":" + std::to_string(v.size()) +
                            ",\"type\":\"SCALAR\"}");
        return static_cast<int>(accessors.size()) - 1;
    }
};

template <class T>
std::string join(const std::vector<T>& v) {
    std::string o;
    for (std::size_t i = 0; i < v.size(); ++i) { if (i) o += ","; o += v[i]; }
    return o;
}

}  // namespace

std::vector<std::uint8_t> encode_png(std::uint32_t width, std::uint32_t height,
                                     const std::vector<std::uint8_t>& rgba) {
    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::uint8_t> ihdr;
    put_be32(ihdr, width);
    put_be32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});   // 8-bit RGBA, no interlace
    png_chunk(png, "IHDR", ihdr);
    std::vector<std::uint8_t> raw;
    raw.reserve((static_cast<std::size_t>(width) * 4 + 1) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);   // filter: none
        const auto* row = rgba.data() + static_cast<std::size_t>(y) * width * 4;
        raw.insert(raw.end(), row, row + static_cast<std::size_t>(width) * 4);
    }
    png_chunk(png, "IDAT", zlib_compress(raw));
    png_chunk(png, "IEND", {});
    return png;
}

bool write_glb(const std::filesystem::path& path, const Scene& sc, std::string& err) {
    Bin bin;
    std::vector<std::string> images, textures, materials, meshes, nodes, skins, anims;

    for (std::size_t i = 0; i < sc.images.size(); ++i) {
        const auto& im = sc.images[i];
        auto png = encode_png(im.width, im.height, im.rgba);
        int view = bin.add_view(png.data(), png.size(), 0);
        images.push_back("{\"name\":" + jstr(im.name) + ",\"mimeType\":\"image/png\",\"bufferView\":" +
                         std::to_string(view) + "}");
        textures.push_back("{\"sampler\":0,\"source\":" + std::to_string(i) + "}");
    }

    for (const auto& m : sc.materials) {
        std::string pbr = "{\"baseColorFactor\":" + jarr(m.base_color.begin(), m.base_color.end()) +
                          ",\"metallicFactor\":" + jnum(m.metallic) +
                          ",\"roughnessFactor\":" + jnum(m.roughness);
        if (m.image >= 0) pbr += ",\"baseColorTexture\":{\"index\":" + std::to_string(m.image) + "}";
        pbr += "}";
        std::string j = "{\"name\":" + jstr(m.name) + ",\"pbrMetallicRoughness\":" + pbr;
        if (m.alpha_mode == AlphaMode::Mask)
            j += ",\"alphaMode\":\"MASK\",\"alphaCutoff\":" + jnum(m.alpha_cutoff);
        else if (m.alpha_mode == AlphaMode::Blend)
            j += ",\"alphaMode\":\"BLEND\"";
        if (m.double_sided) j += ",\"doubleSided\":true";
        if (!m.extras_json.empty()) j += ",\"extras\":" + m.extras_json;
        materials.push_back(j + "}");
    }

    for (const auto& me : sc.meshes) {
        std::vector<std::string> prims;
        for (const auto& p : me.primitives) {
            std::string attrs = "\"POSITION\":" +
                std::to_string(bin.add_floats(p.positions, 3, "VEC3", 34962, true));
            if (!p.normals.empty())
                attrs += ",\"NORMAL\":" + std::to_string(bin.add_floats(p.normals, 3, "VEC3", 34962, false));
            if (!p.uvs.empty())
                attrs += ",\"TEXCOORD_0\":" + std::to_string(bin.add_floats(p.uvs, 2, "VEC2", 34962, false));
            if (!p.joints.empty()) {
                attrs += ",\"JOINTS_0\":" + std::to_string(bin.add_u16(p.joints, 4, "VEC4", 34962));
                attrs += ",\"WEIGHTS_0\":" + std::to_string(bin.add_floats(p.weights, 4, "VEC4", 34962, false));
            }
            std::string pj = "{\"attributes\":{" + attrs + "},\"indices\":" +
                             std::to_string(bin.add_indices(p.indices)) + ",\"mode\":4";
            if (p.material >= 0) pj += ",\"material\":" + std::to_string(p.material);
            prims.push_back(pj + "}");
        }
        meshes.push_back("{\"name\":" + jstr(me.name) + ",\"primitives\":[" + join(prims) + "]}");
    }

    std::vector<std::vector<int>> children(sc.nodes.size());
    std::vector<int> roots;
    for (std::size_t i = 0; i < sc.nodes.size(); ++i) {
        int p = sc.nodes[i].parent;
        if (p >= 0 && p < static_cast<int>(sc.nodes.size())) children[p].push_back(static_cast<int>(i));
        else roots.push_back(static_cast<int>(i));
    }
    for (std::size_t i = 0; i < sc.nodes.size(); ++i) {
        const auto& n = sc.nodes[i];
        std::string j = "{\"name\":" + jstr(n.name);
        if (n.translation != std::array<float, 3>{0, 0, 0})
            j += ",\"translation\":" + jarr(n.translation.begin(), n.translation.end());
        if (n.rotation != std::array<float, 4>{0, 0, 0, 1})
            j += ",\"rotation\":" + jarr(n.rotation.begin(), n.rotation.end());
        if (n.scale != std::array<float, 3>{1, 1, 1})
            j += ",\"scale\":" + jarr(n.scale.begin(), n.scale.end());
        if (n.mesh >= 0) j += ",\"mesh\":" + std::to_string(n.mesh);
        if (n.skin >= 0) j += ",\"skin\":" + std::to_string(n.skin);
        if (!children[i].empty()) {
            std::vector<std::string> c;
            for (int k : children[i]) c.push_back(std::to_string(k));
            j += ",\"children\":[" + join(c) + "]";
        }
        if (!n.extras_json.empty()) j += ",\"extras\":" + n.extras_json;
        nodes.push_back(j + "}");
    }

    for (const auto& s : sc.skins) {
        std::vector<float> ibm;
        for (const auto& m : s.inverse_bind) ibm.insert(ibm.end(), m.begin(), m.end());
        int acc = bin.add_floats(ibm, 16, "MAT4", 0, false);
        std::vector<std::string> jn;
        for (int k : s.joints) jn.push_back(std::to_string(k));
        skins.push_back("{\"name\":" + jstr(s.name) + ",\"inverseBindMatrices\":" +
                        std::to_string(acc) + ",\"joints\":[" + join(jn) + "]}");
    }

    for (const auto& a : sc.animations) {
        std::vector<std::string> samplers, channels;
        for (const auto& c : a.channels) {
            if (c.times.empty()) continue;
            int in = bin.add_floats(c.times, 1, "SCALAR", 0, true);
            int out = bin.add_floats(c.values, c.rotation ? 4 : 3, c.rotation ? "VEC4" : "VEC3", 0, false);
            samplers.push_back("{\"input\":" + std::to_string(in) + ",\"output\":" +
                               std::to_string(out) + ",\"interpolation\":\"LINEAR\"}");
            channels.push_back("{\"sampler\":" + std::to_string(samplers.size() - 1) +
                               ",\"target\":{\"node\":" + std::to_string(c.node) +
                               ",\"path\":\"" + (c.rotation ? "rotation" : "translation") + "\"}}");
        }
        if (channels.empty()) continue;
        anims.push_back("{\"name\":" + jstr(a.name) + ",\"samplers\":[" + join(samplers) +
                        "],\"channels\":[" + join(channels) + "]}");
    }

    while (bin.data.size() % 4) bin.data.push_back(0);
    std::vector<std::string> root_ids;
    for (int r : roots) root_ids.push_back(std::to_string(r));

    std::string json = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"OVLExtractor-3\"}";
    json += ",\"scene\":0,\"scenes\":[{\"name\":" + jstr(sc.name) + ",\"nodes\":[" + join(root_ids) + "]";
    if (!sc.extras_json.empty()) json += ",\"extras\":" + sc.extras_json;
    json += "}]";
    json += ",\"nodes\":[" + join(nodes) + "]";
    if (!meshes.empty())    json += ",\"meshes\":[" + join(meshes) + "]";
    if (!materials.empty()) json += ",\"materials\":[" + join(materials) + "]";
    if (!images.empty()) {
        // 10497 = REPEAT: RCT3 UVs tile freely. 9987 = LINEAR_MIPMAP_LINEAR.
        json += ",\"samplers\":[{\"magFilter\":9729,\"minFilter\":9987,\"wrapS\":10497,\"wrapT\":10497}]";
        json += ",\"images\":[" + join(images) + "],\"textures\":[" + join(textures) + "]";
    }
    if (!skins.empty())     json += ",\"skins\":[" + join(skins) + "]";
    if (!anims.empty())     json += ",\"animations\":[" + join(anims) + "]";
    json += ",\"accessors\":[" + join(bin.accessors) + "]";
    json += ",\"bufferViews\":[" + join(bin.views) + "]";
    json += ",\"buffers\":[{\"byteLength\":" + std::to_string(bin.data.size()) + "}]}";
    while (json.size() % 4) json += ' ';

    std::ofstream o(path, std::ios::binary);
    if (!o) { err = "cannot open " + path.string(); return false; }
    auto w32 = [&](std::uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); };
    std::uint32_t total = 12 + 8 + static_cast<std::uint32_t>(json.size()) + 8 +
                          static_cast<std::uint32_t>(bin.data.size());
    w32(0x46546C67u); w32(2); w32(total);                 // "glTF", version, length
    w32(static_cast<std::uint32_t>(json.size())); w32(0x4E4F534Au);   // "JSON"
    o.write(json.data(), static_cast<std::streamsize>(json.size()));
    w32(static_cast<std::uint32_t>(bin.data.size())); w32(0x004E4942u); // "BIN\0"
    o.write(reinterpret_cast<const char*>(bin.data.data()),
            static_cast<std::streamsize>(bin.data.size()));
    return static_cast<bool>(o);
}

}  // namespace ovl::gltf
