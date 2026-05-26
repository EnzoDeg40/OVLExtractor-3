#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

namespace ovl {

// A single entry in the symbol → OVL index produced by `ovlextract
// --build-index`. The `ovl` path is a stem (no `.common.ovl` / `.unique.ovl`
// suffix) relative to the index's assets root, so it can be re-fed to
// `ovlextract` as an `input` argument or joined with the root to produce a
// full path.
struct TextureIndexEntry {
    std::string symbol;  // original case as declared in the OVL
    std::string ovl;     // path stem relative to assets root
    std::string side;    // "common" or "unique"
    std::string tag;     // loader tag (ftx, tex, shs, …)
};

// Read-only consumer for the JSON map emitted by `--build-index`. Lookups
// are case-insensitive (the JSON keys are stored lowercased to match RCT3's
// resolver semantics).
class TextureIndex {
public:
    // Parse `path` as the JSON document produced by `--build-index`. Returns
    // false if the file cannot be opened or contains no recognisable entries.
    // Tolerant of unknown fields and trailing whitespace.
    bool load(const std::filesystem::path& path);

    // Returns nullptr if no entry exists for `symbol_lc` (caller must
    // lowercase the symbol before calling).
    const TextureIndexEntry* lookup(const std::string& symbol_lc) const;

    // Assets root is where the index was originally built from. Not stored in
    // the JSON (yet), so callers must set it explicitly before using
    // `resolve_ovl_path()` to construct a full OVL file path.
    const std::filesystem::path& assets_root() const { return assets_root_; }
    void set_assets_root(std::filesystem::path p) { assets_root_ = std::move(p); }

    // Build a full filesystem path to one side of the OVL pair referenced by
    // an entry. Returns assets_root() / entry.ovl + ".common.ovl" (or
    // .unique.ovl). The caller is responsible for checking that the file
    // exists.
    std::filesystem::path resolve_ovl_path(const TextureIndexEntry& e) const;

    std::size_t size() const { return entries_.size(); }

private:
    std::filesystem::path                              assets_root_;
    std::unordered_map<std::string, TextureIndexEntry> entries_;
};

}  // namespace ovl
