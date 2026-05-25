#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace ovl {

class OvlParser;

struct ExtractContext {
    std::filesystem::path output_dir;
    bool overwrite = false;
    std::function<void(std::string_view)> log = [](std::string_view){};
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
