#include "ovl/OvlChecksum.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

using namespace ovl;

namespace {

std::vector<std::byte> bytes_of(std::string_view s) {
    std::vector<std::byte> v;
    v.reserve(s.size());
    for (char c : s) {
        v.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return v;
}

std::uint32_t chk(const std::vector<std::byte>& v) {
    return chk_checksum(v.data(), v.size());
}

}  // namespace

// Expected values come from the reflected CRC-32 of the input with its final
// byte repeated, i.e. crc32(data + data[-1:]).
TEST_CASE("chk_checksum reproduces the game's trailing-byte quirk", "[checksum]") {
    REQUIRE(chk(bytes_of("A")) == 0xA9601DBDu);
    REQUIRE(chk(bytes_of("FGRK")) == 0x8AE9CE78u);

    std::vector<std::byte> ramp;
    ramp.reserve(256);
    for (int i = 0; i < 256; ++i) {
        ramp.push_back(static_cast<std::byte>(i));
    }
    REQUIRE(chk(ramp) == 0x3625250Au);
}

TEST_CASE("chk_checksum differs from a plain CRC-32", "[checksum]") {
    // Guards against someone "fixing" the duplicated last byte: plain CRC-32
    // of "FGRK" is 0x836DE476, which shipped .CHK files do not agree with.
    REQUIRE(chk(bytes_of("FGRK")) != 0x836DE476u);
}

TEST_CASE("chk_checksum treats an empty image as 0", "[checksum]") {
    REQUIRE(chk_checksum(nullptr, 0) == 0u);
}

TEST_CASE("chk_matches compares against a stored value", "[checksum]") {
    auto data = bytes_of("FGRK");
    REQUIRE(chk_matches(data.data(), data.size(), 0x8AE9CE78u));
    REQUIRE_FALSE(chk_matches(data.data(), data.size(), 0u));
}
