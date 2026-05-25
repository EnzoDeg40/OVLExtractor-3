#include "ovl/extract/TextureExtractor.hpp"

#include "ovl/BinaryReader.hpp"
#include "ovl/Error.hpp"
#include "ovl/OvlParser.hpp"

#include <algorithm>
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

// Pull pixel bytes from the OVL file starting at the given internal offset,
// reading up to max_bytes (or until the containing block ends, whichever first).
std::vector<std::byte> read_pixels(const OvlParser& parser,
                                   std::uint32_t internal_offset,
                                   std::uint32_t max_bytes) {
    std::vector<std::byte> buf;
    auto pr = parser.offset_to_position(internal_offset);
    if (!pr.found) return buf;

    // Find the containing block to clamp read size to its end.
    std::uint32_t bytes_available = max_bytes;
    const auto& d = parser.side(pr.currentOVL);
    for (std::size_t i = 0; i < d.chunks.size(); ++i) {
        for (const auto& blk : d.chunks[i].blocks) {
            if (internal_offset >= blk.internal_offset &&
                internal_offset < blk.internal_offset + blk.size) {
                std::uint32_t in_block = internal_offset - blk.internal_offset;
                std::uint32_t left = blk.size - in_block;
                bytes_available = std::min(bytes_available, left);
                break;
            }
        }
    }
    if (bytes_available == 0) return buf;

    BinaryReader r(d.ovlname);
    r.seek(pr.position);
    buf.resize(bytes_available);
    for (std::uint32_t i = 0; i < bytes_available; ++i) {
        buf[i] = std::byte{r.read_u8()};
    }
    return buf;
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

    // Palette starts at offset 64 in the header block. Byte order: B G R A.
    // Index 0 is treated as chroma-key transparent (alpha=0), all other
    // indices are opaque — common pattern in early-2000s palette textures.
    const std::byte* pal = raw_header_block.data() + kPaletteOffset;
    bgra_out.resize(pixels * 4);
    for (std::size_t i = 0; i < pixels; ++i) {
        std::uint8_t idx = indices[i];
        std::uint8_t b = std::to_integer<std::uint8_t>(pal[idx * 4 + 0]);
        std::uint8_t g = std::to_integer<std::uint8_t>(pal[idx * 4 + 1]);
        std::uint8_t r = std::to_integer<std::uint8_t>(pal[idx * 4 + 2]);
        std::uint8_t a = (idx == 0) ? 0 : 255;
        bgra_out[i * 4 + 0] = b;
        bgra_out[i * 4 + 1] = g;
        bgra_out[i * 4 + 2] = r;
        bgra_out[i * 4 + 3] = a;
    }
    return true;
}

bool sane_dimensions(std::uint32_t w, std::uint32_t h) {
    return w > 0 && w <= 8192 && h > 0 && h <= 8192;
}

// Decode a raw A8 pixel stream (1 byte per pixel, grayscale or alpha mask)
// into BGRA suitable for TGA output. Returns false if not enough data.
bool decode_a8(const FtxHeader& h,
               const std::vector<std::byte>& pixels,
               std::vector<std::uint8_t>& bgra_out) {
    const std::size_t n = static_cast<std::size_t>(h.width) * h.height;
    if (pixels.size() < n) return false;
    bgra_out.resize(n * 4);
    for (std::size_t i = 0; i < n; ++i) {
        std::uint8_t v = std::to_integer<std::uint8_t>(pixels[i]);
        bgra_out[i*4+0] = v;
        bgra_out[i*4+1] = v;
        bgra_out[i*4+2] = v;
        bgra_out[i*4+3] = 255;
    }
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

    // Format-8 (indexed8 + palette) → decode to TGA.
    if (h.format == 8) {
        std::vector<std::uint8_t> bgra;
        if (decode_indexed8(parser, h, raw, bgra)) {
            auto tga_out = out_dir / (base + ".tga");
            write_tga(tga_out, h.width, h.height, bgra);
        }
    } else {
        // Non-format-8 (codes 4, 5, 6, 7, 9): all empirically validated as A8
        // grayscale alpha masks. The format_code appears to encode a size
        // class (texture is 2^code × 2^code), not a pixel layout — the game
        // shader applies tint to the monochrome mask at render time.
        // Pixel data lives at pixel_internal_offset (separate chunk from the
        // header). mipmap_count read from the header is garbage for these
        // formats, so we just emit level 0.
        FtxHeader h0 = h;
        h0.mipmap_count = 1;
        std::uint32_t cap = h0.width * h0.height;
        auto pixels = read_pixels(parser, h0.pixel_internal_offset, cap);
        if (pixels.empty()) {
            ctx.log("skip (no pixel data at offset " +
                    std::to_string(h0.pixel_internal_offset) + "): " + symbol);
        } else {
            std::vector<std::uint8_t> a8;
            if (decode_a8(h0, pixels, a8)) {
                write_tga(out_dir / (base + ".tga"), h0.width, h0.height, a8);
            } else {
                ctx.log("skip (insufficient A8 data " +
                        std::to_string(pixels.size()) + "/" +
                        std::to_string(cap) + "): " + symbol);
            }
        }
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

}  // namespace ovl
