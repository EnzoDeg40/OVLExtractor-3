#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ovl {

// RCT3 OVL files are little-endian. These helpers read fixed-width integers
// byte-by-byte so the host endianness is irrelevant. Never use fread(&struct)
// for multi-byte fields — the legacy code relied on MSVC/Win32 sizeof rules
// that don't hold on Linux 64-bit (where unsigned long is 8 bytes).

inline std::uint16_t read_le_u16(const std::byte* p) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[0])) |
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[1])) << 8);
}

inline std::uint32_t read_le_u32(const std::byte* p) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[0])) |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[1])) << 8 |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[2])) << 16 |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[3])) << 24;
}

inline std::int32_t read_le_i32(const std::byte* p) noexcept {
    return static_cast<std::int32_t>(read_le_u32(p));
}

inline float read_le_f32(const std::byte* p) noexcept {
    std::uint32_t u = read_le_u32(p);
    float f = 0.0f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

}  // namespace ovl
