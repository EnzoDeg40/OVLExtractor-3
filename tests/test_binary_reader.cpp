#include "ovl/BinaryReader.hpp"
#include "ovl/Error.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace ovl;

namespace {

std::filesystem::path make_temp(const std::vector<std::byte>& bytes) {
    auto p = std::filesystem::temp_directory_path() /
             ("ovl_test_" + std::to_string(std::rand()) + ".bin");
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return p;
}

}  // namespace

TEST_CASE("BinaryReader throws on missing file", "[reader]") {
    REQUIRE_THROWS_AS(BinaryReader("/definitely/not/a/file"), OvlError);
}

TEST_CASE("BinaryReader reads u32 little-endian", "[reader]") {
    auto p = make_temp({std::byte{0x46}, std::byte{0x47},
                        std::byte{0x52}, std::byte{0x4B}});
    BinaryReader r(p);
    REQUIRE(r.size() == 4);
    REQUIRE(r.read_u32() == 0x4B524746u);
    REQUIRE(r.eof());
    std::filesystem::remove(p);
}

TEST_CASE("BinaryReader seek/tell/skip", "[reader]") {
    auto p = make_temp({std::byte{1}, std::byte{2}, std::byte{3},
                        std::byte{4}, std::byte{5}, std::byte{6}});
    BinaryReader r(p);
    REQUIRE(r.tell() == 0);
    r.skip(2);
    REQUIRE(r.tell() == 2);
    REQUIRE(r.read_u8() == 3);
    r.seek(0);
    REQUIRE(r.read_u8() == 1);
    std::filesystem::remove(p);
}

TEST_CASE("BinaryReader read_ascii", "[reader]") {
    auto p = make_temp({std::byte{'h'}, std::byte{'i'},
                        std::byte{'!'}, std::byte{0}});
    BinaryReader r(p);
    REQUIRE(r.read_ascii(3) == "hi!");
    std::filesystem::remove(p);
}

TEST_CASE("BinaryReader read_cstring", "[reader]") {
    auto p = make_temp({std::byte{'h'}, std::byte{'i'},
                        std::byte{0}, std::byte{'x'}});
    BinaryReader r(p);
    REQUIRE(r.read_cstring() == "hi");
    REQUIRE(r.read_u8() == 'x');
    std::filesystem::remove(p);
}

TEST_CASE("BinaryReader throws on EOF", "[reader]") {
    auto p = make_temp({std::byte{1}, std::byte{2}});
    BinaryReader r(p);
    REQUIRE(r.read_u16() == 0x0201u);
    REQUIRE_THROWS_AS(r.read_u8(), OvlError);
    std::filesystem::remove(p);
}
