#pragma once

#include "ovl/BinaryReader.hpp"
#include "ovl/OvlData.hpp"
#include "ovl/OvlTypes.hpp"

#include <array>
#include <cstdint>
#include <filesystem>

namespace ovl {

// Pure parser - no UI, no .NET, no Windows. Direct C++17 port of the legacy
// OVLReader::ReadOVL (OVLReader.cpp:364-1182). Both sides of an OVL pair
// (.common.ovl + .unique.ovl) are parsed into the two slots of `data_`.
class OvlParser {
public:
    OvlParser() = default;

    // Parse the OVL pair at `basepath`. The path may end with .common.ovl,
    // .unique.ovl, .ovl, or have no extension - the parser auto-appends the
    // missing suffix and reads both sides (legacy behavior).
    // Throws OvlError on hard failure (missing file, bad magic, ...).
    void parse(const std::filesystem::path& basepath);

    const OvlData& side(OvlSide s) const { return data_[static_cast<std::size_t>(s)]; }
    OvlData&       side(OvlSide s)       { return data_[static_cast<std::size_t>(s)]; }
    bool           has_unique() const    { return has_unique_; }
    bool           valid() const         { return valid_; }

    // Lookup helpers - direct ports of legacy OVLReader public API.
    Loader         loader_by_id(std::uint32_t id, OvlSide side) const;
    PositionReturn offset_to_position(std::uint32_t offset) const;
    bool           is_relocation(std::uint32_t offset) const;
    std::string    string_from_offset(std::uint32_t offset) const;
    std::string    pointer_data_at_offset(std::uint32_t offset) const;
    std::string    datablock_name_from_offset(std::uint32_t offset, bool strip_tag) const;
    bool           has_resource(const std::string& resource_name) const;

private:
    // Returns false on soft failure (e.g. v6 not fully supported); throws on
    // hard errors. Mirrors legacy ReadOVL signature with side selector + offset
    // accumulator carried across common/unique passes.
    bool parse_side(const std::filesystem::path& ovlname, OvlSide side, std::int32_t& offset);

    // Parse helpers - one per logical section of the legacy ReadOVL function.
    void parse_header(BinaryReader& r, OvlData& d);
    void parse_references(BinaryReader& r, OvlData& d, std::uint32_t version);
    void parse_loaders(BinaryReader& r, OvlData& d, std::uint32_t version);
    void parse_chunks_v1_v4(BinaryReader& r, OvlData& d, std::uint32_t version);
    void parse_chunks_v5(BinaryReader& r, OvlData& d);
    void parse_block_data(BinaryReader& r, OvlData& d, std::uint32_t version, std::int32_t& offset);
    void parse_relocations(BinaryReader& r, OvlData& d);
    void parse_stringtable(BinaryReader& r, OvlData& d);
    void parse_symbol_references(BinaryReader& r, OvlData& d, std::uint32_t version);
    void parse_loader_references(BinaryReader& r, OvlData& d);
    void parse_symbol_resolves(BinaryReader& r, OvlData& d, std::uint32_t version);
    void link_loader_references(OvlData& d);
    void resolve_relocation_targets(BinaryReader& r, OvlData& d);
    void preresolve_loaders(BinaryReader& r, OvlSide currentSide);

    std::array<OvlData, 2> data_{};
    bool                   has_unique_ = false;
    bool                   valid_ = false;
};

}  // namespace ovl
