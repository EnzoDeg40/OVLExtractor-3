#pragma once

#include "ovl/extract/IResourceExtractor.hpp"

namespace ovl {

// Writes a structural text dump of the OVL contents (references, chunks,
// loaders, symbols, relocations, pre-resolved data). Loose port of the
// legacy DumpOVL function from Form1.h:1185 — readable summary, not a
// byte-for-byte hex dump.
class DumpExtractor : public IResourceExtractor {
public:
    std::string_view name() const override { return "dump"; }
    ExtractResult extract(const OvlParser& parser, const ExtractContext& ctx) override;
};

}  // namespace ovl
