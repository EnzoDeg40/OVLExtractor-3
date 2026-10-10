#include "ovl/extract/AtlasExtractor.hpp"

#include "ovl/BinaryReader.hpp"
#include "ovl/Error.hpp"
#include "ovl/OvlParser.hpp"
#include "ovl/extract/TextureExtractor.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace ovl {

namespace {

struct DecodedTexture {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> bgra;  // row-major BGRA, top-down
};

struct Rect {
    std::int32_t left = 0, top = 0, right = 0, bottom = 0;
};

std::string sanitize(std::string s) {
    for (auto& c : s) {
        if (c == '/' || c == '\\' || c == ':' || c == '?' || c == '*' ||
            c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    }
    if (s.empty()) s = "unnamed";
    return s;
}

void write_tga(const std::filesystem::path& path,
               std::uint32_t width,
               std::uint32_t height,
               const std::vector<std::uint8_t>& bgra) {
    std::ofstream o(path, std::ios::binary);
    if (!o) return;
    std::uint8_t hdr[18] = {0};
    hdr[2]  = 2;
    hdr[12] = static_cast<std::uint8_t>(width & 0xFF);
    hdr[13] = static_cast<std::uint8_t>((width >> 8) & 0xFF);
    hdr[14] = static_cast<std::uint8_t>(height & 0xFF);
    hdr[15] = static_cast<std::uint8_t>((height >> 8) & 0xFF);
    hdr[16] = 32;
    hdr[17] = 0x28;
    o.write(reinterpret_cast<char*>(hdr), 18);
    o.write(reinterpret_cast<const char*>(bgra.data()),
            static_cast<std::streamsize>(bgra.size()));
}

// Decode the FTX whose header lives at `internal_offset` (shared decoder, docs
// §3). Transparency comes from the FTX alpha plane when there is one (the old
// "index 0 = transparent" chroma-key is gone: it was a stand-in for it).
bool decode_ftx_at(const OvlParser& parser,
                   OvlSide /*side*/,
                   std::uint64_t /*file_pos*/,
                   std::uint32_t internal_offset,
                   DecodedTexture& out) {
    return TextureExtractor::decode_ftx_bgra(parser, internal_offset, out.width,
                                             out.height, out.bgra);
}

// Decode any texture loader (ftx / tex / fts / ftt) to BGRA pixels.
// FTX layout has the full header at datapointer. TEX is a wrapper:
// 76 bytes of metadata at datapointer, with an internal offset at +0x34
// pointing to the actual FTX-style header elsewhere in the OVL.
bool decode_texture(const OvlParser& parser,
                    OvlSide side,
                    std::size_t lf_index,
                    std::string_view tag,
                    DecodedTexture& out) {
    const auto& d = parser.side(side);
    const auto& lf = d.linkedfiles[lf_index];

    auto pr = parser.offset_to_position(lf.loaderreference.datapointer);
    if (!pr.found) return false;

    if (tag == "tex") {
        // The page of the bitmap table this tex symbol maps to (docs §4.1):
        // one DXT / A8R8G8B8 sheet sliced by the gsi rects.
        return TextureExtractor::decode_tex_bgra(parser, side, lf_index, out.width,
                                                 out.height, out.bgra);
    }
    return decode_ftx_at(parser, pr.currentOVL, pr.position,
                         lf.loaderreference.datapointer, out);
}

// Read GSI binary: at datapointer, skip 8 bytes, read u32 = coord block offset.
// At coord block offset: read 4 × u32 = left, top, right, bottom (pixel space).
// Also returns the offset stored at gsi+0, which references the parent texture
// (resolved to a name via parser.datablock_name_from_offset).
// Find a SymbolResolve across both sides whose pointer field matches the given
// internal offset. Returns the stripped symbol name (or empty if not found).
// The texture reference for a gsi is a relocation slot — bytes on disk are 0,
// so we look it up via the resolves table instead of reading them.
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string symbol_at_pointer(const OvlParser& parser, std::uint32_t off) {
    for (int s = 0; s < 2; ++s) {
        const auto& d = parser.side(static_cast<OvlSide>(s));
        for (const auto& sr : d.symbolresolves) {
            if (sr.pointer == off) {
                std::string name = parser.string_from_offset(sr.stringpointer);
                auto cut = name.rfind(':');
                if (cut != std::string::npos) name = name.substr(0, cut);
                return name;
            }
        }
    }
    return {};
}

bool read_gsi(const OvlParser& parser,
              const LinkedFiles& lf,
              std::string& texture_name,
              Rect& rect) {
    auto pr = parser.offset_to_position(lf.loaderreference.datapointer);
    if (!pr.found) return false;
    BinaryReader r(parser.side(pr.currentOVL).ovlname);
    r.seek(pr.position + 8);
    std::uint32_t coord_off = r.read_u32();    // gsi+8 → coord block offset

    // Texture ref is a relocation slot — bytes on disk are 0, the runtime
    // would fill them from the symbol-resolves table. Look it up by pointer
    // location (try gsi+4 first per empirical inspection, fall back to gsi+0).
    texture_name = symbol_at_pointer(parser, lf.loaderreference.datapointer + 4);
    if (texture_name.empty())
        texture_name = symbol_at_pointer(parser, lf.loaderreference.datapointer);
    if (texture_name.empty()) return false;

    auto pc = parser.offset_to_position(coord_off);
    if (!pc.found) return false;
    BinaryReader r2(parser.side(pc.currentOVL).ovlname);
    r2.seek(pc.position);
    rect.left   = static_cast<std::int32_t>(r2.read_u32());
    rect.top    = static_cast<std::int32_t>(r2.read_u32());
    rect.right  = static_cast<std::int32_t>(r2.read_u32());
    rect.bottom = static_cast<std::int32_t>(r2.read_u32());
    return true;
}

void crop_bgra(const DecodedTexture& tex,
               const Rect& r,
               std::vector<std::uint8_t>& out,
               std::uint32_t& crop_w,
               std::uint32_t& crop_h) {
    std::int32_t l = std::max(0, r.left);
    std::int32_t t = std::max(0, r.top);
    std::int32_t rt = std::min<std::int32_t>(r.right, tex.width);
    std::int32_t b  = std::min<std::int32_t>(r.bottom, tex.height);
    if (rt <= l || b <= t) { crop_w = crop_h = 0; out.clear(); return; }
    crop_w = static_cast<std::uint32_t>(rt - l);
    crop_h = static_cast<std::uint32_t>(b - t);
    out.resize(static_cast<std::size_t>(crop_w) * crop_h * 4);
    for (std::uint32_t y = 0; y < crop_h; ++y) {
        const std::uint8_t* src = tex.bgra.data() +
            ((t + y) * tex.width + l) * 4;
        std::uint8_t* dst = out.data() + y * crop_w * 4;
        std::copy(src, src + crop_w * 4, dst);
    }
}

// Pass 1: collect all decoded textures from both sides into a single map
// keyed by symbol name (gsi loaders on side A may reference textures on B).
void collect_textures(const OvlParser& parser,
                      OvlSide side,
                      std::map<std::string, DecodedTexture>& textures,
                      std::size_t& seen,
                      std::size_t& decoded) {
    const auto& d = parser.side(side);
    for (std::size_t i = 0; i < d.linkedfiles.size(); ++i) {
        const auto& lf = d.linkedfiles[i];
        Loader ldr = parser.loader_by_id(lf.loaderreference.loadernumber, side);
        if (ldr.tag != "ftx" && ldr.tag != "tex" &&
            ldr.tag != "fts" && ldr.tag != "ftt") continue;
        ++seen;
        std::string sym = parser.string_from_offset(lf.symbolresolve.stringpointer);
        auto cut = sym.rfind(':');
        if (cut != std::string::npos) sym = sym.substr(0, cut);
        DecodedTexture tx;
        if (decode_texture(parser, side, i, ldr.tag, tx)) {
            textures[lower(sym)] = std::move(tx);  // symbols are case-insensitive
            ++decoded;
        }
    }
}

// Pass 2: iterate gsi loaders on one side, crop from the unified map.
bool process_gsi_side(const OvlParser& parser,
                      OvlSide side,
                      const std::map<std::string, DecodedTexture>& textures,
                      const ExtractContext& ctx,
                      ExtractResult& res) {
    const auto& d = parser.side(side);
    bool wrote_any = false;
    for (std::size_t i = 0; i < d.linkedfiles.size(); ++i) {
        const auto& lf = d.linkedfiles[i];
        Loader ldr = parser.loader_by_id(lf.loaderreference.loadernumber, side);
        if (ldr.tag != "gsi") continue;

        std::string gsi_sym = parser.string_from_offset(lf.symbolresolve.stringpointer);
        auto cut = gsi_sym.rfind(':');
        if (cut != std::string::npos) gsi_sym = gsi_sym.substr(0, cut);
        std::string base = sanitize(gsi_sym);

        std::string tex_name;
        Rect rect{};
        if (!read_gsi(parser, lf, tex_name, rect)) {
            ctx.log("gsi: read failed for " + gsi_sym);
            ++res.errors;
            continue;
        }

        auto it = textures.find(lower(tex_name));
        if (it == textures.end()) {
            ctx.log("gsi: texture '" + tex_name + "' not decoded (skipping " +
                    gsi_sym + ")");
            continue;
        }

        std::vector<std::uint8_t> cropped;
        std::uint32_t cw = 0, ch = 0;
        crop_bgra(it->second, rect, cropped, cw, ch);
        if (cw == 0 || ch == 0) {
            // A few of Main's gsi are degenerate on disk (right ≤ left or
            // bottom ≤ top): there is no sprite to write, and it isn't a
            // decode failure.
            ctx.log("gsi: empty rect for " + gsi_sym + " (skipped)");
            continue;
        }

        auto out_path = ctx.output_dir / (base + ".tga");
        if (!ctx.overwrite && std::filesystem::exists(out_path)) {
            ctx.log("skip (exists): " + base);
            continue;
        }
        std::filesystem::create_directories(ctx.output_dir);
        write_tga(out_path, cw, ch, cropped);
        ++res.files_written;
        wrote_any = true;
        ctx.log("wrote " + base + " (" + std::to_string(cw) + "x" +
                std::to_string(ch) + " from " + tex_name + ")");
    }
    return wrote_any;
}

}  // namespace

ExtractResult AtlasExtractor::extract(const OvlParser& parser,
                                      const ExtractContext& ctx) {
    ExtractResult res{};
    std::map<std::string, DecodedTexture> textures;
    std::size_t seen = 0, decoded = 0;
    collect_textures(parser, OvlSide::Common, textures, seen, decoded);
    if (parser.has_unique())
        collect_textures(parser, OvlSide::Unique, textures, seen, decoded);
    ctx.log("textures: " + std::to_string(decoded) + "/" +
            std::to_string(seen) + " decoded");
    if (textures.empty()) return res;
    process_gsi_side(parser, OvlSide::Common, textures, ctx, res);
    if (parser.has_unique())
        process_gsi_side(parser, OvlSide::Unique, textures, ctx, res);
    return res;
}

}  // namespace ovl
