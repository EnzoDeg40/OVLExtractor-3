#include "ovl/extract/ModelExtractor.hpp"

#include "ovl/BinaryReader.hpp"
#include "ovl/Error.hpp"
#include "ovl/OvlParser.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
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

// shs (StaticShape) — rigid LOD mesh used by scenery items.
//
// Header at loaderreference.datapointer (100 bytes):
//   +0x00 .. +0x17  bounding box: float min[3], float max[3]
//   +0x18           u32 vertex_count
//   +0x1C           u32 index_count
//   +0x20, +0x24    u32 (=1, =1)
//   +0x28           u32 ptr (unique-side data, e.g. material entry)
//   +0x2C           u32 (=1)
//   +0x30           u32 ptr to data block (this is what we want)
//   +0x34           u32 ptr to LOD name footer (16B = self-ref + 12B ascii name)
//   +0x38           u32 ptr to unique-side per-LOD entry
//   +0x3C           u32 -1 (terminator)
//   +0x40 .. +0x63  zeros (padding)
//
// Data block (starting at +0x30 ptr) layout:
//   [64B]   transform matrix (4x4 floats, row-major) — LOD's local→world
//   [vc x 36B]  vertex array, each:
//                 +0x00  pos.x, pos.y, pos.z      (3 floats)
//                 +0x0C  norm.x, norm.y, norm.z   (3 floats)
//                 +0x18  0xFFFFFFFF               (sentinel / unused)
//                 +0x1C  uv.u, uv.v               (2 floats)
//   [ic x 4B]   index array (u32 triangle list)
//   [16B]   LOD name footer (matches +0x34 above)
struct ShsHeader {
    std::uint32_t vertex_count;
    std::uint32_t index_count;
    std::uint32_t data_off;       // +0x30
};

bool read_shs_header(const OvlParser& parser, const LinkedFiles& lf,
                     ShsHeader& h) {
    auto pr = parser.offset_to_position(lf.loaderreference.datapointer);
    if (!pr.found) return false;
    BinaryReader r(parser.side(pr.currentOVL).ovlname);
    r.seek(pr.position + 0x18);
    h.vertex_count = r.read_u32();
    h.index_count  = r.read_u32();
    r.seek(pr.position + 0x30);
    h.data_off     = r.read_u32();
    return true;
}

bool process_shs(const OvlParser& parser,
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

    ShsHeader h{};
    if (!read_shs_header(parser, lf, h)) return false;
    ctx.log("shs " + symbol + ": v=" + std::to_string(h.vertex_count) +
            " i=" + std::to_string(h.index_count));
    if (h.vertex_count == 0 || h.index_count == 0 ||
        h.vertex_count > 200000 || h.index_count > 600000) {
        ctx.log("shs: implausible counts, skipping");
        return false;
    }

    auto pd = parser.offset_to_position(h.data_off);
    if (!pd.found) return false;
    BinaryReader r(parser.side(pd.currentOVL).ovlname);
    r.seek(pd.position + 64);  // skip 64-byte transform matrix

    std::vector<Vertex> verts(h.vertex_count);
    for (std::uint32_t i = 0; i < h.vertex_count; ++i) {
        verts[i].x = r.read_f32();
        verts[i].y = r.read_f32();
        verts[i].z = r.read_f32();
        (void)r.read_f32(); (void)r.read_f32(); (void)r.read_f32();  // normal
        (void)r.read_u32();                                          // sentinel
        verts[i].u = r.read_f32();
        verts[i].v = r.read_f32();
    }

    std::vector<std::uint32_t> idx(h.index_count);
    for (auto& x : idx) x = r.read_u32();

    std::filesystem::create_directories(out_dir);
    std::ofstream o(out_path);
    if (!o) return false;
    o << "# RCT3 OVL extract (shs) — " << symbol << "\n";
    o << "# verts=" << h.vertex_count << " indices=" << h.index_count << "\n";
    o << "o " << base << "\n";
    for (const auto& vt : verts) o << "v " << vt.x << " " << vt.y << " " << vt.z << "\n";
    for (const auto& vt : verts) o << "vt " << vt.u << " " << (1.0f - vt.v) << "\n";
    std::uint32_t tris = h.index_count / 3;
    for (std::uint32_t t = 0; t < tris; ++t) {
        std::uint32_t a = idx[t*3 + 0] + 1;
        std::uint32_t b = idx[t*3 + 1] + 1;
        std::uint32_t c = idx[t*3 + 2] + 1;
        o << "f " << a << "/" << a << " " << b << "/" << b
          << " " << c << "/" << c << "\n";
    }

    ctx.log("wrote " + base + " (" + std::to_string(h.vertex_count) +
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
        try {
            bool ok = false;
            if (ldr.tag == "mms") {
                ok = process_mms(parser, side, i, ctx.output_dir,
                                 ctx.overwrite, ctx);
            } else if (ldr.tag == "shs") {
                ok = process_shs(parser, side, i, ctx.output_dir,
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
    side_loop(parser, OvlSide::Common, ctx, res);
    if (parser.has_unique()) side_loop(parser, OvlSide::Unique, ctx, res);
    return res;
}

}  // namespace ovl
