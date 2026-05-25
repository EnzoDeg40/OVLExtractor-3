#pragma once

#include "ovl/extract/IResourceExtractor.hpp"

namespace ovl {

// Extracts PCM audio embedded behind 'snd' loaders to standalone .wav files.
// One .wav per LinkedFile (one per sound symbol). Stereo sounds produce a
// 2-channel interleaved WAV; mono produces single-channel.
class SoundExtractor : public IResourceExtractor {
public:
    std::string_view name() const override { return "sound"; }
    ExtractResult extract(const OvlParser& parser, const ExtractContext& ctx) override;
};

}  // namespace ovl
