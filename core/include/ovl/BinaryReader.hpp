#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace ovl {

// RAII binary file reader that replaces the legacy fopen_s/FILE* pattern.
// All read_* methods deserialize little-endian fields byte-by-byte, so the
// host endianness and struct alignment never matter.
class BinaryReader {
public:
    explicit BinaryReader(const std::filesystem::path& path);

    BinaryReader(const BinaryReader&) = delete;
    BinaryReader& operator=(const BinaryReader&) = delete;
    BinaryReader(BinaryReader&&) = default;
    BinaryReader& operator=(BinaryReader&&) = default;

    std::uint8_t  read_u8();
    std::uint16_t read_u16();
    std::uint32_t read_u32();
    std::int32_t  read_i32();
    float         read_f32();

    // Read exactly n bytes as an ASCII string (legacy GetStringA(FILE*, size_t)).
    std::string read_ascii(std::size_t n);

    // Read a NUL-terminated ASCII string, bounded by max_len to guard against
    // runaway reads on corrupt files. The terminating NUL is consumed but not
    // included in the result. Returns empty if no NUL is found before max_len.
    std::string read_cstring(std::size_t max_len = 4096);

    void          seek(std::uint64_t pos);
    void          skip(std::int64_t delta);
    std::uint64_t tell();
    std::uint64_t size() const { return size_; }
    bool          eof();
    const std::filesystem::path& path() const { return path_; }

private:
    void read_raw(void* dst, std::size_t n);

    std::filesystem::path path_;
    std::ifstream         stream_;
    std::uint64_t         size_;
};

}  // namespace ovl
