#pragma once

#include <cstdint>

namespace ovl {

struct OvlHeader {
    std::uint32_t magic;
    std::uint32_t headerversion;
    std::uint32_t version;
};

struct OvlHeader2 {
    std::uint32_t unknown;
    std::uint32_t totalloaders;
};

struct OvlHeaderVersion2Extra {
    std::uint32_t unknown;
};

struct OvlHeaderV4Unknown {
    std::uint32_t unknowna;
    std::uint32_t unknownb;
};

struct OvlHeaderV5Extra {
    std::uint32_t subversion;
};

struct OvlHeaderV5Unknown {
    std::uint32_t unknowna;
    std::uint32_t unknownb;
    std::uint32_t unknownc;
};

struct OvlV5LoaderExtra {
    std::uint32_t count;
    std::uint32_t order;
};

struct OvlExtendedHeaders {
    OvlHeaderVersion2Extra headerversion2extra{};
    OvlHeaderV5Extra       ovlv5extra{};
    OvlHeaderV5Unknown     ovlv5unknown{};
    OvlHeaderV4Unknown     ovlv4unknown{};
};

}  // namespace ovl
