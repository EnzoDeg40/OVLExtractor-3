#include "ovl/extract/ModelExtractor.hpp"

#include "ovl/BinaryReader.hpp"
#include "ovl/Error.hpp"
#include "ovl/OvlParser.hpp"
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

// Convert IEEE 754 half-precision (16-bit) float to single-precision.
float half_to_float(std::uint16_t h) {
    std::uint32_t sign = (h & 0x8000) >> 15;
    std::uint32_t exp  = (h & 0x7C00) >> 10;
    std::uint32_t mant =  h & 0x03FF;
    std::uint32_t f;
    if (exp == 0) {
        if (mant == 0) { f = sign << 31; }
        else {
            exp = 1;
            while ((mant & 0x0400) == 0) { mant <<= 1; --exp; }
            mant &= 0x03FF;
            f = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        f = (sign << 31) | (0xFF << 23) | (mant << 13);
    } else {
        f = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
    }
    float out; std::memcpy(&out, &f, 4); return out;
}

// Each decoder reads positions for `vcount` vertices from `buf` (size `bufsz`)
// into `out`. Returns false if buffer too small for this layout.
using PositionDecoder = bool (*)(const std::uint8_t* buf, std::size_t bufsz,
                                 std::uint32_t vcount, std::vector<Vertex>& out);

bool dec_int8_127_s3(const std::uint8_t* b, std::size_t sz, std::uint32_t n,
                     std::vector<Vertex>& v) {
    if (sz < n * 3) return false;
    for (std::uint32_t i = 0; i < n; ++i) {
        v[i].x = (std::int8_t)b[i*3+0] / 127.0f;
        v[i].y = (std::int8_t)b[i*3+1] / 127.0f;
        v[i].z = (std::int8_t)b[i*3+2] / 127.0f;
    }
    return true;
}

bool dec_uint8_255_s3(const std::uint8_t* b, std::size_t sz, std::uint32_t n,
                      std::vector<Vertex>& v) {
    if (sz < n * 3) return false;
    for (std::uint32_t i = 0; i < n; ++i) {
        v[i].x = b[i*3+0] / 255.0f - 0.5f;
        v[i].y = b[i*3+1] / 255.0f - 0.5f;
        v[i].z = b[i*3+2] / 255.0f - 0.5f;
    }
    return true;
}

bool dec_int8_127_s4(const std::uint8_t* b, std::size_t sz, std::uint32_t n,
                     std::vector<Vertex>& v) {
    if (sz < n * 4) return false;
    for (std::uint32_t i = 0; i < n; ++i) {
        v[i].x = (std::int8_t)b[i*4+0] / 127.0f;
        v[i].y = (std::int8_t)b[i*4+1] / 127.0f;
        v[i].z = (std::int8_t)b[i*4+2] / 127.0f;
    }
    return true;
}

bool dec_int16_s6(const std::uint8_t* b, std::size_t sz, std::uint32_t n,
                  std::vector<Vertex>& v) {
    if (sz < n * 6) return false;
    auto r16 = [&](std::size_t o) -> std::int16_t {
        return (std::int16_t)(b[o] | (b[o+1] << 8));
    };
    for (std::uint32_t i = 0; i < n; ++i) {
        v[i].x = r16(i*6+0) / 32767.0f;
        v[i].y = r16(i*6+2) / 32767.0f;
        v[i].z = r16(i*6+4) / 32767.0f;
    }
    return true;
}

bool dec_float16_s6(const std::uint8_t* b, std::size_t sz, std::uint32_t n,
                    std::vector<Vertex>& v) {
    if (sz < n * 6) return false;
    auto rh = [&](std::size_t o) -> std::uint16_t {
        return (std::uint16_t)(b[o] | (b[o+1] << 8));
    };
    for (std::uint32_t i = 0; i < n; ++i) {
        v[i].x = half_to_float(rh(i*6+0));
        v[i].y = half_to_float(rh(i*6+2));
        v[i].z = half_to_float(rh(i*6+4));
    }
    return true;
}

bool dec_float32_s12(const std::uint8_t* b, std::size_t sz, std::uint32_t n,
                     std::vector<Vertex>& v) {
    if (sz < n * 12) return false;
    auto rf = [&](std::size_t o) -> float {
        float f; std::memcpy(&f, b + o, 4); return f;
    };
    for (std::uint32_t i = 0; i < n; ++i) {
        v[i].x = rf(i*12+0); v[i].y = rf(i*12+4); v[i].z = rf(i*12+8);
    }
    return true;
}

bool dec_packed10_snorm(const std::uint8_t* b, std::size_t sz, std::uint32_t n,
                        std::vector<Vertex>& v) {
    if (sz < n * 4) return false;
    auto sign_extend_10 = [](std::uint32_t u) -> float {
        std::int32_t s = (u & 0x200) ? (std::int32_t)(u | 0xFFFFFC00)
                                     : (std::int32_t)u;
        return s / 511.0f;
    };
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t w = b[i*4+0] | (b[i*4+1] << 8) |
                          (b[i*4+2] << 16) | (b[i*4+3] << 24);
        v[i].x = sign_extend_10(w & 0x3FF);
        v[i].y = sign_extend_10((w >> 10) & 0x3FF);
        v[i].z = sign_extend_10((w >> 20) & 0x3FF);
    }
    return true;
}

// Sign-magnitude int8: bit 7 = sign, bits 0-6 = magnitude.
bool dec_sm8(const std::uint8_t* b, std::size_t sz, std::uint32_t n,
             std::vector<Vertex>& v) {
    if (sz < n * 3) return false;
    auto sm = [](std::uint8_t u) -> float {
        float mag = (u & 0x7F) / 127.0f;
        return (u & 0x80) ? -mag : mag;
    };
    for (std::uint32_t i = 0; i < n; ++i) {
        v[i].x = sm(b[i*3+0]); v[i].y = sm(b[i*3+1]); v[i].z = sm(b[i*3+2]);
    }
    return true;
}

// uint8 biased: (u - 128) / 127  → like int8 but with 128 = zero (subtle diff)
bool dec_bias128_s3(const std::uint8_t* b, std::size_t sz, std::uint32_t n,
                    std::vector<Vertex>& v) {
    if (sz < n * 3) return false;
    for (std::uint32_t i = 0; i < n; ++i) {
        v[i].x = ((int)b[i*3+0] - 128) / 127.0f;
        v[i].y = ((int)b[i*3+1] - 128) / 127.0f;
        v[i].z = ((int)b[i*3+2] - 128) / 127.0f;
    }
    return true;
}

// int8/127 with axis swap XZY (Z-up engine → Y-up tool — very common).
bool dec_int8_xzy(const std::uint8_t* b, std::size_t sz, std::uint32_t n,
                  std::vector<Vertex>& v) {
    if (sz < n * 3) return false;
    for (std::uint32_t i = 0; i < n; ++i) {
        v[i].x = (std::int8_t)b[i*3+0] / 127.0f;
        v[i].z = (std::int8_t)b[i*3+1] / 127.0f;
        v[i].y = (std::int8_t)b[i*3+2] / 127.0f;
    }
    return true;
}

// int8/127 with Y axis flipped.
bool dec_int8_flipy(const std::uint8_t* b, std::size_t sz, std::uint32_t n,
                    std::vector<Vertex>& v) {
    if (sz < n * 3) return false;
    for (std::uint32_t i = 0; i < n; ++i) {
        v[i].x =  (std::int8_t)b[i*3+0] / 127.0f;
        v[i].y = -(std::int8_t)b[i*3+1] / 127.0f;
        v[i].z =  (std::int8_t)b[i*3+2] / 127.0f;
    }
    return true;
}

// Delta-encoded: each vertex is offset from previous (running sum).
bool dec_delta_int8(const std::uint8_t* b, std::size_t sz, std::uint32_t n,
                    std::vector<Vertex>& v) {
    if (sz < n * 3) return false;
    float cx = 0, cy = 0, cz = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        cx += (std::int8_t)b[i*3+0] / 127.0f;
        cy += (std::int8_t)b[i*3+1] / 127.0f;
        cz += (std::int8_t)b[i*3+2] / 127.0f;
        v[i].x = cx; v[i].y = cy; v[i].z = cz;
    }
    return true;
}

// Same as sm8 but 4-byte stride (last byte = padding/flag).
bool dec_sm8_s4(const std::uint8_t* b, std::size_t sz, std::uint32_t n,
                std::vector<Vertex>& v) {
    if (sz < n * 4) return false;
    auto sm = [](std::uint8_t u) -> float {
        float mag = (u & 0x7F) / 127.0f;
        return (u & 0x80) ? -mag : mag;
    };
    for (std::uint32_t i = 0; i < n; ++i) {
        v[i].x = sm(b[i*4+0]); v[i].y = sm(b[i*4+1]); v[i].z = sm(b[i*4+2]);
    }
    return true;
}

// Same as bias128 but 4-byte stride.
bool dec_bias128_s4(const std::uint8_t* b, std::size_t sz, std::uint32_t n,
                    std::vector<Vertex>& v) {
    if (sz < n * 4) return false;
    for (std::uint32_t i = 0; i < n; ++i) {
        v[i].x = ((int)b[i*4+0] - 128) / 127.0f;
        v[i].y = ((int)b[i*4+1] - 128) / 127.0f;
        v[i].z = ((int)b[i*4+2] - 128) / 127.0f;
    }
    return true;
}

// Hypothesis: positions are in the BASE vertex buffer (which we use for UVs),
// not in morph data. Each vertex is 12 bytes:
//   +0  u16 unknown_a   ← maybe packs X+Y
//   +2  u16 unknown_b   ← maybe packs Z + flags
//   +4  float U
//   +8  float V
// Try int16 / 32767 for x and y from unknown_a as packed bytes (2× int8).
bool dec_base_int8_a4(const std::uint8_t* b, std::size_t sz, std::uint32_t n,
                      std::vector<Vertex>& v) {
    if (sz < n * 12) return false;
    for (std::uint32_t i = 0; i < n; ++i) {
        v[i].x = (std::int8_t)b[i*12+0] / 127.0f;
        v[i].y = (std::int8_t)b[i*12+1] / 127.0f;
        v[i].z = (std::int8_t)b[i*12+2] / 127.0f;
    }
    return true;
}

bool dec_base_int16_xy(const std::uint8_t* b, std::size_t sz, std::uint32_t n,
                       std::vector<Vertex>& v) {
    if (sz < n * 12) return false;
    auto r16 = [&](std::size_t o) -> std::int16_t {
        return (std::int16_t)(b[o] | (b[o+1] << 8));
    };
    for (std::uint32_t i = 0; i < n; ++i) {
        v[i].x = r16(i*12+0) / 32767.0f;
        v[i].y = r16(i*12+2) / 32767.0f;
        v[i].z = 0.0f;
    }
    return true;
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

    // Diagnostic: dump full MMS header (40 bytes)
    {
        auto pr = parser.offset_to_position(lf.loaderreference.datapointer);
        if (pr.found) {
            BinaryReader r(parser.side(pr.currentOVL).ovlname);
            r.seek(pr.position);
            std::cerr << "MMS_HDR " << symbol << " (" << h.vertex_count << "v):";
            for (int i = 0; i < 10; ++i) {
                std::uint32_t v = r.read_u32();
                std::cerr << " +" << (i*4) << "=" << v;
                if (i == 2) std::cerr << "(AU1)";
                if (i == 3) std::cerr << "(AU2)";
            }
            std::cerr << "\n";
        }
    }

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
    // We emit one OBJ per candidate decoder so the user can A/B test.
    auto pp = parser.offset_to_position(morph0.positions_off);
    if (!pp.found) return false;
    BinaryReader rp(parser.side(pp.currentOVL).ovlname);
    rp.seek(pp.position);
    // Dump first 48 bytes of position data
    {
        std::cerr << "POS_BYTES " << symbol << " (" << h.vertex_count << "v):";
        for (int i = 0; i < 48; ++i) {
            std::cerr << " " << std::hex << std::setw(2) << std::setfill('0')
                      << (int)rp.read_u8() << std::dec;
        }
        std::cerr << "\n";
        rp.seek(pp.position);
    }
    // Read enough bytes for the worst-case decoder (float32 = 12 B/vtx).
    // Read a generous block — the actual buffer may be longer if multiple
    // keyframes are stored, but we only need frame 0.
    std::size_t want = static_cast<std::size_t>(h.vertex_count) * 16;
    std::vector<std::uint8_t> pbuf(want);
    for (std::size_t i = 0; i < want; ++i) {
        if (rp.eof()) { pbuf.resize(i); break; }
        pbuf[i] = rp.read_u8();
    }
    std::vector<Vertex> verts(h.vertex_count);
    // Default: try int8/127 stride 3. Other variants emitted below in
    // process_mms_variants for diagnostic comparison.
    if (!dec_int8_127_s3(pbuf.data(), pbuf.size(), h.vertex_count, verts)) {
        return false;
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

    auto write_obj = [&](const std::filesystem::path& path,
                         const std::vector<Vertex>& vs,
                         const std::string& suffix) {
        std::ofstream o(path);
        if (!o) return;
        o << "# RCT3 OVL extract — " << symbol << " (" << suffix << ")\n";
        o << "# verts=" << h.vertex_count << " indices=" << h.index_count
          << " morphs=" << h.morph_count << "\n";
        o << "o " << base << "_" << suffix << "\n";
        for (const auto& vt : vs) o << "v " << vt.x << " " << vt.y << " " << vt.z << "\n";
        for (const auto& vt : vs) o << "vt " << vt.u << " " << (1.0f - vt.v) << "\n";
        std::uint32_t tris = h.index_count / 3;
        for (std::uint32_t t = 0; t < tris; ++t) {
            std::uint32_t a = idx[t*3 + 0] + 1;
            std::uint32_t b = idx[t*3 + 1] + 1;
            std::uint32_t c = idx[t*3 + 2] + 1;
            o << "f " << a << "/" << a << " " << b << "/" << b
              << " " << c << "/" << c << "\n";
        }
    };

    std::filesystem::create_directories(out_dir);

    // Default output: int8/127 stride 3
    write_obj(out_path, verts, "default");

    // Diagnostic variants — one OBJ per candidate decoder.
    struct Cand { const char* name; PositionDecoder fn; };
    static const Cand cands[] = {
        {"int8_127_s3",    dec_int8_127_s3},
        {"uint8_255_s3",   dec_uint8_255_s3},
        {"int8_127_s4",    dec_int8_127_s4},
        {"int16_s6",       dec_int16_s6},
        {"float16_s6",     dec_float16_s6},
        {"float32_s12",    dec_float32_s12},
        {"packed10_snorm", dec_packed10_snorm},
        {"sm8_s3",         dec_sm8},
        {"sm8_s4",         dec_sm8_s4},
        {"bias128_s3",     dec_bias128_s3},
        {"bias128_s4",     dec_bias128_s4},
        {"int8_xzy",       dec_int8_xzy},
        {"int8_flipy",     dec_int8_flipy},
        {"delta_int8",     dec_delta_int8},
    };
    for (const auto& c : cands) {
        std::vector<Vertex> vs = verts;  // copy UVs
        if (!c.fn(pbuf.data(), pbuf.size(), h.vertex_count, vs)) continue;
        auto vp = out_dir / (base + "." + c.name + ".obj");
        write_obj(vp, vs, c.name);
    }

    // Also try base-buffer-as-positions hypothesis.
    static const Cand base_cands[] = {
        {"base_int8_a4", dec_base_int8_a4},
        {"base_int16_xy", dec_base_int16_xy},
    };
    for (const auto& c : base_cands) {
        std::vector<Vertex> vs = verts;
        if (!c.fn(vbuf.data(), vbuf.size(), h.vertex_count, vs)) continue;
        auto vp = out_dir / (base + "." + c.name + ".obj");
        write_obj(vp, vs, c.name);
    }

    std::uint32_t tris = h.index_count / 3;
    ctx.log("wrote " + base + " (" + std::to_string(h.vertex_count) +
            "v, " + std::to_string(tris) + "t) + variants");
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

// Sub-mesh descriptor — the unit of "one material slot" inside a shs.
//
// Each shs header at +0x28 holds a pointer to a sub-mesh table: an array of
// u32 sub-mesh descriptor pointers terminated by 0xFFFFFFFF. Each pointed-to
// descriptor is ≥ 40 bytes; the fields we care about are at fixed offsets:
//
//   +0x18  u32  vertex_count   (this sub-mesh's verts)
//   +0x1C  u32  index_count    (this sub-mesh's indices)
//   +0x20  u32  vertex_offset  (virtual offset, own buffer per sub-mesh)
//   +0x24  u32  index_offset   (virtual offset, own buffer per sub-mesh)
//
// Verified: sum of sub-mesh vc/ic == header vc/ic for HI/ME/LO samples of
// 45medslopechain. Each sub-mesh has its own contiguous vertex buffer
// (stride 36, sentinel-at-+24 layout) and own index buffer (u32 triangle
// list), indexed locally (0..vc-1 within the sub-mesh).
struct SubMeshDesc {
    std::uint32_t vc;
    std::uint32_t ic;
    std::uint32_t verts_off;
    std::uint32_t idx_off;
};

std::vector<SubMeshDesc> read_submesh_table(const OvlParser& parser,
                                            const LinkedFiles& lf) {
    std::vector<SubMeshDesc> out;
    auto pr = parser.offset_to_position(lf.loaderreference.datapointer);
    if (!pr.found) return out;
    BinaryReader rh(parser.side(pr.currentOVL).ovlname);
    rh.seek(pr.position + 0x28);
    std::uint32_t table_ptr = rh.read_u32();
    if (table_ptr == 0) return out;
    auto tp = parser.offset_to_position(table_ptr);
    if (!tp.found) return out;
    BinaryReader rt(parser.side(tp.currentOVL).ovlname);
    rt.seek(tp.position);
    std::vector<std::uint32_t> sm_ptrs;
    for (int i = 0; i < 64; ++i) {  // safety bound
        std::uint32_t p = rt.read_u32();
        if (p == 0xFFFFFFFFu || p == 0) break;
        sm_ptrs.push_back(p);
    }
    for (auto p : sm_ptrs) {
        auto sp = parser.offset_to_position(p);
        if (!sp.found) { out.clear(); return out; }
        BinaryReader rs(parser.side(sp.currentOVL).ovlname);
        rs.seek(sp.position + 0x18);
        SubMeshDesc d{};
        d.vc        = rs.read_u32();
        d.ic        = rs.read_u32();
        d.verts_off = rs.read_u32();
        d.idx_off   = rs.read_u32();
        out.push_back(d);
    }
    return out;
}

// Materials referenced by a shs come from its SymbolResolve slice (the
// resolves whose loadpointer matches this LoaderReference's internal_offset).
// Each (ftx, txs) consecutive pair = one material slot, in the same order
// as the sub-mesh table. Returns pairs of (ftx_symbol, txs_symbol).
std::vector<std::pair<std::string, std::string>>
collect_materials(const OvlParser& parser, OvlSide side,
                  const LinkedFiles& lf) {
    std::vector<std::pair<std::string, std::string>> out;
    const auto& d = parser.side(side);
    std::vector<std::string> seq;
    for (const auto& sr : d.symbolresolves) {
        if (sr.loadpointer != lf.loaderreference.internal_offset) continue;
        seq.push_back(parser.string_from_offset(sr.stringpointer));
    }
    for (std::size_t i = 0; i + 1 < seq.size(); i += 2) {
        out.emplace_back(seq[i], seq[i + 1]);
    }
    return out;
}

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

bool process_shs(const OvlParser& parser,
                 OvlSide side,
                 std::size_t lf_index,
                 const std::filesystem::path& out_dir,
                 bool overwrite,
                 const ExtractContext& ctx,
                 AutoTextureState& auto_tex) {
    const auto& d = parser.side(side);
    const auto& lf = d.linkedfiles[lf_index];

    std::string symbol = parser.string_from_offset(lf.symbolresolve.stringpointer);
    auto cut = symbol.rfind(':');
    if (cut != std::string::npos) symbol = symbol.substr(0, cut);
    std::string base = sanitize(symbol);

    if (g_shs_survey()) {
        dump_shs_survey(parser, side, lf, symbol);
        return true;
    }

    auto out_path = out_dir / (base + ".obj");
    if (!overwrite && std::filesystem::exists(out_path)) {
        ctx.log("skip (exists): " + base);
        return true;
    }

    // Read vc/ic from fixed header offsets
    std::uint32_t vc, ic;
    {
        auto pr = parser.offset_to_position(lf.loaderreference.datapointer);
        if (!pr.found) return false;
        BinaryReader r(parser.side(pr.currentOVL).ovlname);
        r.seek(pr.position + 0x18);
        vc = r.read_u32();
        ic = r.read_u32();
    }
    ctx.log("shs " + symbol + ": v=" + std::to_string(vc) +
            " i=" + std::to_string(ic));
    if (vc == 0 || ic == 0 || vc > 200000 || ic > 600000) {
        ctx.log("shs: implausible counts, skipping");
        return false;
    }

    auto submeshes = read_submesh_table(parser, lf);
    if (submeshes.empty()) {
        ctx.log("shs: no sub-mesh table at +0x28");
        return false;
    }

    // Sanity: sub-mesh counts should sum to header totals.
    std::uint32_t sum_vc = 0, sum_ic = 0;
    for (const auto& s : submeshes) { sum_vc += s.vc; sum_ic += s.ic; }
    if (sum_vc != vc || sum_ic != ic) {
        ctx.log("shs: sub-mesh sum mismatch (header v=" + std::to_string(vc) +
                "/i=" + std::to_string(ic) + " vs sum v=" +
                std::to_string(sum_vc) + "/i=" + std::to_string(sum_ic) + ")");
        return false;
    }

    auto materials = collect_materials(parser, side, lf);

    struct SubMesh {
        std::vector<Vertex>         verts;
        std::vector<std::uint32_t>  idx;
        std::string                 mtl_name;    // sanitized ftx symbol
        std::string                 ftx_symbol;  // full original-case symbol
        std::string                 txs_name;    // raw txs symbol (for sidecar)
    };
    std::vector<SubMesh> meshes;
    meshes.reserve(submeshes.size());
    for (std::size_t s = 0; s < submeshes.size(); ++s) {
        const auto& sd = submeshes[s];
        SubMesh m;
        // Vertices (stride 36, sentinel at +24)
        auto pv = parser.offset_to_position(sd.verts_off);
        if (!pv.found) {
            ctx.log("shs: sub-mesh " + std::to_string(s) +
                    " vertex pointer unresolved");
            return false;
        }
        BinaryReader rv(parser.side(pv.currentOVL).ovlname);
        rv.seek(pv.position);
        m.verts.reserve(sd.vc);
        for (std::uint32_t i = 0; i < sd.vc; ++i) {
            Vertex v;
            v.x = rv.read_f32();
            v.y = rv.read_f32();
            v.z = rv.read_f32();
            (void)rv.read_f32(); (void)rv.read_f32(); (void)rv.read_f32();
            (void)rv.read_u32();   // sentinel (validated empirically)
            v.u = rv.read_f32();
            v.v = rv.read_f32();
            m.verts.push_back(v);
        }
        // Indices (u32 triangle list, sub-mesh-local)
        auto pi = parser.offset_to_position(sd.idx_off);
        if (!pi.found) {
            ctx.log("shs: sub-mesh " + std::to_string(s) +
                    " index pointer unresolved");
            return false;
        }
        BinaryReader ri(parser.side(pi.currentOVL).ovlname);
        ri.seek(pi.position);
        m.idx.resize(sd.ic);
        for (auto& x : m.idx) x = ri.read_u32();
        // Material binding (one (ftx, txs) pair per sub-mesh, in order)
        if (s < materials.size()) {
            m.ftx_symbol = materials[s].first;
            m.mtl_name   = sanitize(strip_tag(materials[s].first));
            m.txs_name   = materials[s].second;
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
        std::string tga_name;         // sanitized basename + ".tga", may be empty
        bool        resolved = false; // true iff index hit
    };
    std::map<std::string, MtlSlot> mtl_slots;  // keyed by sanitized mtl name
    std::vector<std::string> mtl_order;
    for (const auto& m : meshes) {
        if (mtl_slots.count(m.mtl_name)) continue;
        MtlSlot slot;
        slot.ftx_symbol = m.ftx_symbol;
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
        mtl << "newmtl " << name << "\n";
        if (slot.resolved) {
            mtl << "# source: " << slot.ftx_symbol << "\n";
        } else if (!slot.ftx_symbol.empty()) {
            mtl << "# unresolved: " << slot.ftx_symbol
                << " (no entry in texture index)\n";
        }
        mtl << "Kd 1.0 1.0 1.0\n"
            << "Ka 0.0 0.0 0.0\n"
            << "Ks 0.0 0.0 0.0\n"
            << "d 1.0\n"
            << "illum 1\n";
        if (slot.resolved) {
            mtl << "map_Kd " << slot.tga_name << "\n";
        }
        mtl << "\n";
    }

    // OBJ: all verts/UVs first (sub-meshes concatenated), then per-sub-mesh
    // group with usemtl directive and local-to-global index remap.
    std::ofstream o(out_path);
    if (!o) return false;
    o << "# RCT3 OVL extract (shs) — " << symbol << "\n";
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
    std::uint32_t vbase = 0;
    std::uint32_t tris_total = 0;
    for (std::size_t s = 0; s < meshes.size(); ++s) {
        const auto& m = meshes[s];
        o << "g " << base << "_sub" << s << "\n";
        o << "usemtl " << m.mtl_name << "\n";
        std::uint32_t tris = static_cast<std::uint32_t>(m.idx.size()) / 3;
        for (std::uint32_t t = 0; t < tris; ++t) {
            std::uint32_t a = m.idx[t*3 + 0] + vbase + 1;
            std::uint32_t b = m.idx[t*3 + 1] + vbase + 1;
            std::uint32_t c = m.idx[t*3 + 2] + vbase + 1;
            o << "f " << a << "/" << a << " " << b << "/" << b
              << " " << c << "/" << c << "\n";
        }
        tris_total += tris;
        vbase += static_cast<std::uint32_t>(m.verts.size());
    }

    ctx.log("wrote " + base + " (" + std::to_string(vc) + "v, " +
            std::to_string(tris_total) + "t, " +
            std::to_string(meshes.size()) + " submesh(es))");
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
            // mms (MorphMesh) position decoding is still unresolved (see
            // candidate decoder dump in process_mms). Until it is, skip it in
            // bulk runs so we don't flood the output with 15 noise variants
            // per animated mesh. shs (StaticShape) is fully decoded.
            if (ldr.tag == "shs") {
                ok = process_shs(parser, side, i, ctx.output_dir,
                                 ctx.overwrite, ctx, auto_tex);
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
