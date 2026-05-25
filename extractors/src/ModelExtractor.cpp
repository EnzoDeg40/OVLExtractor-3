#include "ovl/extract/ModelExtractor.hpp"

#include "ovl/BinaryReader.hpp"
#include "ovl/Error.hpp"
#include "ovl/OvlParser.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace ovl {

namespace {

// MMS header at loaderreference.datapointer (40 bytes):
//   +0  u32 vertex_count
//   +4  u32 index_count          (number of u16 indices, /3 = triangles)
//   +8  u32 stride_id            (format ID, NOT byte stride — derive from offsets)
//   +12 u32 unk_a
//   +16 u32 type_flag
//   +20 u32 morph_count
//   +24 u32 vertex_data_offset   (base vertex buffer: 12B = u16×2 + 2× float32 UV)
//   +28 u32 index_data_offset    (u16 triangle list)
//   +32 u32 reserved
//   +36 u32 morph_data_offset    (per-morph descriptors, 64 bytes each)
struct MmsHeader {
    std::uint32_t vertex_count;
    std::uint32_t index_count;
    std::uint32_t morph_count;
    std::uint32_t vertex_off;
    std::uint32_t index_off;
    std::uint32_t morph_off;
};

std::string sanitize(std::string s) {
    for (auto& c : s) {
        if (c == '/' || c == '\\' || c == ':' || c == '?' || c == '*' ||
            c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    }
    if (s.empty()) s = "unnamed";
    return s;
}

bool read_header(const OvlParser& parser, const LinkedFiles& lf, MmsHeader& h) {
    auto pr = parser.offset_to_position(lf.loaderreference.datapointer);
    if (!pr.found) return false;
    BinaryReader r(parser.side(pr.currentOVL).ovlname);
    r.seek(pr.position);
    h.vertex_count = r.read_u32();
    h.index_count  = r.read_u32();
    (void)r.read_u32();                 // +8 stride_id (format hint, not bytes)
    (void)r.read_u32();                 // +12
    (void)r.read_u32();                 // +16 type_flag
    h.morph_count  = r.read_u32();
    h.vertex_off   = r.read_u32();
    h.index_off    = r.read_u32();
    (void)r.read_u32();                 // +32 reserved
    h.morph_off    = r.read_u32();      // +36
    return true;
}

struct Vertex { float x, y, z, u, v; };

// Read UV from the base vertex buffer (12 bytes per vertex):
//   +0  u16    unknown_a (possibly bone/morph index)
//   +2  u16    unknown_b
//   +4  float  U
//   +8  float  V
void read_uvs(const std::uint8_t* buf, std::uint32_t count,
              std::vector<Vertex>& verts) {
    auto rd_f32 = [&](std::size_t off) -> float {
        float f; std::memcpy(&f, buf + off, 4); return f;
    };
    for (std::uint32_t i = 0; i < count; ++i) {
        std::size_t base = i * 12;
        verts[i].u = rd_f32(base + 4);
        verts[i].v = rd_f32(base + 8);
    }
}

// Each morph entry is 64 bytes:
//   +0..+31  unknown (32 bytes)
//   +32 u32  name_ptr
//   +36 u32  times_count
//   +40 u32  times_offset
//   +44 u32  positions_offset  ← what we want for morph[0] (base pose)
//   +48 u32  attachment_offset
//   +52..+63 unknown (12 bytes)
struct MorphEntry {
    std::uint32_t name_ptr;
    std::uint32_t times_count;
    std::uint32_t times_off;
    std::uint32_t positions_off;
};

bool read_morph(const OvlParser& parser, std::uint32_t morph_table_off,
                std::size_t morph_index, MorphEntry& out) {
    auto pm = parser.offset_to_position(morph_table_off + morph_index * 64);
    if (!pm.found) return false;
    BinaryReader r(parser.side(pm.currentOVL).ovlname);
    r.seek(pm.position + 32);            // skip unknown 32 bytes
    out.name_ptr      = r.read_u32();
    out.times_count   = r.read_u32();
    out.times_off     = r.read_u32();
    out.positions_off = r.read_u32();
    return true;
}

bool process_mms(const OvlParser& parser,
                 OvlSide side,
                 std::size_t lf_index,
                 const std::filesystem::path& out_dir,
                 bool overwrite,
                 const ExtractContext& ctx) {
    const auto& d = parser.side(side);
    const auto& lf = d.linkedfiles[lf_index];

    std::string symbol = parser.string_from_offset(lf.symbolresolve.stringpointer);
    auto cut = symbol.rfind(':');
    if (cut != std::string::npos) symbol = symbol.substr(0, cut);
    std::string base = sanitize(symbol);

    auto out_path = out_dir / (base + ".obj");
    if (!overwrite && std::filesystem::exists(out_path)) {
        ctx.log("skip (exists): " + base);
        return true;
    }

    MmsHeader h{};
    if (!read_header(parser, lf, h)) return false;

    ctx.log("mms " + symbol + ": v=" + std::to_string(h.vertex_count) +
            " i=" + std::to_string(h.index_count) +
            " morph=" + std::to_string(h.morph_count));
    if (h.vertex_count == 0 || h.index_count == 0 ||
        h.vertex_count > 200000 || h.index_count > 600000 ||
        h.morph_count == 0) {
        ctx.log("mms: cannot extract — no morph (or implausible counts)");
        return false;
    }

    // Read morph[0] for base pose positions.
    MorphEntry morph0{};
    if (!read_morph(parser, h.morph_off, 0, morph0)) return false;
    std::string animname = parser.string_from_offset(morph0.name_ptr);
    ctx.log("  morph[0]='" + animname + "' times=" +
            std::to_string(morph0.times_count) + " positions_off=" +
            std::to_string(morph0.positions_off));

    // KNOWN-BROKEN: position decoding does not produce coherent meshes.
    // The SFStructs.h MorhpMeshVertex is declared as 3 × uint8, but neither
    // raw int8/127 nor uint8/255 nor 4-byte-stride variants give a
    // recognizable shape on validation. The legacy OVLExtractor-2 left
    // position decoding commented out (Form1.h:2222-2235). The
    // "Algorithm Unknown 1" header field (offset +8) likely controls the
    // decode somehow — its meaning is undocumented. Cobra-tools does not
    // support RCT3 mms. Positions written here are placeholder int8/127
    // values; the topology (indices/UVs) is correct.
    auto pp = parser.offset_to_position(morph0.positions_off);
    if (!pp.found) return false;
    BinaryReader rp(parser.side(pp.currentOVL).ovlname);
    rp.seek(pp.position);
    std::vector<Vertex> verts(h.vertex_count);
    for (std::uint32_t i = 0; i < h.vertex_count; ++i) {
        std::int8_t x = static_cast<std::int8_t>(rp.read_u8());
        std::int8_t y = static_cast<std::int8_t>(rp.read_u8());
        std::int8_t z = static_cast<std::int8_t>(rp.read_u8());
        verts[i].x = static_cast<float>(x) / 127.0f;
        verts[i].y = static_cast<float>(y) / 127.0f;
        verts[i].z = static_cast<float>(z) / 127.0f;
    }

    // Read UV from base vertex buffer (12 bytes per vertex).
    auto pv = parser.offset_to_position(h.vertex_off);
    if (!pv.found) return false;
    BinaryReader rv(parser.side(pv.currentOVL).ovlname);
    rv.seek(pv.position);
    std::vector<std::uint8_t> vbuf(h.vertex_count * 12);
    for (auto& b : vbuf) b = rv.read_u8();
    read_uvs(vbuf.data(), h.vertex_count, verts);

    // Read index buffer (u16 triangle list).
    auto pi = parser.offset_to_position(h.index_off);
    if (!pi.found) return false;
    BinaryReader ri(parser.side(pi.currentOVL).ovlname);
    ri.seek(pi.position);
    std::vector<std::uint16_t> idx(h.index_count);
    for (auto& x : idx) x = ri.read_u16();

    std::filesystem::create_directories(out_dir);
    std::ofstream o(out_path);
    if (!o) return false;
    o << "# RCT3 OVL extract — " << symbol << "\n";
    o << "# verts=" << h.vertex_count << " indices=" << h.index_count
      << " morphs=" << h.morph_count << "\n";
    o << "o " << base << "\n";
    for (const auto& vt : verts) {
        o << "v " << vt.x << " " << vt.y << " " << vt.z << "\n";
    }
    for (const auto& vt : verts) {
        o << "vt " << vt.u << " " << (1.0f - vt.v) << "\n";  // OBJ V flip
    }
    std::uint32_t tris = h.index_count / 3;
    for (std::uint32_t t = 0; t < tris; ++t) {
        std::uint32_t a = idx[t*3 + 0] + 1;  // OBJ is 1-indexed
        std::uint32_t b = idx[t*3 + 1] + 1;
        std::uint32_t c = idx[t*3 + 2] + 1;
        o << "f " << a << "/" << a << " "
                  << b << "/" << b << " "
                  << c << "/" << c << "\n";
    }
    ctx.log("wrote " + base + ".obj (" + std::to_string(h.vertex_count) +
            "v, " + std::to_string(tris) + "t)");
    return true;
}

bool side_loop(const OvlParser& parser,
               OvlSide side,
               const ExtractContext& ctx,
               ExtractResult& res) {
    const auto& d = parser.side(side);
    bool any = false;
    for (std::size_t i = 0; i < d.linkedfiles.size(); ++i) {
        const auto& lf = d.linkedfiles[i];
        Loader ldr = parser.loader_by_id(lf.loaderreference.loadernumber, side);
        if (ldr.tag != "mms") continue;
        try {
            if (process_mms(parser, side, i, ctx.output_dir,
                            ctx.overwrite, ctx)) {
                ++res.files_written;
                any = true;
            } else {
                ++res.errors;
            }
        } catch (const std::exception& e) {
            ++res.errors;
            ctx.log(std::string("mms: exception — ") + e.what());
        }
    }
    return any;
}

}  // namespace

ExtractResult ModelExtractor::extract(const OvlParser& parser,
                                      const ExtractContext& ctx) {
    ExtractResult res{};
    side_loop(parser, OvlSide::Common, ctx, res);
    if (parser.has_unique()) side_loop(parser, OvlSide::Unique, ctx, res);
    return res;
}

}  // namespace ovl
