#pragma once

// Pixel work for the writers: read a TGA, resize, quantise to the 256-colour
// palette of an ftx (docs/RCT3_OVL_FORMAT.md §3), and encode / decode DXT3
// mip chains for tex pages (§4.2).
//
// Images are RGBA8, rows top to bottom.

#include <cstdint>
#include <filesystem>
#include <vector>

namespace ovl::img {

struct Image {
    int w = 0, h = 0;
    std::vector<std::uint8_t> px;  // w * h * 4, RGBA
    std::uint8_t* at(int x, int y) { return &px[(std::size_t(y) * w + x) * 4]; }
    const std::uint8_t* at(int x, int y) const { return &px[(std::size_t(y) * w + x) * 4]; }
};

Image make(int w, int h);

// Uncompressed or RLE true-colour TGA (24 / 32 bit), either row order.
Image read_tga(const std::filesystem::path& p);
void  write_tga(const Image& im, const std::filesystem::path& p);

// Square crop (clamped to the image) then box / bilinear resize.
Image crop(const Image& im, int x, int y, int w, int h);
Image resize(const Image& im, int w, int h);
void  paste(Image& dst, const Image& src, int x, int y);
// Half size, 2x2 box (odd sizes round up).
Image half(const Image& im);

// 256-colour palette (median cut) + indices; alpha is ignored.
struct Indexed {
    std::vector<std::uint8_t> palette_bgra;  // 1024 B, byte 3 = 0 like the game
    std::vector<std::uint8_t> indices;       // w * h
};
Indexed quantise(const Image& im);

// DXT3 (BC2) of one level: 16 bytes per 4x4 block, rows of blocks top-down.
std::vector<std::uint8_t> encode_dxt3(const Image& im);
Image                     decode_dxt3(const std::uint8_t* data, int w, int h);

}  // namespace ovl::img
