#include "ovl/extract/PeopleExtractor.hpp"

#include "ovl/BinaryReader.hpp"
#include "ovl/OvlParser.hpp"
#include "ovl/extract/GltfWriter.hpp"
#include "ovl/extract/ShapeReader.hpp"
#include "ovl/extract/TextureExtractor.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace ovl {

namespace {

// ------------------------------------------------------------ small helpers

std::optional<BinaryReader> open_at(const OvlParser& parser, std::uint32_t off) {
    auto p = parser.offset_to_position(off);
    if (!p.found) return std::nullopt;
    BinaryReader r(parser.side(p.currentOVL).ovlname);
    r.seek(p.position);
    return r;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string untagged(const std::string& s) {
    auto colon = s.rfind(':');
    return colon == std::string::npos ? s : s.substr(0, colon);
}

std::string sanitize(std::string s) {
    for (auto& c : s)
        if (c == '/' || c == '\\' || c == ':' || c == '?' || c == '*' ||
            c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    return s.empty() ? "unnamed" : s;
}

std::string js(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
    return o + "\"";
}

std::string jf(float f) {
    char b[32];
    std::snprintf(b, sizeof b, "%.7g", static_cast<double>(f));
    return b;
}

std::string pair_stem(const OvlParser& parser) {
    std::string s = std::filesystem::path(parser.side(OvlSide::Common).ovlname).filename().string();
    for (std::string suffix : {".common.ovl", ".unique.ovl", ".ovl"})
        if (s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0)
            return s.substr(0, s.size() - suffix.size());
    return s;
}

// Symbol (untagged, "" if none) that a pointer slot resolves to.
std::string ref(const OvlParser& parser, std::uint32_t slot) {
    return untagged(resolved_symbol_at(parser, slot));
}

// ------------------------------------------------------------ records (§20)

// PeoplePart (prt), 20 B: u32 (1), u32 (0), tex*, mms*, u32 palette.
// palette is the Person colour byte the part's UVs are offset by: 0 skin,
// 1 upper clothing, 2 lower clothing, 3 upper swimsuit, 4 lower swimsuit.
struct PartRec {
    std::string   symbol, texture, mesh;
    std::uint32_t unk0 = 0, unk4 = 0, palette = 0;
};

// One level of detail of a part group: {u8 count, u8 optional, u16, prt*[]}.
struct LodRec {
    std::vector<std::string> parts, optional;
};

// PeoplePartGroup (ppg), 0x54 B:
//   +0x00 u32, +0x04 u32, +0x08 u32, +0x0C u8[4], +0x10 u32 (1 for heads),
//   +0x14 ptr, +0x18 f32 LOD distance,
//   +0x1C 3 LOD records (clothes) + phd* (heads),
//   +0x38 3 LOD records (swimsuit) + phd* (heads).
struct GroupRec {
    std::string              symbol;
    std::uint32_t            unk0 = 0, unk4 = 0, unk8 = 0, unk10 = 0;
    std::uint8_t             flags[4] = {0, 0, 0, 0};
    float                    lod_distance = 0;
    LodRec                   lods[3], swim_lods[3];
    std::string              head, swim_head;
};

// PeopleHead (phd), 0xC0 B: two 4×4 matrices, u32 (1), u32 (0), tex*, sal*,
// ssk*[6] for LOD 1 and ssk*[6] for LOD 2 (neutral, Angry, Happy, Sad,
// Scared, Sick).
struct HeadRec {
    std::string              symbol, texture, animations;
    float                    m[2][16] = {};
    std::uint32_t            unk80 = 0, unk84 = 0;
    std::vector<std::string> skins[2];
};

bool read_part(const OvlParser& parser, std::uint32_t base, PartRec& p) {
    auto r = open_at(parser, base);
    if (!r) return false;
    p.unk0 = r->read_u32();
    p.unk4 = r->read_u32();
    (void)r->read_u32();
    (void)r->read_u32();
    p.palette = r->read_u32();
    p.texture = ref(parser, base + 8);
    p.mesh = ref(parser, base + 12);
    return true;
}

bool read_lod(const OvlParser& parser, std::uint32_t slot, LodRec& out) {
    auto r = open_at(parser, slot);
    if (!r) return false;
    const std::uint8_t count = r->read_u8();
    const std::uint8_t optional = r->read_u8();
    (void)r->read_u16();
    const std::uint32_t arr = r->read_u32();
    for (std::uint32_t k = 0; k < count + optional; ++k) {
        std::string s = ref(parser, arr + k * 4);
        (k < count ? out.parts : out.optional).push_back(s);
    }
    return true;
}

bool read_group(const OvlParser& parser, std::uint32_t base, GroupRec& g) {
    auto r = open_at(parser, base);
    if (!r) return false;
    g.unk0 = r->read_u32();
    g.unk4 = r->read_u32();
    g.unk8 = r->read_u32();
    for (auto& f : g.flags) f = r->read_u8();
    g.unk10 = r->read_u32();
    (void)r->read_u32();
    g.lod_distance = r->read_f32();
    for (int k = 0; k < 3; ++k) {
        if (!read_lod(parser, base + 0x1C + k * 8, g.lods[k])) return false;
        if (!read_lod(parser, base + 0x38 + k * 8, g.swim_lods[k])) return false;
    }
    g.head = ref(parser, base + 0x34);
    g.swim_head = ref(parser, base + 0x50);
    return true;
}

bool read_head(const OvlParser& parser, std::uint32_t base, HeadRec& h) {
    auto r = open_at(parser, base);
    if (!r) return false;
    for (auto& m : h.m)
        for (float& f : m) f = r->read_f32();
    h.unk80 = r->read_u32();
    h.unk84 = r->read_u32();
    h.texture = ref(parser, base + 0x88);
    h.animations = ref(parser, base + 0x8C);
    for (int lod = 0; lod < 2; ++lod)
        for (int k = 0; k < 6; ++k)
            h.skins[lod].push_back(ref(parser, base + 0x90 + (lod * 6 + k) * 4));
    return true;
}

// ------------------------------------------------------------ glTF output

// Every `tex` of the pair, decoded once and turned upright (docs §4.3).
class Textures {
public:
    explicit Textures(const OvlParser& parser) : parser_(parser) {}
    int image(gltf::Scene& sc, const std::string& tex) {
        if (tex.empty()) return -1;
        auto ref = find_linked(parser_, tex + ":tex");
        if (!ref) return -1;
        gltf::Image im;
        std::vector<std::uint8_t> bgra;
        if (!TextureExtractor::decode_tex_bgra(parser_, ref->side, ref->index,
                                               im.width, im.height, bgra))
            return -1;
        TextureExtractor::flip_rows(im.width, im.height, bgra);
        im.name = tex;
        im.rgba.resize(bgra.size());
        for (std::size_t i = 0; i + 3 < bgra.size(); i += 4) {
            im.rgba[i] = bgra[i + 2];
            im.rgba[i + 1] = bgra[i + 1];
            im.rgba[i + 2] = bgra[i];
            im.rgba[i + 3] = 255;
        }
        sc.images.push_back(std::move(im));
        return static_cast<int>(sc.images.size()) - 1;
    }
private:
    const OvlParser& parser_;
};

// One-mesh .glb. Positions mirrored on Z (Direct3D → glTF, as SceneryExtractor
// does); V turned to the top-down convention of the upright image.
bool write_mesh_glb(const std::filesystem::path& path, const std::string& name,
                    const std::vector<ShapeVertex>& verts,
                    const std::vector<std::uint32_t>& indices,
                    int image_tex, gltf::Scene sc, const std::string& extras,
                    std::string& err) {
    sc.name = name;
    gltf::Material mat;
    mat.name = name;
    mat.image = image_tex;
    mat.extras_json = extras;
    sc.materials.push_back(mat);
    gltf::Primitive p;
    p.material = 0;
    for (const auto& v : verts) {
        p.positions.insert(p.positions.end(), {v.pos[0], v.pos[1], -v.pos[2]});
        p.normals.insert(p.normals.end(), {v.normal[0], v.normal[1], -v.normal[2]});
        p.uvs.insert(p.uvs.end(), {v.uv[0], 1.0f - v.uv[1]});
    }
    // Mirroring flips the winding: swap two corners of each triangle.
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3)
        p.indices.insert(p.indices.end(), {indices[t], indices[t + 2], indices[t + 1]});
    gltf::Mesh mesh;
    mesh.name = name;
    mesh.primitives.push_back(std::move(p));
    sc.meshes.push_back(std::move(mesh));
    gltf::Node node;
    node.name = name;
    node.mesh = 0;
    sc.nodes.push_back(node);
    return gltf::write_glb(path, sc, err);
}

}  // namespace

ExtractResult PeopleExtractor::extract(const OvlParser& parser, const ExtractContext& ctx) {
    ExtractResult res{};
    std::vector<PartRec> parts;
    std::vector<GroupRec> groups;
    std::vector<HeadRec> heads;
    std::vector<MorphMesh> morphs;
    std::vector<SimpleSkin> skins;
    std::map<std::string, std::string> mesh_texture;   // mesh/ssk → tex (lower-case key)

    for (std::size_t s = 0; s < (parser.has_unique() ? 2u : 1u); ++s) {
        auto side = static_cast<OvlSide>(s);
        const auto& d = parser.side(side);
        for (std::size_t i = 0; i < d.linkedfiles.size(); ++i) {
            const std::string tag =
                parser.loader_by_id(d.linkedfiles[i].loaderreference.loadernumber, side).tag;
            const std::uint32_t base = d.linkedfiles[i].loaderreference.datapointer;
            const std::string sym = linked_symbol(parser, side, i, false);
            std::string err;
            bool ok = true;
            if (tag == "prt") {
                PartRec p; p.symbol = sym;
                ok = read_part(parser, base, p);
                if (ok) parts.push_back(p);
            } else if (tag == "ppg") {
                GroupRec g; g.symbol = sym;
                ok = read_group(parser, base, g);
                if (ok) groups.push_back(g);
            } else if (tag == "phd") {
                HeadRec h; h.symbol = sym;
                ok = read_head(parser, base, h);
                if (ok) heads.push_back(h);
            } else if (tag == "mms") {
                MorphMesh m;
                ok = read_morph_mesh(parser, side, i, m, err);
                if (ok) morphs.push_back(std::move(m));
            } else if (tag == "ssk") {
                SimpleSkin k;
                ok = read_simple_skin(parser, side, i, k, err);
                if (ok) skins.push_back(std::move(k));
            } else {
                continue;
            }
            if (!ok) {
                ++res.errors;
                ctx.log(tag + " " + sym + ": " + (err.empty() ? "unreadable" : err));
            }
        }
    }
    if (parts.empty() && groups.empty() && heads.empty() && morphs.empty() && skins.empty())
        return res;

    for (const auto& p : parts)
        if (!p.mesh.empty()) mesh_texture.emplace(lower(p.mesh), p.texture);
    for (const auto& h : heads)
        for (const auto& lod : h.skins)
            for (const auto& k : lod)
                if (!k.empty()) mesh_texture.emplace(lower(k), h.texture);

    std::filesystem::create_directories(ctx.output_dir);
    const std::string stem = pair_stem(parser);

    // ---- JSON sidecar
    std::ostringstream o;
    o << "{\n  \"ovl\": " << js(stem) << ",\n  \"parts\": [";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto& p = parts[i];
        o << (i ? "," : "") << "\n    {\"symbol\": " << js(p.symbol)
          << ", \"texture\": " << js(p.texture) << ", \"mesh\": " << js(p.mesh)
          << ", \"palette\": " << p.palette << ", \"unk0\": " << p.unk0
          << ", \"unk4\": " << p.unk4 << "}";
    }
    o << "\n  ],\n  \"part_groups\": [";
    auto lods_json = [&](const LodRec (&lods)[3]) {
        std::string s = "[";
        for (int k = 0; k < 3; ++k) {
            s += k ? ", " : "";
            s += "{\"parts\": [";
            for (std::size_t j = 0; j < lods[k].parts.size(); ++j)
                s += (j ? ", " : "") + js(lods[k].parts[j]);
            s += "], \"optional\": [";
            for (std::size_t j = 0; j < lods[k].optional.size(); ++j)
                s += (j ? ", " : "") + js(lods[k].optional[j]);
            s += "]}";
        }
        return s + "]";
    };
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const auto& g = groups[i];
        o << (i ? "," : "") << "\n    {\"symbol\": " << js(g.symbol)
          << ", \"lod_distance\": " << jf(g.lod_distance)
          << ", \"flags\": [" << int(g.flags[0]) << ", " << int(g.flags[1]) << ", "
          << int(g.flags[2]) << ", " << int(g.flags[3]) << "]"
          << ", \"unk0\": " << g.unk0 << ", \"unk4\": " << g.unk4
          << ", \"unk8\": " << g.unk8 << ", \"unk10\": " << g.unk10
          << ",\n     \"lods\": " << lods_json(g.lods)
          << ",\n     \"swim_lods\": " << lods_json(g.swim_lods)
          << ",\n     \"head\": " << js(g.head) << ", \"swim_head\": " << js(g.swim_head) << "}";
    }
    o << "\n  ],\n  \"heads\": [";
    for (std::size_t i = 0; i < heads.size(); ++i) {
        const auto& h = heads[i];
        o << (i ? "," : "") << "\n    {\"symbol\": " << js(h.symbol)
          << ", \"texture\": " << js(h.texture) << ", \"animations\": " << js(h.animations);
        for (int m = 0; m < 2; ++m) {
            o << ", \"matrix" << m << "\": [";
            for (int k = 0; k < 16; ++k) o << (k ? ", " : "") << jf(h.m[m][k]);
            o << "]";
        }
        o << ", \"unk80\": " << h.unk80 << ", \"unk84\": " << h.unk84;
        for (int lod = 0; lod < 2; ++lod) {
            o << ", \"skins_lod" << lod + 1 << "\": [";
            for (std::size_t k = 0; k < h.skins[lod].size(); ++k)
                o << (k ? ", " : "") << js(h.skins[lod][k]);
            o << "]";
        }
        o << "}";
    }
    o << "\n  ],\n  \"morph_meshes\": [";
    for (std::size_t i = 0; i < morphs.size(); ++i) {
        const auto& m = morphs[i];
        o << (i ? "," : "") << "\n    {\"symbol\": " << js(m.symbol)
          << ", \"vertices\": " << m.verts.size() << ", \"triangles\": " << m.indices.size() / 3
          << ", \"positions\": " << m.position_count << ", \"normals\": " << m.normal_count
          << ", \"type_flag\": " << m.type_flag << ", \"animations\": [";
        for (std::size_t k = 0; k < m.morphs.size(); ++k) {
            const auto& mo = m.morphs[k];
            o << (k ? ", " : "") << "[" << js(mo.name) << ", " << mo.times.size() << ", "
              << jf(mo.times.empty() ? 0.f : mo.times.back()) << "]";
        }
        o << "]}";
    }
    o << "\n  ],\n  \"simple_skins\": [";
    for (std::size_t i = 0; i < skins.size(); ++i) {
        const auto& k = skins[i];
        o << (i ? "," : "") << "\n    {\"symbol\": " << js(k.symbol)
          << ", \"vertices\": " << k.verts.size() << ", \"triangles\": " << k.indices.size() / 3
          << ", \"bones\": [";
        for (std::size_t b = 0; b < k.bones.size(); ++b)
            o << (b ? ", " : "") << "[" << js(k.bones[b].name) << ", " << k.bones[b].vertex_count << "]";
        o << "]}";
    }
    o << "\n  ]\n}\n";
    {
        auto path = ctx.output_dir / (stem + ".people.json");
        if (ctx.overwrite || !std::filesystem::exists(path)) {
            std::ofstream(path) << o.str();
            ++res.files_written;
        }
    }

    // ---- one .glb per mesh
    Textures textures(parser);
    auto texture_of = [&](const std::string& mesh) {
        auto it = mesh_texture.find(lower(mesh));
        return it == mesh_texture.end() ? std::string() : it->second;
    };
    auto emit = [&](const std::string& symbol, const std::vector<ShapeVertex>& verts,
                    const std::vector<std::uint32_t>& indices, const std::string& extras) {
        auto path = ctx.output_dir / (sanitize(symbol) + ".glb");
        if (!ctx.overwrite && std::filesystem::exists(path)) return;
        gltf::Scene sc;
        const std::string tex = texture_of(symbol);
        int img = textures.image(sc, tex);
        std::string err;
        std::string ex = "{\"texture\": " + js(tex) + (extras.empty() ? "" : ", " + extras) + "}";
        if (write_mesh_glb(path, symbol, verts, indices, img, std::move(sc), ex, err)) {
            ++res.files_written;
            ctx.log("wrote " + path.filename().string());
        } else {
            ++res.errors;
            ctx.log(symbol + ": " + err);
        }
    };
    for (const auto& m : morphs) {
        if (m.morphs.empty()) continue;
        int morph = find_morph(m, "IdleStandard");
        if (morph < 0) morph = 0;
        std::vector<ShapeVertex> verts;
        std::string err;
        if (!morph_pose(parser, m, static_cast<std::size_t>(morph), 0, verts, err)) {
            ++res.errors;
            ctx.log(m.symbol + ": " + err);
            continue;
        }
        emit(m.symbol, verts, m.indices, "\"pose\": " + js(m.morphs[morph].name));
    }
    for (const auto& k : skins) {
        std::string bones = "\"bones\": [";
        for (std::size_t b = 0; b < k.bones.size(); ++b)
            bones += (b ? ", " : "") + js(k.bones[b].name);
        emit(k.symbol, k.verts, k.indices, bones + "], \"space\": \"bone-local\"");
    }
    return res;
}

}  // namespace ovl
