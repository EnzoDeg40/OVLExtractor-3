#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace ovl {

class OvlParser;
class TextureIndex;

struct ExtractContext {
    std::filesystem::path                  output_dir;
    bool                                   overwrite = false;
    std::function<void(std::string_view)>  log = [](std::string_view){};
    // Optional global symbol → OVL index (from `ovlextract --build-index`).
    // When set, ModelExtractor emits `map_Kd <symbol>.tga` in .mtl files for
    // each shs material whose ftx symbol is found in the index.
    const TextureIndex*                    texture_index = nullptr;
    // When true and `texture_index` is set, ModelExtractor also extracts each
    // referenced texture to `output_dir` so the .mtl's map_Kd paths resolve.
    bool                                   auto_extract_textures = false;
};

struct ExtractResult {
    std::size_t files_written = 0;
    std::size_t errors = 0;
};

class IResourceExtractor {
public:
    virtual ~IResourceExtractor() = default;
    virtual std::string_view name() const = 0;
    virtual ExtractResult extract(const OvlParser& parser, const ExtractContext& ctx) = 0;
};

}  // namespace ovl
