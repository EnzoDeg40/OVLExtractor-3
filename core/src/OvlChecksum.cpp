#include "ovl/OvlChecksum.hpp"

namespace ovl {

namespace {

// RCT3 builds this table the long way round (0x00E03F90): it bit-reverses the
// index into the top byte, runs eight MSB-first rounds against the unreflected
// polynomial 0x04C11DB7, then bit-reverses the 32-bit result. That is
// algebraically the ordinary reflected table, so we generate it directly.
struct Crc32Table {
    std::uint32_t entry[256];

    constexpr Crc32Table() : entry{} {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            entry[i] = c;
        }
    }
};

constexpr Crc32Table kTable{};

inline void fold(std::uint32_t& crc, std::uint8_t b) noexcept {
    crc = (crc >> 8) ^ kTable.entry[(crc & 0xFFu) ^ b];
}

}  // namespace

std::uint32_t chk_checksum(const std::byte* data, std::size_t size) noexcept {
    if (size == 0) {
        return 0;
    }
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        fold(crc, std::to_integer<std::uint8_t>(data[i]));
    }
    // The game's off-by-one: the last byte is folded in twice. See the header.
    fold(crc, std::to_integer<std::uint8_t>(data[size - 1]));
    return ~crc;
}

bool chk_matches(const std::byte* data, std::size_t size,
                 std::uint32_t stored) noexcept {
    return chk_checksum(data, size) == stored;
}

}  // namespace ovl
