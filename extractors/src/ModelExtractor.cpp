#include "ovl/extract/ModelExtractor.hpp"

#include "ovl/BinaryReader.hpp"
#include "ovl/Error.hpp"
#include "ovl/OvlParser.hpp"
#include "ovl/extract/ShapeReader.hpp"
#include "ovl/extract/TextureExtractor.hpp"
#include "ovl/extract/TextureIndex.hpp"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ovl {

namespace {

std::string sanitize(std::string s) {
    for (auto& c : s) {
        if (c == '/' || c == '\\' || c == ':' || c == '?' || c == '*' ||
            c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    }
    if (s.empty()) s = "unnamed";
    return s;
}


struct Vertex { float x=0, y=0, z=0, nx=0, ny=0, nz=0, u=0, v=0; };

// mms (MorphMesh, docs §7) → OBJ in one pose: "IdleStandard" keyframe 0 when
// the mesh has it (people), else the first morph's first keyframe.
bool process_mms(const OvlParser& parser,
                 OvlSide side,
                 std::size_t lf_index,
                 const std::filesystem::path& out_dir,
                 bool overwrite,
                 const ExtractContext& ctx) {
    MorphMesh mesh;
    std::string err;
    if (!read_morph_mesh(parser, side, lf_index, mesh, err)) {
        ctx.log("mms " + linked_symbol(parser, side, lf_index) + ": " + err);
        return false;
    }
    const std::string base = sanitize(mesh.symbol);
    const auto out_path = out_dir / (base + ".obj");
    if (!overwrite && std::filesystem::exists(out_path)) {
        ctx.log("skip (exists): " + base);
        return true;
    }
    if (mesh.morphs.empty()) {
        ctx.log("mms " + mesh.symbol + ": no morph, no positions");
        return false;
    }
    int morph = find_morph(mesh, "IdleStandard");
    if (morph < 0) morph = 0;
    std::vector<ShapeVertex> vs;
    if (!morph_pose(parser, mesh, static_cast<std::size_t>(morph), 0, vs, err)) {
        ctx.log("mms " + mesh.symbol + ": " + err);
        return false;
    }
    std::filesystem::create_directories(out_dir);
    std::ofstream o(out_path);
    if (!o) return false;
    o << "# RCT3 OVL extract — " << mesh.symbol << " (mms, pose "
      << mesh.morphs[morph].name << " keyframe 0)\n";
    o << "o " << base << "\n";
    for (const auto& v : vs) o << "v " << v.pos[0] << " " << v.pos[1] << " " << v.pos[2] << "\n";
    for (const auto& v : vs) o << "vt " << v.uv[0] << " " << (1.0f - v.uv[1]) << "\n";
    if (mesh.normal_count)
        for (const auto& v : vs) o << "vn " << v.normal[0] << " " << v.normal[1] << " " << v.normal[2] << "\n";
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        o << "f";
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t i = mesh.indices[t + k] + 1;
            o << " " << i << "/" << i;
            if (mesh.normal_count) o << "/" << i;
        }
        o << "\n";
    }
    ctx.log("wrote " + base + " (" + std::to_string(vs.size()) + "v, " +
            std::to_string(mesh.indices.size() / 3) + "t, pose " +
            mesh.morphs[morph].name + ")");
    return true;
}


// shs (StaticShape) — rigid mesh used by scenery, vehicles, props.
//
// The 100-byte header has a fixed prelude:
//   +0x00 .. +0x17  bbox: float min[3], float max[3]
//   +0x18           u32 vertex_count    (sum across all sub-meshes)
//   +0x1C           u32 index_count     (sum across all sub-meshes)
//   +0x20           u32 num_submeshes (duplicated at +0x24)
//   +0x28           u32 → sub-mesh table  ← the ONE pointer we follow
//
// The sub-mesh table is a list of pointers terminated by 0xFFFFFFFF. Each
// pointer targets a 40-byte sub-mesh descriptor (see SubMeshDesc below).
// Each sub-mesh owns its own vertex buffer (stride 36, sentinel 0xFFFFFFFF
// at +24) and its own u32-triangle-list index buffer (indices local to its
// own vertex space, 0..vc-1). Material binding is by ORDER: sub-mesh i uses
// the i-th (ftx, txs) pair from this shs's SymbolResolve slice.
//
// Per-vertex layout (stride 36 B):
//   +0x00  pos.x, pos.y, pos.z       (3× f32)
//   +0x0C  norm.x, norm.y, norm.z    (3× f32)
//   +0x18  0xFFFFFFFF                (sentinel, fixed)
//   +0x1C  uv.u, uv.v                (2× f32)

// Sub-mesh table + descriptor layout and the order-based (ftx, txs) material
// binding are documented and decoded in ShapeReader.cpp (docs §8.2–8.5).

// Strip the trailing ":ftx" / ":txs" tag from a symbol; returns "" if empty.
std::string strip_tag(std::string s) {
    auto cut = s.rfind(':');
    if (cut != std::string::npos) s.resize(cut);
    return s;
}

std::string to_lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Render class for a `txs` shader symbol. rct3dump's 40-style table (docs §12)
// gives each style a D3D AlphaTestEnable + blend state; for OBJ/MTL purposes we
// collapse that to three cases. AlphaTestEnable=true ⇔ the texture is a cutout
// (its alpha channel masks the pixel) — that's every `SiAlpha*` style plus the
// two named ones. `SIGlass` is the only semi-transparent blend. Everything else
// (`SIOpaque*`, the opaque `*Chrome` variants, `SIFillZ`) is opaque.
enum class TxsClass { Opaque, Cutout, Blend };

TxsClass classify_txs(const std::string& txs_symbol) {
    std::string n = to_lower(strip_tag(txs_symbol));
    if (n == "siglass") return TxsClass::Blend;
    if (n.rfind("sialpha", 0) == 0 || n == "billboardstandard" || n == "guiicon")
        return TxsClass::Cutout;
    return TxsClass::Opaque;
}

const char* txs_class_name(TxsClass c) {
    return c == TxsClass::Opaque ? "opaque"
         : c == TxsClass::Cutout ? "cutout"
                                 : "blend";
}

// Per-extract() shared state: caches OvlParser instances for cross-OVL texture
// lookups (a single coaster shs may reference textures from 2–3 distinct
// OVLs; many shs in a batch run share the same source texture OVL — caching
// avoids repeated multi-millisecond re-parses) and tracks which texture
// symbols have already been extracted to avoid duplicate disk writes.
struct AutoTextureState {
    std::map<std::string, std::unique_ptr<OvlParser>> parsers;
    std::set<std::string>                             done_symbols_lc;

    OvlParser* get_or_parse(const std::filesystem::path& path,
                            const ExtractContext& ctx) {
        std::string key = path.string();
        auto it = parsers.find(key);
        if (it != parsers.end()) return it->second.get();
        try {
            auto p = std::make_unique<OvlParser>();
            p->parse(path.string());
            auto* raw = p.get();
            parsers.emplace(key, std::move(p));
            return raw;
        } catch (const std::exception& e) {
            ctx.log("auto-textures: cannot parse " + key + ": " + e.what());
            parsers.emplace(key, nullptr);
            return nullptr;
        }
    }
};

// Survey-mode: dump CSV row of the 25 u32 header words + pointer-flags for
// each, cross-referenced against the relocations set. Triggered by env var
// OVL_SHS_SURVEY=1. Used for one-shot reverse-engineering of header variants.
// Output schema (stdout):
//   side,ovlbase,symbol,vc,ic,w00..w24,p00..p24
//     wNN  hex value of u32 at +0xNN*4 from header start (25 cols)
//     pNN  'R' if that offset is in relocations table, else '.'
bool g_shs_survey() {
    static const bool on = std::getenv("OVL_SHS_SURVEY") != nullptr;
    return on;
}

void dump_shs_survey(const OvlParser& parser, OvlSide side,
                     const LinkedFiles& lf, const std::string& symbol) {
    auto pr = parser.offset_to_position(lf.loaderreference.datapointer);
    if (!pr.found) return;
    const auto& sd = parser.side(pr.currentOVL);
    BinaryReader r(sd.ovlname);
    r.seek(pr.position);
    std::uint32_t w[25];
    for (int i = 0; i < 25; ++i) w[i] = r.read_u32();

    std::vector<std::uint8_t> is_ptr(25, 0);
    for (auto rel : sd.relocations) {
        std::uint32_t off = lf.loaderreference.datapointer;
        if (rel >= off && rel < off + 100 && ((rel - off) % 4) == 0) {
            is_ptr[(rel - off) / 4] = 1;
        }
    }
    auto ovlbase = std::filesystem::path(sd.ovlname).stem().stem().string();
    std::cout << side_name(side) << "," << ovlbase << "," << symbol
              << "," << w[6] /*vc=+0x18*/ << "," << w[7] /*ic=+0x1C*/;
    for (int i = 0; i < 25; ++i)
        std::cout << "," << std::hex << w[i] << std::dec;
    for (int i = 0; i < 25; ++i)
        std::cout << "," << (is_ptr[i] ? 'R' : '.');
    // For each relocated field, dump first 16 bytes at its target as hex.
    // Format: "idx=hexbytes" separated by ';'. Empty if no relocation.
    std::cout << ",";
    bool first = true;
    for (int i = 0; i < 25; ++i) {
        if (!is_ptr[i] || w[i] == 0) continue;
        auto tp = parser.offset_to_position(w[i]);
        if (!tp.found) continue;
        BinaryReader rr(parser.side(tp.currentOVL).ovlname);
        rr.seek(tp.position);
        if (!first) std::cout << ";";
        first = false;
        std::cout << std::hex << (i*4) << std::dec << "=";
        for (int b = 0; b < 16; ++b)
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << (unsigned)rr.read_u8() << std::dec;
    }
    std::cout << "\n";

    // Sub-mesh table walk: pointer at +0x28 (w10) → list of sub-mesh
    // descriptor pointers terminated by 0xFFFFFFFF. Each descriptor is ~40
    // bytes. We dump up to 8 descriptors, 48 bytes each, for layout RE.
    if (is_ptr[10] && w[10] != 0) {
        auto pt = parser.offset_to_position(w[10]);
        if (pt.found) {
            BinaryReader rt(parser.side(pt.currentOVL).ovlname);
            rt.seek(pt.position);
            std::vector<std::uint32_t> sm_ptrs;
            for (int i = 0; i < 16; ++i) {
                std::uint32_t p = rt.read_u32();
                if (p == 0xFFFFFFFFu) break;
                sm_ptrs.push_back(p);
            }
            std::cout << "SMTABLE " << symbol << " count=" << sm_ptrs.size();
            for (auto p : sm_ptrs)
                std::cout << " " << std::hex << p << std::dec;
            std::cout << "\n";
            for (std::size_t i = 0; i < sm_ptrs.size(); ++i) {
                auto sp = parser.offset_to_position(sm_ptrs[i]);
                if (!sp.found) continue;
                BinaryReader rs(parser.side(sp.currentOVL).ovlname);
                rs.seek(sp.position);
                std::cout << "SMDESC " << symbol << "[" << i << "]@"
                          << std::hex << sm_ptrs[i] << std::dec << ":";
                for (int b = 0; b < 48; ++b)
                    std::cout << " " << std::hex << std::setw(2)
                              << std::setfill('0') << (unsigned)rs.read_u8()
                              << std::dec;
                std::cout << "\n";
            }
        }
    }
}

// Shared mesh extractor for shs (StaticShape, rigid) and bsh (BoneShape,
// skinned). They share the same header prelude (vc@+0x18, ic@+0x1C, sub-mesh
// table ptr @+0x28), sub-mesh descriptor layout, material binding, and OBJ/MTL
// emission. They differ only in the per-vertex stride and index width:
//   shs: VERTEX  (36 B) = pos(12) normal(12) sentinel/color(4) uv(8); u32 idx
//   bsh: VERTEX2 (44 B) = pos(12) normal(12) bone(4) unk(4) color(4) uv(8); u16 idx
// Decoding is shared with SceneryExtractor (ShapeReader). bsh vertex positions
// are model-space at the bind pose, which is also frame 0 of the item's ban
// animation, so the OBJ shows the item as the game does when it starts
// animating. For the skeleton + animation, use `--types scenery` (glTF).
bool process_mesh(const OvlParser& parser,
                  OvlSide side,
                  std::size_t lf_index,
                  const std::filesystem::path& out_dir,
                  bool overwrite,
                  const ExtractContext& ctx,
                  AutoTextureState& auto_tex,
                  bool bone) {
    const char* kind = bone ? "bsh" : "shs";
    const auto& d = parser.side(side);
    const auto& lf = d.linkedfiles[lf_index];

    std::string symbol = parser.string_from_offset(lf.symbolresolve.stringpointer);
    auto cut = symbol.rfind(':');
    if (cut != std::string::npos) symbol = symbol.substr(0, cut);
    std::string base = sanitize(symbol);

    if (!bone && g_shs_survey()) {
        dump_shs_survey(parser, side, lf, symbol);
        return true;
    }

    auto out_path = out_dir / (base + ".obj");
    if (!overwrite && std::filesystem::exists(out_path)) {
        ctx.log("skip (exists): " + base);
        return true;
    }

    Shape shape;
    std::string err;
    if (!read_shape(parser, side, lf_index, bone, shape, err)) {
        ctx.log(std::string(kind) + " " + symbol + ": " + err);
        return false;
    }
    std::uint32_t vc = 0, ic = 0;
    for (const auto& p : shape.parts) {
        vc += static_cast<std::uint32_t>(p.verts.size());
        ic += static_cast<std::uint32_t>(p.indices.size());
    }

    struct SubMesh {
        std::vector<Vertex>         verts;
        std::vector<std::uint32_t>  idx;
        std::string                 mtl_name;    // sanitized ftx symbol
        std::string                 ftx_symbol;  // full original-case symbol
        std::string                 txs_name;    // raw txs symbol (for sidecar)
    };
    std::vector<SubMesh> meshes;
    meshes.reserve(shape.parts.size());
    for (const auto& part : shape.parts) {
        if (part.texture_flags & kNonVisualFlags) continue;   // shadow / collision mesh
        SubMesh m;
        // RCT3 is Direct3D (left-handed); OBJ consumers assume right-handed.
        // Mirror Z and flip the winding, or every model comes out mirrored
        // (text on signs reads backwards).
        m.verts.reserve(part.verts.size());
        for (const auto& sv : part.verts) {
            Vertex v;
            v.x = sv.pos[0];    v.y = sv.pos[1];    v.z = -sv.pos[2];
            v.nx = sv.normal[0]; v.ny = sv.normal[1]; v.nz = -sv.normal[2];
            v.u = sv.uv[0];     v.v = sv.uv[1];
            m.verts.push_back(v);
        }
        m.idx = part.indices;
        for (std::size_t t = 0; t + 2 < m.idx.size(); t += 3)
            std::swap(m.idx[t + 1], m.idx[t + 2]);
        // Material binding (one (ftx, txs) pair per sub-mesh, in order). The
        // material key combines BOTH: one ftx can be sampled by several
        // sub-meshes with different txs shaders (e.g. gigacoaster as both
        // SIOpaqueSpecular50Reflection and SIAlphaMaskLow), which are distinct
        // materials. The texture file (map_Kd) is still keyed by ftx alone.
        if (!part.ftx_symbol.empty() || !part.txs_symbol.empty()) {
            m.ftx_symbol = part.ftx_symbol;
            m.txs_name   = part.txs_symbol;
            std::string ftx_base = sanitize(strip_tag(part.ftx_symbol));
            std::string txs_base = sanitize(strip_tag(part.txs_symbol));
            m.mtl_name = txs_base.empty() ? ftx_base : (ftx_base + "__" + txs_base);
        } else {
            m.mtl_name = "default";
        }
        meshes.push_back(std::move(m));
    }

    std::filesystem::create_directories(out_dir);

    // For each unique ftx symbol referenced by this shs, look up where it
    // lives via the global texture index (if provided). Resolution is
    // case-insensitive — the index keys are pre-lowercased. We record one
    // record per unique sanitized material name (the .mtl key) so a single
    // ftx referenced by several sub-meshes (with different txs shaders) only
    // produces one map_Kd line.
    struct MtlSlot {
        std::string ftx_symbol;       // original-case full symbol, may be empty
        std::string txs_name;         // raw txs symbol (drives alpha/blend)
        std::string tga_name;         // sanitized basename + ".tga", may be empty
        bool        resolved = false; // true iff index hit
    };
    std::map<std::string, MtlSlot> mtl_slots;  // keyed by (ftx__txs) material name
    std::vector<std::string> mtl_order;
    for (const auto& m : meshes) {
        if (mtl_slots.count(m.mtl_name)) continue;
        MtlSlot slot;
        slot.ftx_symbol = m.ftx_symbol;
        slot.txs_name   = m.txs_name;
        if (ctx.texture_index && !m.ftx_symbol.empty()) {
            auto lc = to_lower(m.ftx_symbol);
            if (const auto* e = ctx.texture_index->lookup(lc)) {
                slot.resolved = true;
                slot.tga_name = sanitize(strip_tag(e->symbol)) + ".tga";
            }
        }
        mtl_slots.emplace(m.mtl_name, std::move(slot));
        mtl_order.push_back(m.mtl_name);
    }

    // Auto-extract textures for resolved slots (one extraction per unique
    // symbol across the whole batch). The index entry tells us which OVL +
    // side hosts the texture; parsers are cached in `auto_tex` so the same
    // shared texture OVL (e.g. Track6_Textures) is only parsed once.
    if (ctx.auto_extract_textures && ctx.texture_index) {
        for (auto& [mtl_name, slot] : mtl_slots) {
            if (!slot.resolved) continue;
            auto lc = to_lower(slot.ftx_symbol);
            if (!auto_tex.done_symbols_lc.insert(lc).second) continue;
            const auto* e = ctx.texture_index->lookup(lc);
            if (!e) continue;
            auto src_path = ctx.texture_index->resolve_ovl_path(*e);
            OvlParser* sp = auto_tex.get_or_parse(src_path, ctx);
            if (!sp) continue;
            ExtractContext sub_ctx;
            sub_ctx.output_dir = out_dir;
            sub_ctx.overwrite  = ctx.overwrite;
            sub_ctx.log        = ctx.log;
            if (!TextureExtractor::extract_symbol(*sp, lc, sub_ctx)) {
                ctx.log("auto-textures: " + slot.ftx_symbol +
                        " declared in " + e->ovl + " (" + e->side +
                        ") but no matching linkedfile found");
            }
        }
    }

    // Companion .mtl. One newmtl per sanitized ftx name; map_Kd is emitted
    // only when the symbol resolves via the index, otherwise we leave a
    // comment so the file is still valid for Blender.
    auto mtl_path = out_dir / (base + ".mtl");
    std::ofstream mtl(mtl_path);
    mtl << "# RCT3 OVL extract — " << symbol << "\n";
    for (const auto& name : mtl_order) {
        const auto& slot = mtl_slots.at(name);
        TxsClass cls = slot.txs_name.empty() ? TxsClass::Opaque
                                             : classify_txs(slot.txs_name);
        mtl << "newmtl " << name << "\n";
        // Provenance + shader class. The txs style (docs §12) decides whether
        // the texture's alpha channel is a cutout mask (map_d) or ignored.
        mtl << "# tex: " << (slot.ftx_symbol.empty() ? "(none)" : slot.ftx_symbol);
        if (!slot.txs_name.empty())
            mtl << "  txs: " << slot.txs_name << " (" << txs_class_name(cls) << ")";
        mtl << "\n";
        if (!slot.resolved && !slot.ftx_symbol.empty())
            mtl << "# unresolved: " << slot.ftx_symbol
                << " (not in texture index)\n";
        mtl << "Ka 0.0 0.0 0.0\n"
            << "Kd 1.0 1.0 1.0\n"
            << "Ks 0.0 0.0 0.0\n";
        // Opaque → fully solid. Cutout → solid base, per-texel alpha from the
        // texture (map_d). Blend (SIGlass) → semi-transparent.
        mtl << "d " << (cls == TxsClass::Blend ? "0.6" : "1.0") << "\n";
        mtl << "illum 2\n";
        if (slot.resolved) {
            // <tex>.tga is opaque; <tex>_rgba.tga carries the FTX alpha plane
            // (docs §3.5) and is what alpha-tested / blended materials sample.
            if (cls != TxsClass::Opaque) {
                std::string rgba = slot.tga_name.substr(0, slot.tga_name.size() - 4) + "_rgba.tga";
                mtl << "map_Kd " << rgba << "\n"
                    << "map_d " << rgba << "\n";
            } else {
                mtl << "map_Kd " << slot.tga_name << "\n";
            }
        }
        mtl << "\n";
    }

    // OBJ: all verts/UVs first (sub-meshes concatenated), then per-sub-mesh
    // group with usemtl directive and local-to-global index remap.
    std::ofstream o(out_path);
    if (!o) return false;
    o << "# RCT3 OVL extract (" << kind << ") — " << symbol << "\n";
    o << "# verts=" << vc << " indices=" << ic
      << " submeshes=" << meshes.size() << "\n";
    o << "mtllib " << base << ".mtl\n";
    o << "o " << base << "\n";
    for (const auto& m : meshes) {
        for (const auto& v : m.verts)
            o << "v " << v.x << " " << v.y << " " << v.z << "\n";
    }
    for (const auto& m : meshes) {
        for (const auto& v : m.verts)
            o << "vt " << v.u << " " << (1.0f - v.v) << "\n";
    }
    for (const auto& m : meshes) {
        for (const auto& v : m.verts)
            o << "vn " << v.nx << " " << v.ny << " " << v.nz << "\n";
    }
    std::uint32_t vbase = 0;
    std::uint32_t tris_total = 0;
    for (std::size_t s = 0; s < meshes.size(); ++s) {
        const auto& m = meshes[s];
        o << "g " << base << "_sub" << s << "\n";
        o << "usemtl " << m.mtl_name << "\n";
        std::uint32_t tris = static_cast<std::uint32_t>(m.idx.size()) / 3;
        // Faces index position/UV/normal — all share the per-vertex order.
        for (std::uint32_t t = 0; t < tris; ++t) {
            std::uint32_t a = m.idx[t*3 + 0] + vbase + 1;
            std::uint32_t b = m.idx[t*3 + 1] + vbase + 1;
            std::uint32_t c = m.idx[t*3 + 2] + vbase + 1;
            o << "f " << a << "/" << a << "/" << a << " "
              << b << "/" << b << "/" << b << " "
              << c << "/" << c << "/" << c << "\n";
        }
        tris_total += tris;
        vbase += static_cast<std::uint32_t>(m.verts.size());
    }

    ctx.log("wrote " + base + " (" + std::to_string(vc) + "v, " +
            std::to_string(tris_total) + "t, " +
            std::to_string(meshes.size()) + " submesh(es))");
    return true;
}

// One animation keyframe: time + a 3-vector (a translation, or an axis-angle
// rotation whose magnitude is the angle in radians — rct3dump's rotmath).
struct TXYZ { float t, x, y, z; };

// ban (BoneAnim) — skeletal animation tracks for bsh meshes. OBJ can't carry a
// skeleton + keyframes, so we emit the animation as a JSON sidecar
// (<name>.anim.json) for downstream use (a glTF converter, inspection, …).
// Layout (rct3dump §13.3; all pointers are internal offsets):
//   BoneAnim     { u32 BoneCount; BoneAnimBone* Bones; f32 TotalTime; }
//   BoneAnimBone { char* Name; u32 TransCount; txyz* Translate;
//                  u32 RotCount; txyz* Rotate; }   (20 bytes/entry)
//   txyz         { f32 Time, X, Y, Z; }            (16 bytes/entry)
bool process_ban(const OvlParser& parser, OvlSide side, std::size_t lf_index,
                 const std::filesystem::path& out_dir, bool overwrite,
                 const ExtractContext& ctx) {
    const auto& d = parser.side(side);
    const auto& lf = d.linkedfiles[lf_index];
    std::string symbol = parser.string_from_offset(lf.symbolresolve.stringpointer);
    auto cut = symbol.rfind(':');
    if (cut != std::string::npos) symbol = symbol.substr(0, cut);
    std::string base = sanitize(symbol);

    auto out_path = out_dir / (base + ".anim.json");
    if (!overwrite && std::filesystem::exists(out_path)) {
        ctx.log("skip (exists): " + base + ".anim.json");
        return true;
    }

    auto pr = parser.offset_to_position(lf.loaderreference.datapointer);
    if (!pr.found) return false;
    BinaryReader r(parser.side(pr.currentOVL).ovlname);
    r.seek(pr.position);
    std::uint32_t bone_count = r.read_u32();
    std::uint32_t bones_ptr  = r.read_u32();
    float         total_time = r.read_f32();
    if (bone_count == 0 || bone_count > 4096) {
        ctx.log("ban: " + symbol + " implausible bone count " +
                std::to_string(bone_count));
        return false;
    }
    auto bp = parser.offset_to_position(bones_ptr);
    if (!bp.found) {
        ctx.log("ban: " + symbol + " bones pointer unresolved");
        return false;
    }
    const std::string bones_ovl = parser.side(bp.currentOVL).ovlname;

    auto read_track = [&](std::uint32_t ptr, std::uint32_t count) {
        std::vector<TXYZ> out;
        if (ptr == 0 || count == 0 || count > 1000000) return out;
        auto tp = parser.offset_to_position(ptr);
        if (!tp.found) return out;
        BinaryReader rt(parser.side(tp.currentOVL).ovlname);
        rt.seek(tp.position);
        out.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            TXYZ k;
            k.t = rt.read_f32(); k.x = rt.read_f32();
            k.y = rt.read_f32(); k.z = rt.read_f32();
            out.push_back(k);
        }
        return out;
    };
    auto jesc = [](const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if (static_cast<unsigned char>(c) >= 0x20) o += c;
        }
        return o;
    };

    std::filesystem::create_directories(out_dir);
    std::ofstream j(out_path);
    if (!j) return false;
    j << "{\n  \"name\": \"" << jesc(symbol) << "\",\n"
      << "  \"total_time\": " << total_time << ",\n"
      << "  \"bones\": [\n";
    std::size_t total_keys = 0;
    for (std::uint32_t b = 0; b < bone_count; ++b) {
        BinaryReader rb(bones_ovl);
        rb.seek(bp.position + static_cast<std::uint64_t>(b) * 20);
        std::uint32_t name_ptr = rb.read_u32();
        std::uint32_t tc = rb.read_u32();
        std::uint32_t tp = rb.read_u32();
        std::uint32_t rc = rb.read_u32();
        std::uint32_t rp = rb.read_u32();
        std::string name = string_at_slot(parser, name_ptr, bones_ptr + b * 20);
        auto trans = read_track(tp, tc);
        auto rot   = read_track(rp, rc);
        total_keys += trans.size() + rot.size();
        auto emit = [&](const char* key, const std::vector<TXYZ>& v, bool last) {
            j << "      \"" << key << "\": [";
            for (std::size_t i = 0; i < v.size(); ++i) {
                if (i) j << ", ";
                j << "[" << v[i].t << "," << v[i].x << "," << v[i].y << ","
                  << v[i].z << "]";
            }
            j << "]" << (last ? "" : ",") << "\n";
        };
        j << "    {\n      \"name\": \"" << jesc(name) << "\",\n";
        emit("translate", trans, false);
        emit("rotate", rot, true);
        j << "    }" << (b + 1 < bone_count ? "," : "") << "\n";
    }
    j << "  ]\n}\n";
    ctx.log("wrote " + base + ".anim.json (" + std::to_string(bone_count) +
            " bone(s), " + std::to_string(total_keys) + " keys, t=" +
            std::to_string(total_time) + "s)");
    return true;
}

bool side_loop(const OvlParser& parser,
               OvlSide side,
               const ExtractContext& ctx,
               ExtractResult& res,
               AutoTextureState& auto_tex) {
    const auto& d = parser.side(side);
    bool any = false;
    for (std::size_t i = 0; i < d.linkedfiles.size(); ++i) {
        const auto& lf = d.linkedfiles[i];
        Loader ldr = parser.loader_by_id(lf.loaderreference.loadernumber, side);
        try {
            bool ok = false;
            if (ldr.tag == "shs") {
                ok = process_mesh(parser, side, i, ctx.output_dir,
                                  ctx.overwrite, ctx, auto_tex, /*bone=*/false);
            } else if (ldr.tag == "bsh") {
                ok = process_mesh(parser, side, i, ctx.output_dir,
                                  ctx.overwrite, ctx, auto_tex, /*bone=*/true);
            } else if (ldr.tag == "ban") {
                ok = process_ban(parser, side, i, ctx.output_dir,
                                 ctx.overwrite, ctx);
            } else if (ldr.tag == "mms") {
                ok = process_mms(parser, side, i, ctx.output_dir,
                                 ctx.overwrite, ctx);
            } else {
                continue;
            }
            if (ok) { ++res.files_written; any = true; }
            else    { ++res.errors; }
        } catch (const std::exception& e) {
            ++res.errors;
            ctx.log(std::string(ldr.tag) + ": exception — " + e.what());
        }
    }
    return any;
}

}  // namespace

ExtractResult ModelExtractor::extract(const OvlParser& parser,
                                      const ExtractContext& ctx) {
    ExtractResult res{};
    AutoTextureState auto_tex;
    side_loop(parser, OvlSide::Common, ctx, res, auto_tex);
    if (parser.has_unique()) side_loop(parser, OvlSide::Unique, ctx, res, auto_tex);
    return res;
}

}  // namespace ovl
