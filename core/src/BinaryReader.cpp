#include "ovl/BinaryReader.hpp"

#include "ovl/Endianness.hpp"
#include "ovl/Error.hpp"

#include <array>
#include <cstddef>

namespace ovl {

BinaryReader::BinaryReader(const std::filesystem::path& path)
    : path_(path), stream_(path, std::ios::binary), size_(0) {
    if (!stream_) {
        throw OvlError("BinaryReader: cannot open file: " + path.string());
    }
    stream_.seekg(0, std::ios::end);
    auto end = stream_.tellg();
    if (end < 0) {
        throw OvlError("BinaryReader: cannot determine size of: " + path.string());
    }
    size_ = static_cast<std::uint64_t>(end);
    stream_.seekg(0, std::ios::beg);
}

void BinaryReader::read_raw(void* dst, std::size_t n) {
    stream_.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
    if (stream_.gcount() != static_cast<std::streamsize>(n)) {
        throw OvlError("BinaryReader: unexpected EOF in " + path_.string());
    }
}

std::uint8_t BinaryReader::read_u8() {
    std::byte b{};
    read_raw(&b, 1);
    return std::to_integer<std::uint8_t>(b);
}

std::uint16_t BinaryReader::read_u16() {
    std::array<std::byte, 2> buf{};
    read_raw(buf.data(), buf.size());
    return read_le_u16(buf.data());
}

std::uint32_t BinaryReader::read_u32() {
    std::array<std::byte, 4> buf{};
    read_raw(buf.data(), buf.size());
    return read_le_u32(buf.data());
}

std::int32_t BinaryReader::read_i32() {
    std::array<std::byte, 4> buf{};
    read_raw(buf.data(), buf.size());
    return read_le_i32(buf.data());
}

float BinaryReader::read_f32() {
    std::array<std::byte, 4> buf{};
    read_raw(buf.data(), buf.size());
    return read_le_f32(buf.data());
}

std::string BinaryReader::read_ascii(std::size_t n) {
    std::string s(n, '\0');
    if (n > 0) read_raw(s.data(), n);
    return s;
}

std::string BinaryReader::read_cstring(std::size_t max_len) {
    std::string s;
    s.reserve(64);
    for (std::size_t i = 0; i < max_len; ++i) {
        auto c = read_u8();
        if (c == 0) return s;
        s.push_back(static_cast<char>(c));
    }
    return s;
}

void BinaryReader::seek(std::uint64_t pos) {
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(pos), std::ios::beg);
    if (!stream_) throw OvlError("BinaryReader: seek failed in " + path_.string());
}

void BinaryReader::skip(std::int64_t delta) {
    stream_.seekg(static_cast<std::streamoff>(delta), std::ios::cur);
    if (!stream_) throw OvlError("BinaryReader: skip failed in " + path_.string());
}

std::uint64_t BinaryReader::tell() {
    auto p = stream_.tellg();
    if (p < 0) throw OvlError("BinaryReader: tell failed in " + path_.string());
    return static_cast<std::uint64_t>(p);
}

bool BinaryReader::eof() {
    return stream_.peek() == std::char_traits<char>::eof();
}

}  // namespace ovl
