#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ovl {

// Magic at byte 0 of every RCT3 OVL: 'F' 'G' 'R' 'K' little-endian = 0x4B524746.
inline constexpr std::uint32_t kOvlMagic = 0x4B524746u;

// RCT3 OVLs come paired: <name>.common.ovl + <name>.unique.ovl. The legacy
// OVLReader uses an int [0,1] to address them; we use a strong enum.
enum class OvlSide : std::uint32_t {
    Common = 0,
    Unique = 1,
};

inline const char* side_name(OvlSide s) {
    return s == OvlSide::Common ? "common" : "unique";
}

struct Reference {
    std::uint16_t length;
    std::string   file;
};

struct Loader {
    std::string   loader;
    std::string   name;
    std::uint32_t type;
    std::string   tag;
};

struct Block {
    std::uint32_t size;
    std::uint64_t position;          // absolute file offset
    std::uint32_t internal_offset;
};

struct Chunk {
    std::uint32_t      num_blocks;
    std::vector<Block> blocks;
};

struct SymbolString {
    std::uint64_t pos;
    std::uint32_t internal_offset;
    std::string   data;
};

struct SymbolStruct {
    std::uint32_t stringpointer;
    std::uint32_t datapointer;
    std::uint32_t ispointer;          // legacy ulong; v5 reads only low 2 bytes
    std::uint32_t loaderpointer;      // legacy ulong; v5 reads only low 2 bytes
    std::uint32_t hash;
    std::uint32_t internal_offset;
};

struct LoadReference {
    std::uint32_t loadernumber;
    std::uint32_t datapointer;
    std::uint32_t hasextradata;
    std::uint32_t symbolstructpointer;
    std::uint32_t num_symbolsresolve;
    std::uint32_t internal_offset;
};

struct SymbolResolve {
    std::uint32_t pointer;
    std::uint32_t stringpointer;
    std::uint32_t loadpointer;
    std::uint32_t stringhash;
    std::uint32_t internal_offset;
};

struct LinkedFiles {
    LoadReference loaderreference;
    SymbolStruct  symbolresolve;
};

struct PreResolved {
    std::uint32_t offset;
    std::string   name;
    std::uint32_t size;
    std::uint32_t count1;
    std::uint32_t count2;
    std::uint32_t count3;
};

struct PositionReturn {
    OvlSide       currentOVL = OvlSide::Common;
    std::uint64_t position   = static_cast<std::uint64_t>(-1);
    bool          found      = false;
};

}  // namespace ovl
