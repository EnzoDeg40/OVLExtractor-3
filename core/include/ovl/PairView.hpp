#pragma once

// Read-only view of an OVL pair as the game sees it once loaded: one address
// space (common chunks 0..8, then unique chunks 0..8), the relocated pointer
// slots, the symbol-reference slots and the objects (loader references) with
// their symbol names. Built on OvlImage, independent of OvlParser; used by the
// type codecs (SceneryCodec) to decode objects and to check that a decoder
// accounted for every pointer of a struct.

#include "ovl/OvlImage.hpp"
#include "ovl/OvlTypes.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ovl {

class PairView {
public:
    struct Object {
        std::string        name;  // "Dice:sid"
        std::string        tag;   // loader tag ("sid")
        OvlSide            side = OvlSide::Common;
        std::uint32_t      data = 0;
        std::vector<Bytes> extra;  // the loader's extra-data chunks
    };

    PairView(const OvlImage& common, const OvlImage& unique);
    static PairView load(const std::filesystem::path& common_path);

    std::uint32_t  u32(std::uint32_t a) const;
    std::int32_t   i32(std::uint32_t a) const { return static_cast<std::int32_t>(u32(a)); }
    std::uint16_t  u16(std::uint32_t a) const;
    float          f32(std::uint32_t a) const;
    Bytes          bytes(std::uint32_t a, std::uint32_t n) const;
    std::string    cstr(std::uint32_t a) const;
    std::u16string wstr(std::uint32_t a) const;

    bool is_reloc(std::uint32_t slot) const { return relocs_.count(slot) > 0; }
    // "name:tag" of the symbol reference that patches `slot`, if any.
    std::optional<std::string> symref(std::uint32_t slot) const;
    // Pointer value at a relocated slot, nullopt if the slot is not relocated.
    std::optional<std::uint32_t> ptr(std::uint32_t slot) const;

    const std::vector<Object>& objects() const { return objects_; }
    const std::vector<std::string>& references(OvlSide s) const {
        return img_[static_cast<std::size_t>(s)].references;
    }
    const Object*              find(const std::string& name_tag) const;

    // Relocated slots and symbol-ref slots inside [a, a + n).
    std::vector<std::uint32_t> slots_in(std::uint32_t a, std::uint32_t n) const;

private:
    struct Placed {
        std::uint32_t start;
        const Bytes*  data;
    };
    const Bytes& block_at(std::uint32_t a, std::uint32_t& off) const;

    std::array<OvlImage, 2>                  img_;
    std::vector<Placed>                      blocks_;
    std::set<std::uint32_t>                  relocs_;
    std::map<std::uint32_t, std::string>     symrefs_;
    std::vector<Object>                      objects_;
};

}  // namespace ovl
