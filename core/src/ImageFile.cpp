// read_image: TGA with our own reader, everything else through stb_image.
#include "ovl/Error.hpp"
#include "ovl/ImageCodec.hpp"

#include <algorithm>
#include <cctype>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include <stb_image.h>

namespace ovl::img {

Image read_image(const std::filesystem::path& p) {
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".tga") return read_tga(p);
    int w = 0, h = 0, n = 0;
    std::uint8_t* px = stbi_load(p.string().c_str(), &w, &h, &n, 4);
    if (!px) throw OvlError("cannot read image " + p.string() + ": " + stbi_failure_reason());
    Image im = make(w, h);
    std::copy_n(px, im.px.size(), im.px.begin());
    stbi_image_free(px);
    return im;
}

}  // namespace ovl::img
