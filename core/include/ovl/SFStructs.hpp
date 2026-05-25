#pragma once

#include <cstdint>

// Port of the legacy SFStructs.h - RCT3-specific resource layouts used by
// individual loaders (sid for sound, mms for morphable meshes, etc.).
// These are descriptive layouts, NOT to be read via fread(&struct, sizeof...).
// Always deserialize field-by-field with BinaryReader::read_u32 / read_f32.

namespace ovl {

struct WavFmtChunk {
    std::uint16_t tag;              // PCM = 1
    std::uint16_t numchannels;      // 1 or 2
    std::uint32_t samplerate;
    std::uint32_t byterate;
    std::uint16_t blockalign;
    std::uint16_t bitspersample;
};

struct SidSound {
    WavFmtChunk   fmt;
    std::uint16_t unk1;             // 0
    std::uint32_t unk2;
    float         unk3;
    float         unk4;
    float         unk5;
    std::uint32_t unk6;
    float         unk7;
    std::uint32_t unk8;
    float         unk9;
    float         unk10;
    float         unk11;
    std::int32_t  loop;
    std::uint32_t channel1;         // offset to PCM channel 1
    std::int32_t  channel1_size;
    std::uint32_t channel2;         // offset to PCM channel 2 (stereo)
    std::int32_t  channel2_size;
};

struct MorphMeshVertex {
    std::uint8_t X;
    std::uint8_t Y;
    std::uint8_t Z;
};

struct SpriteCoords {
    float left;                     // 0.0 - 1.0
    float top;
    float right;
    float bottom;
};

struct ParticleSkin {
    std::uint32_t spritecount;      // 1-16 observed
    std::uint32_t pos;
    std::uint32_t tex_ref;
    std::uint32_t flaga;            // 0 or 1
    std::uint32_t flagb;            // 0 = animated, 1 = static
    float         unknownmodifier;  // 0.125 - 2.0, usually 1.0
    std::uint32_t unk1;             // always 0
};

}  // namespace ovl
