#include "ovl/extract/TextureExtractor.hpp"

#include "ovl/BinaryReader.hpp"
#include "ovl/Error.hpp"
#include "ovl/OvlParser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace ovl {

namespace {

// Layout discovered empirically (Arrows.common.ovl + others):
// At loaderreference.datapointer for ftx loaders (76-byte header):
//   off  0:  u32 format         (8 = indexed8, 7/6/5 = other variants)
//   off  4:  u32 width
//   off  8:  u32 height
//   off 12:  u32 unk_a          (0)
//   off 16:  u32 unk_b          (0)
//   off 20:  u32 unk_c          (7 commonly)
//   off 24:  u32 mipmap_count   (1)
//   off 28:  u32 metadata_off1
//   off 32:  u32 unk_e          (1)
//   off 36:  u32 metadata_off2
//   off 40:  u32 zero
//   off 44:  u32 format_repeat  (same as offset 0)
//   off 48:  u32 width_repeat
//   off 52:  u32 height_repeat
//   off 56:  u32 unk_c_repeat   (7)
//   off 60:  u32 metadata_off3
//   off 64:  u32 pixel_internal_offset  <-- this points into chunk 1 block N
//   off 68:  u32 zero
//   off 72:  u32 zero
// Then for format=8 a 256-entry RGBA palette follows (1024 bytes).
struct FtxHeader {
    std::uint32_t format;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t mipmap_count;
    std::uint32_t pixel_internal_offset;
};

bool read_ftx_header(BinaryReader& r, FtxHeader& h, std::uint64_t base_pos) {
    h.format = r.read_u32();
    h.width  = r.read_u32();
    h.height = r.read_u32();
    r.seek(base_pos + 24);
    h.mipmap_count = r.read_u32();
    // Pixel pointer is at offset 60 within the header block.
    r.seek(base_pos + 60);
    h.pixel_internal_offset = r.read_u32();
    return true;
}

std::string sanitize(std::string s) {
    for (auto& c : s) {
        if (c == '/' || c == '\\' || c == ':' || c == '?' || c == '*' ||
            c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    }
    if (s.empty()) s = "unnamed";
    return s;
}

void write_json_sidecar(const std::filesystem::path& path,
                        const FtxHeader& h,
                        const std::string& symbol,
                        const std::string& loader_tag,
                        std::size_t raw_block_size) {
    std::ofstream o(path);
    if (!o) return;
    o << "{\n"
      << "  \"symbol\": \"" << symbol << "\",\n"
      << "  \"loader_tag\": \"" << loader_tag << "\",\n"
      << "  \"format_code\": " << h.format << ",\n"
      << "  \"width\": " << h.width << ",\n"
      << "  \"height\": " << h.height << ",\n"
      << "  \"mipmap_count\": " << h.mipmap_count << ",\n"
      << "  \"pixel_internal_offset\": " << h.pixel_internal_offset << ",\n"
      << "  \"raw_block_size\": " << raw_block_size << "\n"
      << "}\n";
}

// Write a 32-bit uncompressed TGA (top-down). Format BGRA per pixel - this is
// the native TGA pixel order, viewable by virtually any image tool (Preview,
// GIMP, browsers via tga-loader, etc.).
void write_tga(const std::filesystem::path& path,
               std::uint32_t width,
               std::uint32_t height,
               const std::vector<std::uint8_t>& bgra) {
    std::ofstream o(path, std::ios::binary);
    if (!o) return;
    std::uint8_t hdr[18] = {0};
    hdr[2]  = 2;                          // uncompressed true-color
    hdr[12] = static_cast<std::uint8_t>(width & 0xFF);
    hdr[13] = static_cast<std::uint8_t>((width >> 8) & 0xFF);
    hdr[14] = static_cast<std::uint8_t>(height & 0xFF);
    hdr[15] = static_cast<std::uint8_t>((height >> 8) & 0xFF);
    hdr[16] = 32;                         // bits per pixel
    hdr[17] = 0x28;                       // top-left origin, 8-bit alpha
    o.write(reinterpret_cast<char*>(hdr), 18);
    o.write(reinterpret_cast<const char*>(bgra.data()),
            static_cast<std::streamsize>(bgra.size()));
}

// Decode format=8 (indexed8 + palette) into BGRA pixels.
// Palette is 256 entries × 4 bytes. Byte order: B G R A (matches TGA pixel
// order, so we copy through directly). Empirically validated against
// Carcass.common.ovl — earlier RGB-first reading produced blue-tinted output
// because brown (139,69,19) was rendered as (19,69,139).
bool decode_indexed8(const OvlParser& parser,
                     const FtxHeader& h,
                     const std::vector<std::byte>& raw_header_block,
                     std::vector<std::uint8_t>& bgra_out) {
    constexpr std::size_t kPaletteOffset = 64;
    constexpr std::size_t kPaletteSize   = 256 * 4;
    if (raw_header_block.size() < kPaletteOffset + kPaletteSize) return false;
    std::size_t pixels = static_cast<std::size_t>(h.width) * h.height;
    if (pixels == 0) return false;

    // Read pixel indices from the pointer in the header.
    auto pr = parser.offset_to_position(h.pixel_internal_offset);
    if (!pr.found) return false;
    BinaryReader pr_reader(parser.side(pr.currentOVL).ovlname);
    pr_reader.seek(pr.position);
    std::vector<std::uint8_t> indices(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
        indices[i] = pr_reader.read_u8();
    }

    // Palette starts at offset 64 in the header block. Byte order: B G R A,
    // but empirically the 4th byte is NOT a per-entry alpha (mostly 0 or
    // small values across 256 entries on every palette inspected — see the
    // 4th-byte distribution check on Dice and gigacoaster palettes). So we
    // emit fully opaque alpha; whether a pixel should be transparent is a
    // *material* property, encoded by the txs shader (`SIAlphaMask*`) and
    // not by the texture itself. An earlier version hardcoded index 0 to
    // alpha=0 (chroma-key heuristic) — that broke SIOpaque models like the
    // Dice cube, where palette index 0 is a real color used inside the mesh
    // (the dot/edge fill). Restoring alpha for the alpha-masked subset of
    // sub-meshes (chains, leaves, fences…) is a follow-up that needs txs
    // semantics; see §10 of the format notes.
    const std::byte* pal = raw_header_block.data() + kPaletteOffset;
    bgra_out.resize(pixels * 4);
    for (std::size_t i = 0; i < pixels; ++i) {
        std::uint8_t idx = indices[i];
        bgra_out[i * 4 + 0] = std::to_integer<std::uint8_t>(pal[idx * 4 + 0]);
        bgra_out[i * 4 + 1] = std::to_integer<std::uint8_t>(pal[idx * 4 + 1]);
        bgra_out[i * 4 + 2] = std::to_integer<std::uint8_t>(pal[idx * 4 + 2]);
        bgra_out[i * 4 + 3] = 255;
    }
    return true;
}

bool sane_dimensions(std::uint32_t w, std::uint32_t h) {
    return w > 0 && w <= 8192 && h > 0 && h <= 8192;
}

// Trailing-section header for `tex` loaders. The OVL parser exposes the file
// position right after its parsed structures as `OvlData.dataend`. For OVL
// pairs that hold a tex linkedfile (typically on the unique side), the
// COMMON side's trailing data is the actual pixel payload. The header sits
// exactly at common's dataend; pixel data starts 0x30 bytes later.
//
// Layout (relative to common's dataend):
//   +0x00 .. +0x0F  u32×4   relocation offsets (copy of parsed relocations)
//   +0x10           u32     0x18 (constant — block 3 size in bytes?)
//   +0x14 .. +0x1B  zero
//   +0x1C           u32     0x12 (constant)
//   +0x20           u32     width
//   +0x24           u32     height
//   +0x28           u32     mipmap_count
//   +0x2C           u32     total_pixel_data_size (DXT-compressed)
//   +0x30           bytes   first mip level (largest), then each successive
//                           level concatenated, all DXT1-compressed.
//
// Mipmap layout uses the standard rule: each level is half the width and
// half the height of the previous, with each 4×4 block padded to 8 bytes
// (DXT1) even when the level is smaller than 4×4.
struct TexTrailingHeader {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t mipmap_count;
    std::uint32_t data_size;
    std::uint64_t pixel_data_pos;  // absolute file offset
};

// Total bytes a DXT1 (4 bpp) compressed mipmap chain occupies. Each 4×4
// block is 8 bytes; levels smaller than 4×4 still occupy one full block.
std::uint64_t expected_dxt1_size(std::uint32_t w, std::uint32_t h,
                                 std::uint32_t mips) {
    std::uint64_t total = 0;
    for (std::uint32_t i = 0; i < mips; ++i) {
        std::uint32_t lw = std::max(1u, w >> i);
        std::uint32_t lh = std::max(1u, h >> i);
        std::uint64_t blocks = ((lw + 3) / 4) * ((lh + 3) / 4);
        total += blocks * 8;
    }
    return total;
}

bool read_tex_trailing_header(const OvlData& side_data, TexTrailingHeader& h) {
    // `dataend` lands before the trailing section by a variable margin
    // depending on which sub-parsers touched the file pointer last, so we
    // can't seek straight to it. Instead, scan forward from `dataend` for
    // the {w, w, mip, size_matches_DXT1_or_DXT5_chain} 16-byte signature.
    // The header is rigid in shape — `w == h`, both powers of two in
    // [16, 4096], reasonable mip count, and pixel_size matches one of the
    // two compressed mipmap-chain sizes we know — so false positives are
    // negligible. Buffering the whole side into memory avoids per-u32
    // file seeks; OVLs are at most a few MB and this runs once per pair.
    std::uint64_t file_size = 0;
    std::vector<std::uint8_t> buf;
    {
        std::ifstream fs(side_data.ovlname, std::ios::binary | std::ios::ate);
        if (!fs) return false;
        file_size = static_cast<std::uint64_t>(fs.tellg());
        fs.seekg(0);
        buf.resize(static_cast<std::size_t>(file_size));
        if (!fs.read(reinterpret_cast<char*>(buf.data()),
                     static_cast<std::streamsize>(file_size))) return false;
    }
    auto u32 = [&](std::size_t at) -> std::uint32_t {
        return  static_cast<std::uint32_t>(buf[at]) |
               (static_cast<std::uint32_t>(buf[at + 1]) << 8) |
               (static_cast<std::uint32_t>(buf[at + 2]) << 16) |
               (static_cast<std::uint32_t>(buf[at + 3]) << 24);
    };

    const std::uint64_t lo = side_data.dataend > 0 ? side_data.dataend : 0x100;
    if (file_size < lo + 0x30) return false;
    for (std::uint64_t off = lo; off + 0x30 < file_size; ++off) {
        std::uint32_t w   = u32(static_cast<std::size_t>(off + 0x20));
        std::uint32_t hh  = u32(static_cast<std::size_t>(off + 0x24));
        std::uint32_t mip = u32(static_cast<std::size_t>(off + 0x28));
        std::uint32_t sz  = u32(static_cast<std::size_t>(off + 0x2c));
        if (w != hh) continue;
        if (w < 16 || w > 4096) continue;
        if ((w & (w - 1)) != 0) continue;  // not power of 2
        if (mip == 0 || mip > 16) continue;
        if (sz == 0 || off + 0x30 + sz > file_size) continue;
        std::uint64_t expected = expected_dxt1_size(w, hh, mip);
        // Exact DXT1 match (4 bpp) or 2× (DXT3/DXT5 — 8 bpp, alpha-aware).
        // The signature is strict enough that bytes inside random mms /
        // prt / vertex buffers don't accidentally trip it (we ran on the
        // full RCT3 install and the only positives were real tex headers).
        if (sz != expected && sz != expected * 2) continue;
        h.width          = w;
        h.height         = hh;
        h.mipmap_count   = mip;
        h.data_size      = sz;
        h.pixel_data_pos = off + 0x30;
        return true;
    }
    return false;
}

// Decode `blocks` (a DXT1 mipmap level for an image of `w` × `h`) into BGRA
// pixels at `out` (which must be sized w * h * 4).
void decode_dxt1_level(const std::uint8_t* blocks, std::uint32_t w,
                       std::uint32_t h, std::uint8_t* out) {
    auto unpack_565 = [](std::uint16_t v, std::uint8_t out_rgb[3]) {
        std::uint8_t r = (v >> 11) & 0x1F;
        std::uint8_t g = (v >> 5)  & 0x3F;
        std::uint8_t b =  v        & 0x1F;
        out_rgb[0] = static_cast<std::uint8_t>((r << 3) | (r >> 2));
        out_rgb[1] = static_cast<std::uint8_t>((g << 2) | (g >> 4));
        out_rgb[2] = static_cast<std::uint8_t>((b << 3) | (b >> 2));
    };
    const std::uint8_t* blk = blocks;
    for (std::uint32_t by = 0; by < h; by += 4) {
        for (std::uint32_t bx = 0; bx < w; bx += 4) {
            std::uint16_t c0 = static_cast<std::uint16_t>(blk[0] | (blk[1] << 8));
            std::uint16_t c1 = static_cast<std::uint16_t>(blk[2] | (blk[3] << 8));
            std::uint32_t idx = static_cast<std::uint32_t>(blk[4]) |
                                (static_cast<std::uint32_t>(blk[5]) << 8) |
                                (static_cast<std::uint32_t>(blk[6]) << 16) |
                                (static_cast<std::uint32_t>(blk[7]) << 24);
            std::uint8_t pal[4][4];  // [palette_idx][BGRA]
            std::uint8_t rgb0[3], rgb1[3];
            unpack_565(c0, rgb0);
            unpack_565(c1, rgb1);
            pal[0][0] = rgb0[2]; pal[0][1] = rgb0[1]; pal[0][2] = rgb0[0]; pal[0][3] = 255;
            pal[1][0] = rgb1[2]; pal[1][1] = rgb1[1]; pal[1][2] = rgb1[0]; pal[1][3] = 255;
            if (c0 > c1) {
                pal[2][0] = static_cast<std::uint8_t>((2*rgb0[2] + rgb1[2]) / 3);
                pal[2][1] = static_cast<std::uint8_t>((2*rgb0[1] + rgb1[1]) / 3);
                pal[2][2] = static_cast<std::uint8_t>((2*rgb0[0] + rgb1[0]) / 3);
                pal[2][3] = 255;
                pal[3][0] = static_cast<std::uint8_t>((rgb0[2] + 2*rgb1[2]) / 3);
                pal[3][1] = static_cast<std::uint8_t>((rgb0[1] + 2*rgb1[1]) / 3);
                pal[3][2] = static_cast<std::uint8_t>((rgb0[0] + 2*rgb1[0]) / 3);
                pal[3][3] = 255;
            } else {
                pal[2][0] = static_cast<std::uint8_t>((rgb0[2] + rgb1[2]) / 2);
                pal[2][1] = static_cast<std::uint8_t>((rgb0[1] + rgb1[1]) / 2);
                pal[2][2] = static_cast<std::uint8_t>((rgb0[0] + rgb1[0]) / 2);
                pal[2][3] = 255;
                pal[3][0] = 0; pal[3][1] = 0; pal[3][2] = 0; pal[3][3] = 0;
            }
            for (int i = 0; i < 16; ++i) {
                std::uint32_t px = bx + (i % 4);
                std::uint32_t py = by + (i / 4);
                if (px >= w || py >= h) continue;
                std::uint8_t which = static_cast<std::uint8_t>((idx >> (2 * i)) & 0x3);
                std::size_t o = (py * w + px) * 4;
                out[o + 0] = pal[which][0];
                out[o + 1] = pal[which][1];
                out[o + 2] = pal[which][2];
                out[o + 3] = pal[which][3];
            }
            blk += 8;
        }
    }
}

// Pull `n` bytes from `r` into a fresh vector. Helper for the tex path.
std::vector<std::uint8_t> read_n(BinaryReader& r, std::size_t n) {
    std::vector<std::uint8_t> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = r.read_u8();
    return out;
}

// Handle a `tex` linkedfile. Returns true if a .tga was produced.
// `tex_lf_side` is the side that DECLARES the linked file (typically unique);
// the actual DXT pixel data lives on the OTHER side's trailing section.
bool extract_tex(const OvlParser& parser,
                 OvlSide tex_lf_side,
                 const std::string& symbol,
                 const std::filesystem::path& out_path,
                 const ExtractContext& ctx) {
    // Pick the side whose trailing data section is non-empty. In every sample
    // we've seen, that's the OPPOSITE side from the tex linkedfile declaration
    // (the linkedfile sits on unique, the data on common). Fall back to the
    // declaring side if needed.
    OvlSide data_side = (tex_lf_side == OvlSide::Unique)
                      ? OvlSide::Common : OvlSide::Unique;
    if (!parser.has_unique() && data_side == OvlSide::Unique) {
        data_side = OvlSide::Common;
    }

    TexTrailingHeader h{};
    if (!read_tex_trailing_header(parser.side(data_side), h)) return false;

    if (!sane_dimensions(h.width, h.height) || h.mipmap_count == 0 ||
        h.mipmap_count > 16 || h.data_size == 0 || h.data_size > 16 * 1024 * 1024) {
        ctx.log("tex: implausible header for " + symbol +
                " (w=" + std::to_string(h.width) +
                " h=" + std::to_string(h.height) +
                " mip=" + std::to_string(h.mipmap_count) +
                " sz=" + std::to_string(h.data_size) + ")");
        return false;
    }

    std::uint64_t expected = expected_dxt1_size(h.width, h.height, h.mipmap_count);
    if (h.data_size != expected) {
        // The format is probably DXT3 or DXT5 (2x DXT1 size) or something
        // else we don't decode yet. Skip but log.
        ctx.log("tex: " + symbol + " " + std::to_string(h.width) + "x" +
                std::to_string(h.height) + " size=" + std::to_string(h.data_size) +
                " ≠ DXT1 expected " + std::to_string(expected) + " (non-DXT1)");
        return false;
    }

    BinaryReader r(parser.side(data_side).ovlname);
    r.seek(h.pixel_data_pos);
    auto blocks = read_n(r, h.data_size);

    // Decode only mip level 0 (the full-resolution image) to TGA. Subsequent
    // mips exist in the stream but consumers can regenerate them on the fly.
    std::uint64_t lvl0_size = expected_dxt1_size(h.width, h.height, 1);
    std::vector<std::uint8_t> bgra(static_cast<std::size_t>(h.width) * h.height * 4);
    decode_dxt1_level(blocks.data(),
                      h.width, h.height, bgra.data());
    (void)lvl0_size;  // sub-mip data simply ignored
    write_tga(out_path, h.width, h.height, bgra);
    ctx.log("wrote " + out_path.filename().string() + " (" +
            std::to_string(h.width) + "x" + std::to_string(h.height) +
            ", DXT1, " + std::to_string(h.mipmap_count) + " mips)");
    return true;
}

bool extract_one(const OvlParser& parser,
                 OvlSide side,
                 std::size_t lf_index,
                 const std::filesystem::path& out_dir,
                 bool overwrite,
                 const ExtractContext& ctx) {
    const auto& d = parser.side(side);
    const auto& lf = d.linkedfiles[lf_index];
    Loader ldr = parser.loader_by_id(lf.loaderreference.loadernumber, side);
    if (ldr.tag != "ftx" && ldr.tag != "tex" && ldr.tag != "fts" && ldr.tag != "ftt") {
        return false;
    }

    std::string symbol = parser.string_from_offset(lf.symbolresolve.stringpointer);
    auto suffix_pos = symbol.rfind(':');
    if (suffix_pos != std::string::npos) symbol = symbol.substr(0, suffix_pos);
    std::string base = sanitize(symbol);

    // tex format is structurally different from ftx: pixel data lives in the
    // opposite side's trailing section, DXT1-compressed with mipmaps. We
    // dispatch to a dedicated decoder and skip the .ovltex / .json sidecar
    // emission (the format is now understood — no need to dump raw blocks).
    if (ldr.tag == "tex") {
        std::filesystem::create_directories(out_dir);
        auto tga_out = out_dir / (base + ".tga");
        if (!overwrite && std::filesystem::exists(tga_out)) {
            ctx.log("skip (exists): " + base);
            return true;
        }
        return extract_tex(parser, side, symbol, tga_out, ctx);
    }

    auto raw_out = out_dir / (base + ".ovltex");
    auto json_out = out_dir / (base + ".json");
    if (!overwrite && std::filesystem::exists(raw_out)) {
        ctx.log("skip (exists): " + base);
        return true;
    }

    auto pr = parser.offset_to_position(lf.loaderreference.datapointer);
    if (!pr.found) {
        ctx.log("skip (datapointer unresolved): " + symbol);
        return false;
    }

    // Locate the containing block to know the maximum readable size.
    std::uint32_t block_size = 0;
    {
        const auto& sd = parser.side(pr.currentOVL);
        for (const auto& chk : sd.chunks) {
            for (const auto& blk : chk.blocks) {
                if (lf.loaderreference.datapointer >= blk.internal_offset &&
                    lf.loaderreference.datapointer < blk.internal_offset + blk.size) {
                    block_size = (blk.internal_offset + blk.size) - lf.loaderreference.datapointer;
                    break;
                }
            }
            if (block_size) break;
        }
    }

    BinaryReader r(parser.side(pr.currentOVL).ovlname);
    r.seek(pr.position);

    FtxHeader h{};
    if (!read_ftx_header(r, h, pr.position)) return false;

    if (!sane_dimensions(h.width, h.height)) {
        ctx.log("skip (bad dims " + std::to_string(h.width) + "x" +
                std::to_string(h.height) + "): " + symbol);
        return false;
    }

    // Read the entire header block (header + palette + small metadata).
    r.seek(pr.position);
    std::vector<std::byte> raw(block_size);
    for (std::uint32_t i = 0; i < block_size; ++i) {
        raw[i] = std::byte{r.read_u8()};
    }
    std::ofstream rawf(raw_out, std::ios::binary);
    rawf.write(reinterpret_cast<const char*>(raw.data()),
               static_cast<std::streamsize>(raw.size()));

    // All FTX textures (format codes 3-9) share the same on-disk layout:
    //   - 256-entry × 4-byte BGRA palette at offset 64 in the header block
    //   - 1-byte index per pixel at pixel_internal_offset (different chunk)
    // The format_code is a size class (2^code × 2^code); format=8 happens to
    // be the only one with non-power-of-2 dimensions allowed.
    std::vector<std::uint8_t> bgra;
    if (decode_indexed8(parser, h, raw, bgra)) {
        write_tga(out_dir / (base + ".tga"), h.width, h.height, bgra);
    } else {
        ctx.log("skip (palette decode failed): " + symbol);
    }

    write_json_sidecar(json_out, h, symbol, std::string(ldr.tag), block_size);

    ctx.log("wrote " + base + " (" +
            std::to_string(h.width) + "x" + std::to_string(h.height) +
            ", fmt=" + std::to_string(h.format) + ")");
    return true;
}

}  // namespace

ExtractResult TextureExtractor::extract(const OvlParser& parser, const ExtractContext& ctx) {
    ExtractResult res{};
    std::filesystem::create_directories(ctx.output_dir);

    for (std::size_t s = 0; s < (parser.has_unique() ? 2u : 1u); ++s) {
        auto side = static_cast<OvlSide>(s);
        const auto& d = parser.side(side);
        for (std::size_t i = 0; i < d.linkedfiles.size(); ++i) {
            try {
                if (extract_one(parser, side, i, ctx.output_dir, ctx.overwrite, ctx)) {
                    ++res.files_written;
                }
            } catch (const std::exception& e) {
                ++res.errors;
                ctx.log(std::string("error: ") + e.what());
            }
        }
    }
    return res;
}

bool TextureExtractor::extract_symbol(const OvlParser& parser,
                                      const std::string& symbol_lc,
                                      const ExtractContext& ctx) {
    auto match = [&](const std::string& s) {
        if (s.size() != symbol_lc.size()) return false;
        for (std::size_t i = 0; i < s.size(); ++i) {
            char c = static_cast<char>(std::tolower(
                static_cast<unsigned char>(s[i])));
            if (c != symbol_lc[i]) return false;
        }
        return true;
    };

    for (std::size_t s = 0; s < (parser.has_unique() ? 2u : 1u); ++s) {
        auto side = static_cast<OvlSide>(s);
        const auto& d = parser.side(side);
        for (std::size_t i = 0; i < d.linkedfiles.size(); ++i) {
            const auto& lf = d.linkedfiles[i];
            std::string sym = parser.string_from_offset(
                lf.symbolresolve.stringpointer);
            if (!match(sym)) continue;
            std::filesystem::create_directories(ctx.output_dir);
            return extract_one(parser, side, i, ctx.output_dir,
                               ctx.overwrite, ctx);
        }
    }
    return false;
}

}  // namespace ovl
