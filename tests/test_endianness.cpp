#include "ovl/Endianness.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>

using namespace ovl;

TEST_CASE("read_le_u16 little-endian", "[endianness]") {
    std::array<std::byte, 2> b{std::byte{0x34}, std::byte{0x12}};
    REQUIRE(read_le_u16(b.data()) == 0x1234u);
}

TEST_CASE("read_le_u32 little-endian", "[endianness]") {
    std::array<std::byte, 4> b{std::byte{0x46}, std::byte{0x47},
                               std::byte{0x52}, std::byte{0x4B}};
    REQUIRE(read_le_u32(b.data()) == 0x4B524746u);  // OVL magic 'FGRK'
}

TEST_CASE("read_le_u32 zero", "[endianness]") {
    std::array<std::byte, 4> b{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
    REQUIRE(read_le_u32(b.data()) == 0u);
}

TEST_CASE("read_le_u32 max", "[endianness]") {
    std::array<std::byte, 4> b{std::byte{0xFF}, std::byte{0xFF},
                               std::byte{0xFF}, std::byte{0xFF}};
    REQUIRE(read_le_u32(b.data()) == 0xFFFFFFFFu);
}

TEST_CASE("read_le_i32 negative", "[endianness]") {
    std::array<std::byte, 4> b{std::byte{0xFF}, std::byte{0xFF},
                               std::byte{0xFF}, std::byte{0xFF}};
    REQUIRE(read_le_i32(b.data()) == -1);
}

TEST_CASE("read_le_f32 roundtrip 1.0", "[endianness]") {
    std::array<std::byte, 4> b{std::byte{0x00}, std::byte{0x00},
                               std::byte{0x80}, std::byte{0x3F}};
    REQUIRE(read_le_f32(b.data()) == 1.0f);
}
