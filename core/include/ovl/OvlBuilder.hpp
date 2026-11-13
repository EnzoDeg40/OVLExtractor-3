#pragma once

// Builds an OVL pair (common + unique) from its meaning: data blocks with
// pointer fields, symbols, loaders and symbol references. The builder owns the
// container mechanics the game expects — string table, symbol / loader /
// symbol-reference tables, block layout in one address space shared by both
// files, relocation tables, djb2 hashes, symbol order, the v5 loader-info and
// extra-data tables — so a type writer only lays out its own structs.
//
// Algorithm after libOVLng's cOvl::Save (docs/LIBOVLNG_WRITER.md §2), with the
// game's own conventions where they differ (measured on the Mac build):
//   - chunk 2 of each file starts with [symbols, loaders, symbol refs];
//     the unique file always carries all three, the common one drops an
//     empty symbol-ref table;
//   - symbols: v1 case-insensitive name, v4 hash, v5 pointer symbols by
//     loader type then hash, data symbols after them by tag then hash.

#include "ovl/OvlImage.hpp"
#include "ovl/OvlTypes.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace ovl {

struct BlockRef {
    OvlSide       side  = OvlSide::Common;
    std::uint32_t chunk = 0;
    std::uint32_t index = 0;  // among the caller's blocks of that chunk
    bool operator<(const BlockRef& o) const {
        return std::tie(side, chunk, index) < std::tie(o.side, o.chunk, o.index);
    }
};

// A location inside a caller block (pointer target or pointer slot).
struct Loc {
    BlockRef      block;
    std::uint32_t offset = 0;
};

// One record of the v5 extra-data table (14 bytes; index = loader number).
struct V5ExtraInfo {
    std::uint16_t a = 0xFFFF;  // 0 only on the second record of mdl loaders
    std::uint16_t b = 1;       // 1: flic, txt, mdl(2nd); 2: btbl, mdl(1st)
    std::uint32_t c = 1;
    std::uint16_t d = 1;
};

class OvlBuilder {
public:
    enum class SymbolOrder { Default, AsAdded };
    enum class StringOrder { Alphabetical, AsAdded };
    enum class SymRefOrder { ByLoader, AsAdded };

    explicit OvlBuilder(std::uint32_t version = 5);

    // --- file level -------------------------------------------------------
    // Loader types are listed in both files' headers; returns the type number.
    std::uint32_t add_loader_type(const ImageLoader& l);
    void add_file_reference(OvlSide side, const std::string& ref);

    // --- data -------------------------------------------------------------
    BlockRef add_block(OvlSide side, std::uint32_t chunk, Bytes data);
    Bytes&   block(const BlockRef& b);
    // Write a relocated pointer at `slot` that points to `target`.
    void set_pointer(const Loc& slot, const Loc& target);
    // Write a relocated pointer at `slot` to `s` in the common string table.
    void set_string_pointer(const Loc& slot, const std::string& s);
    void add_string(const std::string& s);

    // --- symbols, loaders, symbol references --------------------------------
    // Pointer symbol (the object a loader reference creates).
    void add_symbol(OvlSide side, const std::string& name_tag, const Loc& data,
                    std::optional<std::uint16_t> unknown = std::nullopt);
    // Data symbol: `value` stored as is, no relocation.
    void add_data_symbol(OvlSide side, const std::string& name_tag, std::uint32_t value,
                         std::optional<std::uint16_t> unknown = std::nullopt);

    struct LoaderRefSpec {
        std::uint32_t                loader_type = 0;
        std::optional<Loc>           data;        // nullopt: 0, not relocated
        std::string                  symbol;      // "" : no symbol pointer
        std::vector<Bytes>           extra_chunks;
        std::uint16_t                extra_high = 0;  // v5 high half of hasextradata
        std::vector<V5ExtraInfo>     v5_extra;
    };
    std::uint32_t add_loader_ref(OvlSide side, LoaderRefSpec spec);
    // The loader `loader_ref` must patch `slot` with the object `name_tag`.
    void add_symbol_ref(OvlSide side, std::uint32_t loader_ref, const Loc& slot,
                        const std::string& name_tag);

    // --- knobs (defaults reproduce the game's conventions) ------------------
    void set_string_order(StringOrder o) { string_order_ = o; }
    void set_symbol_order(SymbolOrder o) { symbol_order_ = o; }
    void set_symref_order(SymRefOrder o) { symref_order_ = o; }
    void set_trailer(OvlSide side, Bytes b) { side_[idx(side)].trailer = std::move(b); }
    void set_common_symref_table_always(bool on) { common_symref_always_ = on; }

    std::array<OvlImage, 2> build() const;

private:
    struct Symbol {
        std::string                  name;
        std::optional<Loc>           data;
        std::uint32_t                value = 0;
        std::optional<std::uint16_t> unknown;
    };
    struct SymRef {
        std::uint32_t loader;
        Loc           slot;
        std::string   name;
    };
    struct Side {
        std::vector<std::string>                 references;
        std::array<std::vector<Bytes>, 9>        blocks;
        std::vector<Symbol>                      symbols;
        std::vector<LoaderRefSpec>               loaders;
        std::vector<SymRef>                      symrefs;
        Bytes                                    trailer;
    };
    static std::size_t idx(OvlSide s) { return static_cast<std::size_t>(s); }

    std::uint32_t                         version_;
    std::vector<ImageLoader>              loader_types_;
    std::array<Side, 2>                   side_;
    std::map<std::pair<BlockRef, std::uint32_t>, Loc> pointers_;   // slot -> target
    std::map<std::pair<BlockRef, std::uint32_t>, std::string> spointers_;
    std::vector<std::string>              strings_;
    SymbolOrder                           symbol_order_ = SymbolOrder::Default;
    StringOrder                           string_order_ = StringOrder::Alphabetical;
    SymRefOrder                           symref_order_ = SymRefOrder::ByLoader;
    bool                                  common_symref_always_ = false;
};

}  // namespace ovl
