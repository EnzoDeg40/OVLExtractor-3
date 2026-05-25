#include "ovl/extract/SoundExtractor.hpp"

#include "ovl/BinaryReader.hpp"
#include "ovl/Error.hpp"
#include "ovl/OvlParser.hpp"
#include "ovl/SFStructs.hpp"

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

// Layout discovered empirically on Sounds/Sounds.common.ovl :
//   At loaderreference.datapointer (resolved via offset_to_position) lies a
//   80-byte block matching SidSound (cf. SFStructs.hpp), immediately followed
//   by the PCM payload at offsets `channel1` / `channel2`.
struct SidSoundHeader {
    std::uint16_t fmt_tag;            // 1 = PCM
    std::uint16_t numchannels;
    std::uint32_t samplerate;
    std::uint32_t byterate;
    std::uint16_t blockalign;
    std::uint16_t bitspersample;
    // 64 bytes of unknown metadata follow before channel1.
    std::uint32_t channel1;           // internal offset to PCM channel 1
    std::int32_t  channel1_size;
    std::uint32_t channel2;           // internal offset to PCM channel 2 (0 if mono)
    std::int32_t  channel2_size;
};

bool read_sid_header(BinaryReader& r, SidSoundHeader& h) {
    h.fmt_tag       = r.read_u16();
    h.numchannels   = r.read_u16();
    h.samplerate    = r.read_u32();
    h.byterate      = r.read_u32();
    h.blockalign    = r.read_u16();
    h.bitspersample = r.read_u16();
    // After fmt (16 bytes) the struct has 48 bytes of unknown metadata
    // (unk1+pad, unk2-unk11, loop) before reaching channel1 at offset 64.
    // Skipping 48 puts us exactly at channel1. PCM data follows at offset 80.
    r.skip(48);
    h.channel1      = r.read_u32();
    h.channel1_size = r.read_i32();
    h.channel2      = r.read_u32();
    h.channel2_size = r.read_i32();
    return true;
}

std::string sanitize_filename(std::string s) {
    for (auto& c : s) {
        if (c == '/' || c == '\\' || c == ':' || c == '?' || c == '*' ||
            c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    }
    if (s.empty()) s = "unnamed";
    return s;
}

void write_le_u32(std::ostream& o, std::uint32_t v) {
    char b[4] = {static_cast<char>(v & 0xFF),
                 static_cast<char>((v >> 8) & 0xFF),
                 static_cast<char>((v >> 16) & 0xFF),
                 static_cast<char>((v >> 24) & 0xFF)};
    o.write(b, 4);
}

void write_le_u16(std::ostream& o, std::uint16_t v) {
    char b[2] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
    o.write(b, 2);
}

void write_wav(const std::filesystem::path& out,
               std::uint16_t numchannels,
               std::uint32_t samplerate,
               std::uint16_t bitspersample,
               const std::vector<std::byte>& pcm) {
    std::ofstream o(out, std::ios::binary);
    if (!o) throw OvlError("SoundExtractor: cannot write " + out.string());

    std::uint16_t blockalign = static_cast<std::uint16_t>(numchannels * (bitspersample / 8));
    std::uint32_t byterate   = samplerate * blockalign;
    std::uint32_t data_size  = static_cast<std::uint32_t>(pcm.size());
    std::uint32_t riff_size  = 36 + data_size;

    o.write("RIFF", 4);
    write_le_u32(o, riff_size);
    o.write("WAVE", 4);

    o.write("fmt ", 4);
    write_le_u32(o, 16);                   // fmt chunk size
    write_le_u16(o, 1);                    // PCM
    write_le_u16(o, numchannels);
    write_le_u32(o, samplerate);
    write_le_u32(o, byterate);
    write_le_u16(o, blockalign);
    write_le_u16(o, bitspersample);

    o.write("data", 4);
    write_le_u32(o, data_size);
    o.write(reinterpret_cast<const char*>(pcm.data()),
            static_cast<std::streamsize>(pcm.size()));
}

// Read raw PCM bytes from the OVL file. The channel offset is an internal
// OVL offset; we resolve it via the parser to a file position and read.
std::vector<std::byte> read_channel_pcm(const OvlParser& parser,
                                        std::uint32_t channel_offset,
                                        std::uint32_t channel_size) {
    std::vector<std::byte> buf;
    if (channel_offset == 0 || channel_size == 0) return buf;
    auto pr = parser.offset_to_position(channel_offset);
    if (!pr.found) return buf;
    BinaryReader r(parser.side(pr.currentOVL).ovlname);
    r.seek(pr.position);
    buf.resize(channel_size);
    // BinaryReader has no raw read, so loop. (Hot path - could be optimized.)
    for (std::uint32_t i = 0; i < channel_size; ++i) {
        buf[i] = std::byte{r.read_u8()};
    }
    return buf;
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
    if (ldr.tag != "snd") return false;

    std::string symbol = parser.string_from_offset(lf.symbolresolve.stringpointer);
    // Strip ":snd" suffix if present so the output is just "MySound.wav".
    auto suffix_pos = symbol.rfind(":snd");
    if (suffix_pos != std::string::npos) symbol = symbol.substr(0, suffix_pos);
    std::string fname = sanitize_filename(symbol) + ".wav";
    auto out_path = out_dir / fname;
    if (!overwrite && std::filesystem::exists(out_path)) {
        ctx.log("skip (exists): " + out_path.string());
        return true;
    }

    auto pr = parser.offset_to_position(lf.loaderreference.datapointer);
    if (!pr.found) {
        ctx.log("skip (datapointer unresolved): " + symbol);
        return false;
    }

    BinaryReader r(parser.side(pr.currentOVL).ovlname);
    r.seek(pr.position);

    SidSoundHeader h{};
    if (!read_sid_header(r, h)) return false;

    if (h.fmt_tag != 1) {
        ctx.log("skip (non-PCM fmt=" + std::to_string(h.fmt_tag) + "): " + symbol);
        return false;
    }
    if (h.numchannels == 0 || h.numchannels > 2) {
        ctx.log("skip (bad channels=" + std::to_string(h.numchannels) + "): " + symbol);
        return false;
    }
    if (h.samplerate < 1000 || h.samplerate > 192000) {
        ctx.log("skip (bad samplerate=" + std::to_string(h.samplerate) + "): " + symbol);
        return false;
    }

    auto pcm1 = read_channel_pcm(parser, h.channel1, static_cast<std::uint32_t>(h.channel1_size));
    auto pcm2 = read_channel_pcm(parser, h.channel2, static_cast<std::uint32_t>(h.channel2_size));

    std::vector<std::byte> pcm;
    if (h.numchannels == 1) {
        pcm = std::move(pcm1);
    } else {
        // Two channels stored separately; interleave to LRLRLR... for a
        // standard stereo WAV. Per-sample bytes = bitspersample / 8.
        std::size_t bytes_per_sample = h.bitspersample / 8;
        std::size_t samples = std::min(pcm1.size(), pcm2.size()) / bytes_per_sample;
        pcm.resize(samples * 2 * bytes_per_sample);
        for (std::size_t i = 0; i < samples; ++i) {
            std::memcpy(&pcm[i * 2 * bytes_per_sample],
                        &pcm1[i * bytes_per_sample], bytes_per_sample);
            std::memcpy(&pcm[(i * 2 + 1) * bytes_per_sample],
                        &pcm2[i * bytes_per_sample], bytes_per_sample);
        }
    }

    if (pcm.empty()) {
        ctx.log("skip (empty PCM): " + symbol);
        return false;
    }

    write_wav(out_path, h.numchannels, h.samplerate, h.bitspersample, pcm);
    ctx.log("wrote " + out_path.string() + " (" +
            std::to_string(pcm.size()) + " bytes PCM, " +
            std::to_string(h.numchannels) + "ch @ " +
            std::to_string(h.samplerate) + "Hz)");
    return true;
}

}  // namespace

ExtractResult SoundExtractor::extract(const OvlParser& parser, const ExtractContext& ctx) {
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
