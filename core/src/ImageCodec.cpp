#include "ovl/ImageCodec.hpp"

#include "ovl/Error.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>

namespace ovl::img {

Image make(int w, int h) {
    Image im;
    im.w = w;
    im.h = h;
    im.px.assign(std::size_t(w) * h * 4, 0);
    return im;
}

Image read_tga(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw OvlError("cannot open " + p.string());
    std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 18) throw OvlError("not a TGA: " + p.string());
    const int idlen = b[0], cmap = b[1], type = b[2];
    const int w = b[12] | b[13] << 8, h = b[14] | b[15] << 8, bpp = b[16], desc = b[17];
    if (cmap || (type != 2 && type != 10) || (bpp != 24 && bpp != 32))
        throw OvlError("TGA must be true-colour 24/32 bit: " + p.string());
    const int n = bpp / 8;
    std::size_t pos = 18 + std::size_t(idlen);
    Image im = make(w, h);
    std::vector<std::uint8_t> raw(std::size_t(w) * h * n);
    if (type == 2) {
        if (pos + raw.size() > b.size()) throw OvlError("truncated TGA");
        std::copy(b.begin() + std::ptrdiff_t(pos), b.begin() + std::ptrdiff_t(pos + raw.size()), raw.begin());
    } else {
        std::size_t o = 0;
        while (o < raw.size()) {
            if (pos >= b.size()) throw OvlError("truncated TGA");
            const int c = b[pos++];
            const int cnt = (c & 0x7F) + 1;
            if (c & 0x80) {
                for (int k = 0; k < cnt && o < raw.size(); ++k)
                    for (int j = 0; j < n; ++j) raw[o++] = b[pos + std::size_t(j)];
                pos += std::size_t(n);
            } else {
                for (int k = 0; k < cnt * n && o < raw.size(); ++k) raw[o++] = b[pos++];
            }
        }
    }
    const bool top_down = desc & 0x20;
    for (int y = 0; y < h; ++y) {
        const int sy = top_down ? y : h - 1 - y;
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* s = &raw[(std::size_t(sy) * w + x) * n];
            std::uint8_t* d = im.at(x, y);
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = n == 4 ? s[3] : 255;
        }
    }
    return im;
}

void write_tga(const Image& im, const std::filesystem::path& p) {
    std::vector<std::uint8_t> b(18, 0);
    b[2] = 2;
    b[12] = std::uint8_t(im.w);
    b[13] = std::uint8_t(im.w >> 8);
    b[14] = std::uint8_t(im.h);
    b[15] = std::uint8_t(im.h >> 8);
    b[16] = 32;
    b[17] = 0x28;  // top-down, 8 alpha bits
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            const auto* s = im.at(x, y);
            b.insert(b.end(), {s[2], s[1], s[0], s[3]});
        }
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

Image crop(const Image& im, int x, int y, int w, int h) {
    x = std::clamp(x, 0, im.w - 1);
    y = std::clamp(y, 0, im.h - 1);
    w = std::min(w, im.w - x);
    h = std::min(h, im.h - y);
    Image o = make(w, h);
    for (int j = 0; j < h; ++j)
        std::copy_n(im.at(x, y + j), std::size_t(w) * 4, o.at(0, j));
    return o;
}

Image resize(const Image& im, int w, int h) {
    // Area average when shrinking (each output pixel averages its source
    // footprint), nearest otherwise.
    Image o = make(w, h);
    for (int y = 0; y < h; ++y) {
        const int y0 = int(std::floor(double(y) * im.h / h));
        const int y1 = std::max(y0 + 1, int(std::floor(double(y + 1) * im.h / h)));
        for (int x = 0; x < w; ++x) {
            const int x0 = int(std::floor(double(x) * im.w / w));
            const int x1 = std::max(x0 + 1, int(std::floor(double(x + 1) * im.w / w)));
            std::array<std::uint64_t, 4> s{};
            std::uint64_t n = 0;
            for (int yy = y0; yy < std::min(y1, im.h); ++yy)
                for (int xx = x0; xx < std::min(x1, im.w); ++xx) {
                    const auto* p = im.at(xx, yy);
                    for (int c = 0; c < 4; ++c) s[std::size_t(c)] += p[c];
                    ++n;
                }
            for (int c = 0; c < 4; ++c) o.at(x, y)[c] = std::uint8_t(n ? (s[std::size_t(c)] + n / 2) / n : 0);
        }
    }
    return o;
}

void paste(Image& dst, const Image& src, int x, int y) {
    for (int j = 0; j < src.h; ++j)
        for (int i = 0; i < src.w; ++i)
            if (x + i >= 0 && x + i < dst.w && y + j >= 0 && y + j < dst.h)
                std::copy_n(src.at(i, j), 4, dst.at(x + i, y + j));
}

Image half(const Image& im) { return resize(im, std::max(1, (im.w + 1) / 2), std::max(1, (im.h + 1) / 2)); }

// ---- median cut ---------------------------------------------------------------
Indexed quantise(const Image& im) {
    struct Box {
        std::vector<std::uint32_t> px;  // 0x00RRGGBB
        int axis = 0, range = 0;
        void measure() {
            int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
            for (auto c : px)
                for (int k = 0; k < 3; ++k) {
                    const int v = int(c >> (16 - 8 * k)) & 0xFF;
                    lo[k] = std::min(lo[k], v);
                    hi[k] = std::max(hi[k], v);
                }
            range = -1;
            for (int k = 0; k < 3; ++k)
                if (hi[k] - lo[k] > range) { range = hi[k] - lo[k]; axis = k; }
        }
    };
    std::vector<Box> boxes(1);
    boxes[0].px.reserve(std::size_t(im.w) * im.h);
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            const auto* p = im.at(x, y);
            boxes[0].px.push_back(std::uint32_t(p[0]) << 16 | std::uint32_t(p[1]) << 8 | p[2]);
        }
    boxes[0].measure();
    while (boxes.size() < 256) {
        // Split the box with the widest range weighted by its population.
        std::size_t best = 0;
        double score = -1;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            if (boxes[i].px.size() < 2 || boxes[i].range <= 0) continue;
            const double s = double(boxes[i].range) * std::sqrt(double(boxes[i].px.size()));
            if (s > score) { score = s; best = i; }
        }
        if (score < 0) break;
        Box& b = boxes[best];
        const int sh = 16 - 8 * b.axis;
        auto mid = b.px.begin() + std::ptrdiff_t(b.px.size() / 2);
        std::nth_element(b.px.begin(), mid, b.px.end(),
                         [sh](std::uint32_t a, std::uint32_t c) { return ((a >> sh) & 0xFF) < ((c >> sh) & 0xFF); });
        Box nb;
        nb.px.assign(mid, b.px.end());
        b.px.erase(mid, b.px.end());
        b.measure();
        nb.measure();
        boxes.push_back(std::move(nb));
    }
    Indexed out;
    out.palette_bgra.assign(1024, 0);
    std::vector<std::array<int, 3>> pal;
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        std::uint64_t s[3] = {0, 0, 0};
        for (auto c : boxes[i].px)
            for (int k = 0; k < 3; ++k) s[k] += (c >> (16 - 8 * k)) & 0xFF;
        const std::uint64_t n = std::max<std::uint64_t>(1, boxes[i].px.size());
        std::array<int, 3> rgb{int(s[0] / n), int(s[1] / n), int(s[2] / n)};
        pal.push_back(rgb);
        out.palette_bgra[4 * i + 0] = std::uint8_t(rgb[2]);
        out.palette_bgra[4 * i + 1] = std::uint8_t(rgb[1]);
        out.palette_bgra[4 * i + 2] = std::uint8_t(rgb[0]);
    }
    out.indices.resize(std::size_t(im.w) * im.h);
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            const auto* p = im.at(x, y);
            int bi = 0, bd = 1 << 30;
            for (std::size_t i = 0; i < pal.size(); ++i) {
                const int dr = p[0] - pal[i][0], dg = p[1] - pal[i][1], db = p[2] - pal[i][2];
                const int d = 2 * dr * dr + 4 * dg * dg + db * db;
                if (d < bd) { bd = d; bi = int(i); }
            }
            out.indices[std::size_t(y) * im.w + x] = std::uint8_t(bi);
        }
    return out;
}

// ---- DXT3 ---------------------------------------------------------------------
namespace {

std::uint16_t to565(const int c[3]) {
    return std::uint16_t((std::clamp(c[0], 0, 255) * 31 + 127) / 255 << 11 |
                         (std::clamp(c[1], 0, 255) * 63 + 127) / 255 << 5 |
                         (std::clamp(c[2], 0, 255) * 31 + 127) / 255);
}
void from565(std::uint16_t v, int c[3]) {
    c[0] = ((v >> 11) & 31) * 255 / 31;
    c[1] = ((v >> 5) & 63) * 255 / 63;
    c[2] = (v & 31) * 255 / 31;
}
void palette4(std::uint16_t c0, std::uint16_t c1, int p[4][3]) {
    from565(c0, p[0]);
    from565(c1, p[1]);
    for (int k = 0; k < 3; ++k) {
        p[2][k] = (2 * p[0][k] + p[1][k]) / 3;
        p[3][k] = (p[0][k] + 2 * p[1][k]) / 3;
    }
}

}  // namespace

std::vector<std::uint8_t> encode_dxt3(const Image& im) {
    const int bw = std::max(1, (im.w + 3) / 4), bh = std::max(1, (im.h + 3) / 4);
    std::vector<std::uint8_t> out(std::size_t(bw) * bh * 16, 0);
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            int px[16][4];
            for (int j = 0; j < 16; ++j) {
                const int x = std::min(bx * 4 + j % 4, im.w - 1), y = std::min(by * 4 + j / 4, im.h - 1);
                for (int c = 0; c < 4; ++c) px[j][c] = im.at(x, y)[c];
            }
            std::uint8_t* blk = &out[(std::size_t(by) * bw + bx) * 16];
            for (int j = 0; j < 16; ++j) {
                const int a = (px[j][3] * 15 + 127) / 255;
                blk[j / 2] |= std::uint8_t(a << (4 * (j % 2)));
            }
            // Endpoints: the two pixels furthest apart along the block's
            // principal axis (approximated by the bounding-box diagonal whose
            // sign follows the covariance with red).
            double mean[3] = {0, 0, 0};
            for (auto& p : px)
                for (int k = 0; k < 3; ++k) mean[k] += p[k] / 16.0;
            double cov[3] = {0, 0, 0};
            for (auto& p : px)
                for (int k = 0; k < 3; ++k) cov[k] += (p[0] - mean[0]) * (p[k] - mean[k]);
            double axis[3] = {1, cov[1] >= 0 ? 1.0 : -1.0, cov[2] >= 0 ? 1.0 : -1.0};
            if (cov[0] < 1e-6) {  // red flat: use green as the reference
                double cg = 0, cgb = 0;
                for (auto& p : px) {
                    cg += (p[1] - mean[1]) * (p[1] - mean[1]);
                    cgb += (p[1] - mean[1]) * (p[2] - mean[2]);
                }
                axis[0] = 0;
                axis[1] = 1;
                axis[2] = cg < 1e-6 ? 1.0 : (cgb >= 0 ? 1.0 : -1.0);
            }
            int lo = 0, hi = 0;
            double plo = 1e9, phi = -1e9;
            for (int j = 0; j < 16; ++j) {
                const double t = px[j][0] * axis[0] + px[j][1] * axis[1] + px[j][2] * axis[2];
                if (t < plo) { plo = t; lo = j; }
                if (t > phi) { phi = t; hi = j; }
            }
            const std::uint16_t c0 = to565(px[hi]), c1 = to565(px[lo]);
            int pal[4][3];
            palette4(c0, c1, pal);
            std::uint32_t bits = 0;
            for (int j = 0; j < 16; ++j) {
                int bi = 0, bd = 1 << 30;
                for (int k = 0; k < 4; ++k) {
                    const int dr = px[j][0] - pal[k][0], dg = px[j][1] - pal[k][1], db = px[j][2] - pal[k][2];
                    const int d = 2 * dr * dr + 4 * dg * dg + db * db;
                    if (d < bd) { bd = d; bi = k; }
                }
                bits |= std::uint32_t(bi) << (2 * j);
            }
            blk[8] = std::uint8_t(c0);
            blk[9] = std::uint8_t(c0 >> 8);
            blk[10] = std::uint8_t(c1);
            blk[11] = std::uint8_t(c1 >> 8);
            for (int k = 0; k < 4; ++k) blk[12 + k] = std::uint8_t(bits >> (8 * k));
        }
    return out;
}

Image decode_dxt3(const std::uint8_t* data, int w, int h) {
    Image im = make(w, h);
    const int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            const std::uint8_t* blk = data + (std::size_t(by) * bw + bx) * 16;
            int pal[4][3];
            palette4(std::uint16_t(blk[8] | blk[9] << 8), std::uint16_t(blk[10] | blk[11] << 8), pal);
            const std::uint32_t bits = std::uint32_t(blk[12]) | std::uint32_t(blk[13]) << 8 |
                                       std::uint32_t(blk[14]) << 16 | std::uint32_t(blk[15]) << 24;
            for (int j = 0; j < 16; ++j) {
                const int x = bx * 4 + j % 4, y = by * 4 + j / 4;
                if (x >= w || y >= h) continue;
                const int k = (bits >> (2 * j)) & 3;
                auto* d = im.at(x, y);
                for (int c = 0; c < 3; ++c) d[c] = std::uint8_t(pal[k][c]);
                d[3] = std::uint8_t(((blk[j / 2] >> (4 * (j % 2))) & 15) * 17);
            }
        }
    return im;
}

}  // namespace ovl::img
