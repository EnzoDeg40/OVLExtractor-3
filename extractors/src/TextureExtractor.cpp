#include "ovl/extract/TextureExtractor.hpp"

#include "ovl/BinaryReader.hpp"
#include "ovl/Error.hpp"
#include "ovl/OvlParser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace ovl {

namespace {

// FlexiTexture layout at loaderreference.datapointer (docs §3.1). This is the
// on-disk form of rct3dump's FlexiTextureInfoStruct + FlexiTextureStruct;
// every "ptr" field is an internal offset fixed up by the relocation table.
//   FlexiTextureInfoStruct (36 B)
//   +0x00 u32 scale          log2 of the size class (8 = arbitrary dims)
//   +0x04 u32 width
//   +0x08 u32 height
//   +0x0C u32 fps            animation rate (0 = static)
//   +0x10 u32 recolorable    bit 0/1/2 = 1st/2nd/3rd remap colour
//   +0x14 u32 offset_count   (1)
//   +0x18 ptr offsets        -> u32[offset_count] frame sequence
//   +0x1C u32 frame_count    (1)
//   +0x20 ptr frames         -> FlexiTextureStruct[frame_count]
//   FlexiTextureStruct (28 B each, normally right after the offsets array)
//   +0x00 u32 scale, +0x04 width, +0x08 height, +0x0C recolorable
//   +0x10 ptr palette        -> 256 × BGRA (normally inline right after)
//   +0x14 ptr texture        -> width*height u8 palette indices
//   +0x18 ptr alpha          -> width*height u8 alpha, or 0 if none
// For a single-frame texture this puts the palette at +0x44 — earlier
// versions of this extractor read it at +0x40 (the alpha pointer) and every
// colour came out shifted by one palette entry.
struct FtxHeader {
    std::uint32_t format = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t recolorable = 0;
    std::uint32_t frame_count = 0;
    std::uint32_t frames_ptr = 0;
    std::uint32_t palette_ptr = 0;
    std::uint32_t pixel_internal_offset = 0;   // index plane
    std::uint32_t alpha_ptr = 0;
};

bool read_ftx_header(const OvlParser& parser, std::uint32_t datapointer, FtxHeader& h) {
    auto pr = parser.offset_to_position(datapointer);
    if (!pr.found) return false;
    BinaryReader r(parser.side(pr.currentOVL).ovlname);
    r.seek(pr.position);
    h.format      = r.read_u32();
    h.width       = r.read_u32();
    h.height      = r.read_u32();
    r.read_u32();                       // fps
    h.recolorable = r.read_u32();
    r.read_u32();                       // offset_count
    r.read_u32();                       // offsets ptr
    h.frame_count = r.read_u32();
    h.frames_ptr  = r.read_u32();
    auto fp = parser.offset_to_position(h.frames_ptr);
    if (!fp.found) return false;
    BinaryReader fr(parser.side(fp.currentOVL).ovlname);
    fr.seek(fp.position + 0x10);
    h.palette_ptr           = fr.read_u32();
    h.pixel_internal_offset = fr.read_u32();
    h.alpha_ptr             = fr.read_u32();
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
      << "  \"recolorable\": " << h.recolorable << ",\n"
      << "  \"frame_count\": " << h.frame_count << ",\n"
      << "  \"has_alpha_plane\": " << (h.alpha_ptr ? "true" : "false") << ",\n"
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

// Read `n` bytes at internal offset `ptr`.
bool read_plane(const OvlParser& parser, std::uint32_t ptr, std::size_t n,
                std::vector<std::uint8_t>& out) {
    auto p = parser.offset_to_position(ptr);
    if (!p.found) return false;
    BinaryReader r(parser.side(p.currentOVL).ovlname);
    r.seek(p.position);
    out.resize(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = r.read_u8();
    return true;
}

// RCT3's FlexiColour recolour table (docs §3.8): 32 swatches × 85 ramp
// positions, RGB, captured from the running game.
const std::uint8_t kFlexiLut[32 * 85 * 3] = {
#include "FlexiColourLut.inc"
};

// Swap the remap bands of a recolourable palette (BGRA) for the ramps of the
// chosen swatches: band c (indices 1+85c … 85+85c) of a texture whose
// `recolorable` bit c is set takes kFlexiLut[recolor[c]][i - 1 - 85c].
// A channel value outside 0..31 leaves its band alone (the game's
// "no recolour" default is 32).
void apply_recolor(std::vector<std::uint8_t>& pal, std::uint32_t recolorable, const int* recolor) {
    if (!recolor) return;
    for (int c = 0; c < 3; ++c) {
        if (!(recolorable & (1u << c)) || recolor[c] < 0 || recolor[c] > 31) continue;
        const std::uint8_t* ramp = kFlexiLut + recolor[c] * 85 * 3;
        for (int i = 0; i < 85; ++i) {
            std::size_t idx = static_cast<std::size_t>(1 + 85 * c + i);
            pal[idx * 4 + 0] = ramp[i * 3 + 2];
            pal[idx * 4 + 1] = ramp[i * 3 + 1];
            pal[idx * 4 + 2] = ramp[i * 3 + 0];
        }
    }
}

// Decode an FTX frame: BGRA palette[index], alpha from the alpha plane when
// the texture has one (cut-outs: leaves, railings, signs…), else 255. The
// palette's 4th byte is not alpha (always 0) and is ignored.
bool decode_indexed8(const OvlParser& parser, const FtxHeader& h,
                     std::vector<std::uint8_t>& bgra_out, bool& has_alpha,
                     const int* recolor = nullptr) {
    std::size_t pixels = static_cast<std::size_t>(h.width) * h.height;
    if (pixels == 0) return false;
    std::vector<std::uint8_t> pal, indices, alpha;
    if (!read_plane(parser, h.palette_ptr, 256 * 4, pal)) return false;
    apply_recolor(pal, h.recolorable, recolor);
    if (!read_plane(parser, h.pixel_internal_offset, pixels, indices)) return false;
    has_alpha = h.alpha_ptr != 0 && read_plane(parser, h.alpha_ptr, pixels, alpha);
    bgra_out.resize(pixels * 4);
    for (std::size_t i = 0; i < pixels; ++i) {
        std::uint8_t idx = indices[i];
        bgra_out[i * 4 + 0] = pal[idx * 4 + 0];
        bgra_out[i * 4 + 1] = pal[idx * 4 + 1];
        bgra_out[i * 4 + 2] = pal[idx * 4 + 2];
        bgra_out[i * 4 + 3] = has_alpha ? alpha[i] : 255;
    }
    return true;
}

bool sane_dimensions(std::uint32_t w, std::uint32_t h) {
    return w > 0 && w <= 8192 && h > 0 && h <= 8192;
}

// A `tex` page, located through the tex → flic → btbl chain (docs §4.1).
//
// The `tex` wrapper (TextureStruct, 96 B) carries at +0x34 a `FlicStruct**`:
// the internal offset of the pointer slot that precedes a FlicStruct inside
// the `btbl` block ({unk, count} then count × {ptr, FlicStruct{Texture*,
// 1, 1.0f}}). The FlicStruct is the data pointer of a `flic` loader
// reference, and that loader's single extra-data chunk is a u32 index into the
// bitmap table — the `btbl` loader that precedes it in loader order. The
// btbl's data block is BmpTbl{unk, count} followed by the count pointer +
// FlicStruct pairs; its first extra chunk is {0, 0, count × FlicHeader{format,
// width, height, mips}}, its second chunk the count payloads back to back,
// each a contiguous mip chain sized from its header.
//
// `flic` loaders of version 2 carry their pixels inline instead: one chunk
// holding a FlicHeader, then per level a FlicMipHeader{w, h, pitch, blocks}
// and pitch × blocks bytes (rct3dump's ReadTexture).
struct TexPage {
    std::uint32_t format_code = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipmap_count = 0;
    OvlSide       side = OvlSide::Common;  // side whose file holds the bytes
    std::uint64_t mip0_pos = 0;            // absolute file offset of level 0
    std::uint32_t mip0_size = 0;           // bytes of level 0 as stored
    std::uint32_t pitch = 0;               // stored row stride (0 = packed)
    std::uint32_t table_index = 0;         // slot in the btbl (inline: 0)
    bool          inline_layout = false;   // flic v2 per-mip headers
};

// RCT3 reuses Direct3D format codes (docs §4.4). These are the ones the game's
// bitmap tables use: the DXT family for scenery and GUI pages, A8R8G8B8 for a
// few uncompressed ones.
enum : std::uint32_t {
    kFmtA8R8G8B8 = 0x02, kFmtDXT1 = 0x12, kFmtDXT3 = 0x13, kFmtDXT5 = 0x14
};

// Bytes per 4×4 block for a DXT format code: 8 for DXT1 (BC1, 4 bpp), 16 for
// DXT3/DXT5 (BC2/BC3, 8 bpp), 0 for anything that is not block-compressed.
std::uint32_t dxt_block_bytes(std::uint32_t format_code) {
    switch (format_code) {
        case kFmtDXT1: return 8;
        case kFmtDXT3:
        case kFmtDXT5: return 16;
        default:       return 0;
    }
}

bool format_supported(std::uint32_t format_code) {
    return dxt_block_bytes(format_code) != 0 || format_code == kFmtA8R8G8B8;
}

const char* format_name(std::uint32_t format_code) {
    switch (format_code) {
        case kFmtDXT1:     return "DXT1";
        case kFmtDXT3:     return "DXT3";
        case kFmtDXT5:     return "DXT5";
        case kFmtA8R8G8B8: return "A8R8G8B8";
        default:           return "fmt?";
    }
}

std::string hex_u32(std::uint32_t v) {
    std::ostringstream os;
    os << "0x" << std::hex << v;
    return os.str();
}

// Bytes one mip level occupies when tightly packed: whole 4×4 blocks for the
// DXT formats (levels smaller than 4×4 still take one block), 4 B/px for
// A8R8G8B8. 0 for an unsupported format.
std::uint64_t level_size(std::uint32_t format_code, std::uint32_t w, std::uint32_t h) {
    if (std::uint32_t bb = dxt_block_bytes(format_code))
        return static_cast<std::uint64_t>((w + 3) / 4) * ((h + 3) / 4) * bb;
    if (format_code == kFmtA8R8G8B8) return static_cast<std::uint64_t>(w) * h * 4;
    return 0;
}

// Total bytes of a packed mip chain (rct3dump's ReadTextures sizing).
std::uint64_t chain_size(std::uint32_t format_code, std::uint32_t w, std::uint32_t h,
                         std::uint32_t mips) {
    std::uint64_t total = 0;
    for (std::uint32_t i = 0; i < mips; ++i)
        total += level_size(format_code, std::max(1u, w >> i), std::max(1u, h >> i));
    return total;
}

std::uint32_t read_u32_at(const OvlParser& parser, OvlSide side, std::uint64_t pos) {
    BinaryReader r(parser.side(side).ovlname);
    r.seek(pos);
    return r.read_u32();
}

// Loader reference (side, index) whose tag is `tag` and whose data pointer is
// `datapointer`, searched on both sides. Returns false if none.
bool find_loader_ref(const OvlParser& parser, const char* tag, std::uint32_t datapointer,
                     OvlSide& side_out, std::size_t& index_out) {
    for (std::size_t s = 0; s < (parser.has_unique() ? 2u : 1u); ++s) {
        auto side = static_cast<OvlSide>(s);
        const auto& d = parser.side(side);
        for (std::size_t i = 0; i < d.loaderreference.size(); ++i) {
            if (d.loaderreference[i].datapointer != datapointer) continue;
            if (parser.loader_by_id(d.loaderreference[i].loadernumber, side).tag != tag) continue;
            side_out = side;
            index_out = i;
            return true;
        }
    }
    return false;
}

// Follow the chain from the `tex` linked file `lf_index` declared on `side`
// to the page that holds its pixels. `why` receives the reason on failure.
bool resolve_tex_page(const OvlParser& parser, OvlSide side, std::size_t lf_index,
                      TexPage& page, std::string& why) {
    const auto& lf = parser.side(side).linkedfiles[lf_index];
    auto pr = parser.offset_to_position(lf.loaderreference.datapointer);
    if (!pr.found) { why = "tex wrapper unresolved"; return false; }
    std::uint32_t flicpp = read_u32_at(parser, pr.currentOVL, pr.position + 0x34);
    if (flicpp == 0 || !parser.offset_to_position(flicpp).found) {
        why = "no FlicStruct pointer at +0x34";
        return false;
    }

    // The slot holds the FlicStruct's offset on disk (the relocation table
    // only marks it); normally that is the next word, but a few tables pad
    // an entry (Sky's CloudsB), so follow the value and keep +4 as fallback.
    auto sp = parser.offset_to_position(flicpp);
    std::uint32_t flic_off = read_u32_at(parser, sp.currentOVL, sp.position);
    if (flic_off == 0) flic_off = flicpp + 4;

    OvlSide fside{};
    std::size_t fidx = 0;
    if (!find_loader_ref(parser, "flic", flic_off, fside, fidx) &&
        !find_loader_ref(parser, "flic", flicpp + 4, fside, fidx)) {
        why = "no flic loader at " + hex_u32(flic_off);
        return false;
    }
    const auto& fd = parser.side(fside);
    const auto& fchunks = fd.extradata[fidx];
    if (fchunks.empty()) { why = "flic has no extra data"; return false; }
    page.side = fside;

    std::uint32_t flic_version = parser.loader_by_id(fd.loaderreference[fidx].loadernumber, fside).type;
    if (flic_version == 2) {
        // Inline layout: FlicHeader, then {FlicMipHeader, pixels} per level.
        const auto& ch = fchunks[0];
        if (ch.size < 32) { why = "inline flic chunk too small"; return false; }
        BinaryReader r(fd.ovlname);
        r.seek(ch.position);
        page.format_code  = r.read_u32();
        page.width        = r.read_u32();
        page.height       = r.read_u32();
        page.mipmap_count = r.read_u32();
        std::uint32_t mw = r.read_u32(), mh = r.read_u32();
        page.pitch        = r.read_u32();
        std::uint32_t blocks = r.read_u32();
        page.inline_layout = true;
        page.mip0_pos  = ch.position + 32;
        page.mip0_size = page.pitch * blocks;
        if (mw != page.width || mh != page.height || page.mip0_size == 0 ||
            32 + static_cast<std::uint64_t>(page.mip0_size) > ch.size) {
            why = "inline flic level 0 does not match its header";
            return false;
        }
        return true;
    }

    // Bitmap-table layout: the flic's chunk is the slot index into the btbl
    // that precedes it in loader order on the same side.
    if (fchunks[0].size < 4) { why = "flic index chunk too small"; return false; }
    page.table_index = read_u32_at(parser, fside, fchunks[0].position);
    std::size_t bidx = fidx;
    bool found = false;
    while (bidx-- > 0) {
        if (parser.loader_by_id(fd.loaderreference[bidx].loadernumber, fside).tag == "btbl") {
            found = true;
            break;
        }
    }
    if (!found) { why = "no btbl before the flic"; return false; }
    const auto& bchunks = fd.extradata[bidx];
    if (bchunks.size() < 2) { why = "btbl lacks its two chunks"; return false; }
    // The slot count is the BmpTbl {unk, count} at the btbl's data pointer;
    // the chunk repeats the two words (zeroed) before the FlicHeader array.
    auto bp = parser.offset_to_position(fd.loaderreference[bidx].datapointer);
    if (!bp.found) { why = "btbl data unresolved"; return false; }
    std::uint32_t count = read_u32_at(parser, bp.currentOVL, bp.position + 4);
    BinaryReader r(fd.ovlname);
    if (page.table_index >= count || 8 + 16ull * count > bchunks[0].size) {
        why = "btbl index " + std::to_string(page.table_index) + " out of " + std::to_string(count);
        return false;
    }
    std::uint64_t payload = 0;
    for (std::uint32_t i = 0; i <= page.table_index; ++i) {
        r.seek(bchunks[0].position + 8 + 16ull * i);
        std::uint32_t fmt = r.read_u32(), w = r.read_u32(), h = r.read_u32(), mips = r.read_u32();
        if (!format_supported(fmt)) {
            why = "unsupported format " + hex_u32(fmt) + " in btbl slot " + std::to_string(i);
            return false;
        }
        if (i == page.table_index) {
            page.format_code  = fmt;
            page.width        = w;
            page.height       = h;
            page.mipmap_count = mips;
            page.mip0_pos     = bchunks[1].position + payload;
            page.mip0_size    = static_cast<std::uint32_t>(level_size(fmt, w, h));
        }
        payload += chain_size(fmt, w, h, mips);
    }
    if (payload > bchunks[1].size) { why = "btbl payload shorter than its headers claim"; return false; }
    return true;
}

// Build the 4-entry BGRA colour palette (alpha 255) from a DXT colour
// sub-block's two RGB565 endpoints. For DXT1, endpoint order selects the block
// mode: when c0 <= c1 the 4th entry is transparent black ("punch-through"
// 1-bit alpha). DXT3/DXT5 always use the 4-colour interpolation regardless of
// endpoint order, so pass punch_through=false for them.
void build_color_palette(std::uint16_t c0, std::uint16_t c1,
                         std::uint8_t pal[4][4], bool punch_through) {
    auto unpack_565 = [](std::uint16_t v, std::uint8_t rgb[3]) {
        std::uint8_t r = (v >> 11) & 0x1F;
        std::uint8_t g = (v >> 5)  & 0x3F;
        std::uint8_t b =  v        & 0x1F;
        rgb[0] = static_cast<std::uint8_t>((r << 3) | (r >> 2));
        rgb[1] = static_cast<std::uint8_t>((g << 2) | (g >> 4));
        rgb[2] = static_cast<std::uint8_t>((b << 3) | (b >> 2));
    };
    std::uint8_t rgb0[3], rgb1[3];
    unpack_565(c0, rgb0);
    unpack_565(c1, rgb1);
    // pal is BGRA; rgb is RGB, so channel k of pal maps to rgb[2 - k].
    pal[0][0] = rgb0[2]; pal[0][1] = rgb0[1]; pal[0][2] = rgb0[0]; pal[0][3] = 255;
    pal[1][0] = rgb1[2]; pal[1][1] = rgb1[1]; pal[1][2] = rgb1[0]; pal[1][3] = 255;
    if (!punch_through || c0 > c1) {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = static_cast<std::uint8_t>((2 * rgb0[2 - k] + rgb1[2 - k]) / 3);
            pal[3][k] = static_cast<std::uint8_t>((rgb0[2 - k] + 2 * rgb1[2 - k]) / 3);
        }
        pal[2][3] = 255;
        pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; ++k)
            pal[2][k] = static_cast<std::uint8_t>((rgb0[2 - k] + rgb1[2 - k]) / 2);
        pal[2][3] = 255;
        pal[3][0] = 0; pal[3][1] = 0; pal[3][2] = 0; pal[3][3] = 0;
    }
}

// Decode one DXT/BC mipmap level (image `w` × `h`) into BGRA pixels at `out`
// (sized w * h * 4). format_code: 0x12 = DXT1 (BC1), 0x13 = DXT3 (BC2),
// 0x14 = DXT5 (BC3). DXT3/DXT5 blocks are 16 bytes — 8 bytes of alpha then an
// 8-byte colour block; DXT1 blocks are 8 bytes of colour only.
void decode_dxt_level(const std::uint8_t* data, std::uint32_t w, std::uint32_t h,
                      std::uint32_t format_code, std::uint8_t* out) {
    const bool has_alpha_block = (format_code != kFmtDXT1);
    const std::uint32_t block_bytes = has_alpha_block ? 16u : 8u;
    const std::uint8_t* blk = data;
    for (std::uint32_t by = 0; by < h; by += 4) {
        for (std::uint32_t bx = 0; bx < w; bx += 4) {
            const std::uint8_t* alpha = has_alpha_block ? blk : nullptr;
            const std::uint8_t* color = has_alpha_block ? blk + 8 : blk;

            std::uint16_t c0 = static_cast<std::uint16_t>(color[0] | (color[1] << 8));
            std::uint16_t c1 = static_cast<std::uint16_t>(color[2] | (color[3] << 8));
            std::uint32_t idx = static_cast<std::uint32_t>(color[4]) |
                                (static_cast<std::uint32_t>(color[5]) << 8) |
                                (static_cast<std::uint32_t>(color[6]) << 16) |
                                (static_cast<std::uint32_t>(color[7]) << 24);

            std::uint8_t pal[4][4];  // [palette_idx][BGRA]
            build_color_palette(c0, c1, pal, /*punch_through=*/format_code == kFmtDXT1);

            // DXT5: 8-entry interpolated alpha ramp + 48 bits of 3-bit indices.
            std::uint8_t a8[8] = {0};
            std::uint64_t abits = 0;
            if (format_code == kFmtDXT5) {
                a8[0] = alpha[0];
                a8[1] = alpha[1];
                if (a8[0] > a8[1]) {
                    for (int i = 1; i <= 6; ++i)
                        a8[1 + i] = static_cast<std::uint8_t>(
                            ((7 - i) * a8[0] + i * a8[1]) / 7);
                } else {
                    for (int i = 1; i <= 4; ++i)
                        a8[1 + i] = static_cast<std::uint8_t>(
                            ((5 - i) * a8[0] + i * a8[1]) / 5);
                    a8[6] = 0;
                    a8[7] = 255;
                }
                for (int i = 0; i < 6; ++i)
                    abits |= static_cast<std::uint64_t>(alpha[2 + i]) << (8 * i);
            }

            for (int i = 0; i < 16; ++i) {
                std::uint32_t px = bx + (i % 4);
                std::uint32_t py = by + (i / 4);
                if (px >= w || py >= h) continue;
                std::uint8_t which = static_cast<std::uint8_t>((idx >> (2 * i)) & 0x3);
                std::size_t o = (static_cast<std::size_t>(py) * w + px) * 4;
                out[o + 0] = pal[which][0];
                out[o + 1] = pal[which][1];
                out[o + 2] = pal[which][2];
                std::uint8_t a = pal[which][3];  // DXT1: 255, or 0 (punch-through)
                if (format_code == kFmtDXT3) {
                    std::uint8_t byte = alpha[i / 2];
                    std::uint8_t nib = (i & 1) ? static_cast<std::uint8_t>(byte >> 4)
                                               : static_cast<std::uint8_t>(byte & 0x0F);
                    a = static_cast<std::uint8_t>(nib * 17);  // 0..15 → 0..255
                } else if (format_code == kFmtDXT5) {
                    a = a8[(abits >> (3 * i)) & 0x7];
                }
                out[o + 3] = a;
            }
            blk += block_bytes;
        }
    }
}

// Pull `n` bytes from `r` into a fresh vector. Helper for the tex path.
std::vector<std::uint8_t> read_n(BinaryReader& r, std::size_t n) {
    std::vector<std::uint8_t> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = r.read_u8();
    return out;
}

// Decode level 0 of a resolved page to BGRA (row-major, top-down as stored).
bool decode_page(const OvlParser& parser, const TexPage& page,
                 std::vector<std::uint8_t>& bgra) {
    if (!sane_dimensions(page.width, page.height)) return false;
    BinaryReader r(parser.side(page.side).ovlname);
    r.seek(page.mip0_pos);
    auto stored = read_n(r, page.mip0_size);

    std::uint32_t bb = dxt_block_bytes(page.format_code);
    std::uint32_t rows   = bb ? (page.height + 3) / 4 : page.height;
    std::uint32_t packed = bb ? ((page.width + 3) / 4) * bb : page.width * 4;
    const std::uint8_t* src = stored.data();
    std::vector<std::uint8_t> repacked;
    if (page.inline_layout && page.pitch != packed) {
        // Pitch-aware copy (rct3dump's ReadTexture): keep the first `packed`
        // bytes of every stored row.
        if (page.pitch < packed) return false;
        repacked.resize(static_cast<std::size_t>(rows) * packed);
        for (std::uint32_t y = 0; y < rows; ++y)
            std::copy(src + y * page.pitch, src + y * page.pitch + packed,
                      repacked.data() + y * packed);
        src = repacked.data();
    } else if (stored.size() < static_cast<std::size_t>(rows) * packed) {
        return false;
    }

    bgra.assign(static_cast<std::size_t>(page.width) * page.height * 4, 0);
    if (bb) {
        decode_dxt_level(src, page.width, page.height, page.format_code, bgra.data());
    } else {
        // A8R8G8B8 little-endian is B, G, R, A in memory: already our layout.
        std::copy(src, src + bgra.size(), bgra.data());
    }
    return true;
}

// Handle a `tex` linkedfile. Returns true if a .tga was produced.
bool extract_tex(const OvlParser& parser,
                 OvlSide side,
                 std::size_t lf_index,
                 const std::string& symbol,
                 const std::filesystem::path& out_path,
                 const ExtractContext& ctx) {
    TexPage page;
    std::string why;
    if (!resolve_tex_page(parser, side, lf_index, page, why)) {
        ctx.log("tex: " + symbol + " skipped (" + why + ")");
        return false;
    }
    std::vector<std::uint8_t> bgra;
    if (!decode_page(parser, page, bgra)) {
        ctx.log("tex: " + symbol + " page unreadable (" +
                std::to_string(page.width) + "x" + std::to_string(page.height) +
                " " + format_name(page.format_code) + ")");
        return false;
    }
    write_tga(out_path, page.width, page.height, bgra);
    ctx.log("wrote " + out_path.filename().string() + " (" +
            std::to_string(page.width) + "x" + std::to_string(page.height) +
            ", " + format_name(page.format_code) + ", " +
            std::to_string(page.mipmap_count) + " mips, " +
            (page.inline_layout ? std::string("inline") :
                                  "btbl slot " + std::to_string(page.table_index)) + ")");
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

    // tex is structurally different from ftx: the pixels sit in a bitmap
    // table reached through the tex → flic → btbl chain (docs §4.1). It gets
    // a dedicated decoder and no .ovltex / .json sidecar.
    if (ldr.tag == "tex") {
        std::filesystem::create_directories(out_dir);
        auto tga_out = out_dir / (base + ".tga");
        if (!overwrite && std::filesystem::exists(tga_out)) {
            ctx.log("skip (exists): " + base);
            return true;
        }
        return extract_tex(parser, side, lf_index, symbol, tga_out, ctx);
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

    FtxHeader h{};
    if (!read_ftx_header(parser, lf.loaderreference.datapointer, h)) {
        ctx.log("skip (ftx header unresolved): " + symbol);
        return false;
    }

    if (!sane_dimensions(h.width, h.height)) {
        ctx.log("skip (bad dims " + std::to_string(h.width) + "x" +
                std::to_string(h.height) + "): " + symbol);
        return false;
    }

    // Keep the raw header block (header + palette + small metadata) for
    // offline RE.
    BinaryReader r(parser.side(pr.currentOVL).ovlname);
    r.seek(pr.position);
    std::vector<std::byte> raw(block_size);
    for (std::uint32_t i = 0; i < block_size; ++i) {
        raw[i] = std::byte{r.read_u8()};
    }
    std::ofstream rawf(raw_out, std::ios::binary);
    rawf.write(reinterpret_cast<const char*>(raw.data()),
               static_cast<std::streamsize>(raw.size()));

    // All FTX textures (format codes 3-9) share the same layout: indexed8 +
    // BGRA palette (+ optional alpha plane), reached through the frame
    // struct's pointers. Only frame 0 is decoded for animated flexitextures.
    std::vector<std::uint8_t> bgra;
    bool has_alpha = false;
    if (decode_indexed8(parser, h, bgra, has_alpha)) {
        // <name>.tga is opaque: the alpha plane is only meaningful where an
        // alpha-tested material samples the texture (elsewhere it is often 0),
        // so baking it in would punch holes in opaque surfaces for any viewer
        // that honours alpha. The alpha-carrying copy goes to <name>_rgba.tga.
        if (has_alpha) write_tga(out_dir / (base + "_rgba.tga"), h.width, h.height, bgra);
        for (std::size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 255;
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

bool TextureExtractor::decode_ftx_bgra(const OvlParser& parser,
                                       std::uint32_t datapointer,
                                       std::uint32_t& width,
                                       std::uint32_t& height,
                                       std::vector<std::uint8_t>& bgra,
                                       bool* has_alpha,
                                       std::uint32_t* recolorable,
                                       const int* recolor) {
    FtxHeader h{};
    if (!read_ftx_header(parser, datapointer, h)) return false;
    if (!sane_dimensions(h.width, h.height)) return false;
    bool alpha = false;
    if (!decode_indexed8(parser, h, bgra, alpha, recolor)) return false;
    width = h.width;
    height = h.height;
    if (has_alpha) *has_alpha = alpha;
    if (recolorable) *recolorable = h.recolorable;
    return true;
}

bool TextureExtractor::decode_linked_bgra(const OvlParser& parser, OvlSide side,
                                          std::size_t lf_index,
                                          std::uint32_t& width,
                                          std::uint32_t& height,
                                          std::vector<std::uint8_t>& bgra,
                                          bool* has_alpha,
                                          std::uint32_t* recolorable,
                                          const int* recolor) {
    const auto& lf = parser.side(side).linkedfiles[lf_index];
    Loader ldr = parser.loader_by_id(lf.loaderreference.loadernumber, side);
    if (has_alpha) *has_alpha = false;
    if (recolorable) *recolorable = 0;
    if (ldr.tag == "ftx")
        return decode_ftx_bgra(parser, lf.loaderreference.datapointer, width,
                               height, bgra, has_alpha, recolorable, recolor);
    if (ldr.tag == "tex") {
        if (!decode_tex_bgra(parser, side, lf_index, width, height, bgra)) return false;
        // DXT3/DXT5 carry real alpha; DXT1 decodes to opaque 255.
        if (has_alpha) {
            for (std::size_t i = 3; i < bgra.size(); i += 4)
                if (bgra[i] != 255) { *has_alpha = true; break; }
        }
        return true;
    }
    return false;
}

bool TextureExtractor::decode_tex_bgra(const OvlParser& parser,
                                       OvlSide declaring_side,
                                       std::size_t lf_index,
                                       std::uint32_t& width,
                                       std::uint32_t& height,
                                       std::vector<std::uint8_t>& bgra) {
    TexPage page;
    std::string why;
    if (!resolve_tex_page(parser, declaring_side, lf_index, page, why)) return false;
    if (!decode_page(parser, page, bgra)) return false;
    width  = page.width;
    height = page.height;
    return true;
}

}  // namespace ovl
