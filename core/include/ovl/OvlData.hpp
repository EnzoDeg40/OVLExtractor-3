#pragma once

#include "ovl/OvlHeader.hpp"
#include "ovl/OvlTypes.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ovl {

struct V5Extra {
    std::vector<OvlV5LoaderExtra> ovlv5extraloaderinfo;
    std::array<std::uint32_t, 9>  unknownperblock{};
    std::vector<std::uint32_t>    unknownafterfileblocks;
    std::uint32_t                 extrastructcount = 0;
};

// All parsed data for one side (common or unique) of an OVL pair.
// Direct port of the legacy OVLData struct.
struct OvlData {
    std::string                 ovlname;
    bool                        hassymbolresolves = false;
    std::uint32_t               num_references = 0;

    OvlHeader                   h1{};
    OvlHeader2                  h2{};
    OvlExtendedHeaders          extendedheaders{};
    V5Extra                     v5extra{};

    std::vector<Loader>         loaders;
    std::vector<Reference>      references;
    std::vector<Chunk>          chunks;
    std::vector<std::uint32_t>  relocations;
    std::vector<std::uint32_t>  relocationspointingto;
    std::vector<SymbolString>   symbolstring;

    std::vector<SymbolStruct>   symbolpointers;
    std::vector<LoadReference>  loaderreference;
    std::vector<SymbolResolve>  symbolresolves;

    std::vector<PreResolved>    presolvedfurtherdata;
    std::vector<LinkedFiles>    linkedfiles;

    std::uint32_t               postrelocunknown = 0;
    std::uint64_t               dataend = 0;
};

}  // namespace ovl
