#include "ovl/extract/DumpExtractor.hpp"

#include "ovl/Error.hpp"
#include "ovl/OvlParser.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace ovl {

namespace {

std::string hex(std::uint64_t v) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << v;
    return oss.str();
}

void dump_side(std::ostream& out, const OvlParser& parser, OvlSide side) {
    const auto& d = parser.side(side);
    out << "\n======================================================\n";
    out << "Overlay dump of " << d.ovlname << " (side: " << side_name(side) << ")\n";
    out << "======================================================\n";
    out << "Header version: " << d.h1.headerversion
        << "  OVL version: " << d.h1.version << "\n";
    if (d.h1.version == 5 || d.h1.version == 6) {
        out << "Subversion:     " << d.extendedheaders.ovlv5extra.subversion << "\n";
    }
    out << "Num references: " << d.num_references << "\n";
    out << "Total loaders:  " << d.h2.totalloaders << "\n\n";

    if (!d.references.empty()) {
        out << "-- References (" << d.references.size() << ") --\n";
        for (const auto& ref : d.references) {
            out << "  Reference to " << ref.file << "\n";
        }
        out << "\n";
    }

    out << "-- Loaders (" << d.loaders.size() << ") --\n";
    for (std::size_t i = 0; i < d.loaders.size(); ++i) {
        const auto& ldr = d.loaders[i];
        out << "  [" << i << "] tag=" << ldr.tag
            << "  loader=" << ldr.loader
            << "  name=" << ldr.name
            << "  type=" << ldr.type << "\n";
    }
    out << "\n";

    out << "-- Chunks (9) --\n";
    for (std::size_t i = 0; i < d.chunks.size(); ++i) {
        const auto& chk = d.chunks[i];
        out << "  Chunk " << (i + 1) << "/9 : " << chk.num_blocks << " block(s)\n";
        for (std::size_t j = 0; j < chk.blocks.size(); ++j) {
            const auto& blk = chk.blocks[j];
            auto end_off = blk.internal_offset + blk.size;
            out << "    part " << (j + 1) << "/" << chk.num_blocks
                << " : " << hex(blk.internal_offset) << "(" << blk.internal_offset
                << ") - " << hex(end_off) << "(" << end_off
                << ")  size=" << blk.size
                << "  filepos=" << hex(blk.position) << "\n";
        }
    }
    out << "\n";

    out << "-- Stringtable (" << d.symbolstring.size() << ") --\n";
    for (const auto& s : d.symbolstring) {
        out << "  " << hex(s.internal_offset) << ": " << s.data << "\n";
    }
    out << "\n";

    out << "-- Symbol references (" << d.symbolpointers.size() << ") --\n";
    for (std::size_t i = 0; i < d.symbolpointers.size(); ++i) {
        const auto& sp = d.symbolpointers[i];
        out << "  [" << i << "] off=" << hex(sp.internal_offset)
            << "  stringptr=" << hex(sp.stringpointer)
            << " -> '" << parser.string_from_offset(sp.stringpointer) << "'"
            << "  dataptr=" << hex(sp.datapointer)
            << "  isptr=" << sp.ispointer
            << "  loaderptr=" << sp.loaderpointer
            << "  hash=" << hex(sp.hash) << "\n";
    }
    out << "\n";

    out << "-- Loader references (" << d.loaderreference.size() << ") --\n";
    for (std::size_t i = 0; i < d.loaderreference.size(); ++i) {
        const auto& lr = d.loaderreference[i];
        Loader ldr = parser.loader_by_id(lr.loadernumber, side);
        out << "  [" << i << "] off=" << hex(lr.internal_offset)
            << "  loader#" << lr.loadernumber << "(" << ldr.name << "/" << ldr.tag << ")"
            << "  dataptr=" << hex(lr.datapointer)
            << "  hasextra=" << lr.hasextradata
            << "  symbolstructptr=" << hex(lr.symbolstructpointer)
            << "  nresolves=" << lr.num_symbolsresolve << "\n";
    }
    out << "\n";

    if (d.hassymbolresolves) {
        out << "-- Symbol resolves (" << d.symbolresolves.size() << ") --\n";
        for (std::size_t i = 0; i < d.symbolresolves.size(); ++i) {
            const auto& sr = d.symbolresolves[i];
            out << "  [" << i << "] off=" << hex(sr.internal_offset)
                << "  ptr=" << hex(sr.pointer)
                << "  strptr=" << hex(sr.stringpointer)
                << " -> '" << parser.string_from_offset(sr.stringpointer) << "'"
                << "  loadptr=" << hex(sr.loadpointer)
                << "  strhash=" << hex(sr.stringhash) << "\n";
        }
        out << "\n";
    }

    out << "-- Relocations (" << d.relocations.size() << ") --\n";
    for (std::size_t i = 0; i < d.relocations.size(); ++i) {
        out << "  [" << i << "] " << hex(d.relocations[i]) << "\n";
    }
    out << "\n";

    out << "-- Pre-resolved data (" << d.presolvedfurtherdata.size() << ") --\n";
    for (std::size_t i = 0; i < d.presolvedfurtherdata.size(); ++i) {
        const auto& prs = d.presolvedfurtherdata[i];
        out << "  [" << i << "] off=" << hex(prs.offset)
            << "  size=" << prs.size
            << "  : " << prs.name << "\n";
    }
    out << "\n";

    out << "-- Linked files (" << d.linkedfiles.size() << ") --\n";
    for (std::size_t i = 0; i < d.linkedfiles.size(); ++i) {
        const auto& lf = d.linkedfiles[i];
        out << "  [" << i << "] '"
            << parser.string_from_offset(lf.symbolresolve.stringpointer)
            << "' loader#" << lf.loaderreference.loadernumber
            << " dataptr=" << hex(lf.loaderreference.datapointer) << "\n";
    }
    out << "\n";
}

}  // namespace

ExtractResult DumpExtractor::extract(const OvlParser& parser, const ExtractContext& ctx) {
    ExtractResult res{};
    std::filesystem::create_directories(ctx.output_dir);

    auto write_one = [&](OvlSide side) {
        const auto& d = parser.side(side);
        if (d.ovlname.empty()) return;
        std::filesystem::path src(d.ovlname);
        auto stem = src.filename().string();
        // Strip ".common.ovl" / ".unique.ovl" -> base name.
        auto cut = stem.find(".common.ovl");
        if (cut == std::string::npos) cut = stem.find(".unique.ovl");
        if (cut != std::string::npos) stem = stem.substr(0, cut);

        std::string suffix = (side == OvlSide::Common) ? "_common.txt" : "_unique.txt";
        auto out_path = ctx.output_dir / ("OverlayDump_" + stem + suffix);

        if (!ctx.overwrite && std::filesystem::exists(out_path)) {
            ctx.log("skip (exists): " + out_path.string());
            return;
        }
        std::ofstream out(out_path, std::ios::binary);
        if (!out) {
            ++res.errors;
            ctx.log("error: cannot write " + out_path.string());
            return;
        }
        dump_side(out, parser, side);
        ++res.files_written;
        ctx.log("wrote " + out_path.string());
    };

    write_one(OvlSide::Common);
    if (parser.has_unique()) write_one(OvlSide::Unique);
    return res;
}

}  // namespace ovl
