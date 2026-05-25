#include "ovl/OvlParser.hpp"

#include "ovl/Error.hpp"

#include <algorithm>
#include <iostream>
#include <string>

namespace ovl {

namespace {

// Resolve the path for one side. Accepts any of:
//   - basename without extension          -> append .common.ovl / .unique.ovl
//   - basename ending in .common.ovl      -> swap to .unique.ovl for Unique
//   - basename ending in .unique.ovl      -> swap to .common.ovl for Common
//   - basename ending in plain .ovl       -> append legacy suffix
std::filesystem::path resolve_side_path(const std::filesystem::path& base, OvlSide side) {
    auto s = base.string();
    const std::string common_suffix = ".common.ovl";
    const std::string unique_suffix = ".unique.ovl";
    const std::string& want   = (side == OvlSide::Unique) ? unique_suffix : common_suffix;
    const std::string& other  = (side == OvlSide::Unique) ? common_suffix : unique_suffix;

    if (s.size() >= other.size() &&
        s.compare(s.size() - other.size(), other.size(), other) == 0) {
        s.replace(s.size() - other.size(), other.size(), want);
    } else if (s.size() < want.size() ||
               s.compare(s.size() - want.size(), want.size(), want) != 0) {
        // Strip a bare .ovl if present, then append the proper side suffix.
        const std::string ovl_ext = ".ovl";
        if (s.size() >= ovl_ext.size() &&
            s.compare(s.size() - ovl_ext.size(), ovl_ext.size(), ovl_ext) == 0) {
            s.erase(s.size() - ovl_ext.size());
        }
        s += want;
    }
    return std::filesystem::path(s);
}

}  // namespace

void OvlParser::parse(const std::filesystem::path& basepath) {
    valid_ = false;
    has_unique_ = false;
    data_[0] = OvlData{};
    data_[1] = OvlData{};

    std::int32_t offset = 0;
    auto common_path = resolve_side_path(basepath, OvlSide::Common);
    if (!parse_side(common_path, OvlSide::Common, offset)) {
        // parse_side already threw OvlError on hard failures; soft return
        // (v6 partial support) just leaves valid_=false.
        return;
    }

    // Legacy: if common is v6, don't try to read unique (format incomplete).
    if (data_[0].h1.version == 6) {
        return;
    }

    auto unique_path = resolve_side_path(basepath, OvlSide::Unique);
    if (std::filesystem::exists(unique_path)) {
        if (!parse_side(unique_path, OvlSide::Unique, offset)) {
            return;
        }
        has_unique_ = true;
    }

    valid_ = true;
}

bool OvlParser::parse_side(const std::filesystem::path& ovlname, OvlSide side, std::int32_t& offset) {
    auto& d = data_[static_cast<std::size_t>(side)];
    d.ovlname = ovlname.string();

    BinaryReader r(ovlname);

    parse_header(r, d);
    auto version = d.h1.version;

    if (version != 1 && version != 4 && version != 5 && version != 6) {
        throw OvlError("Bad header version, not supported: " + std::to_string(version));
    }

    parse_references(r, d, version);
    parse_loaders(r, d, version);

    if (version == 5 || version == 6) {
        parse_chunks_v5(r, d);
    } else {
        parse_chunks_v1_v4(r, d, version);
    }

    if (version == 5 || version == 6) {
        std::uint32_t temp = r.read_u32();
        if (temp >= 4) {
            d.v5extra.extrastructcount = r.read_u32();
            if (temp > 4) {
                r.skip(static_cast<std::int64_t>(temp - 4));
            }
        }
        std::uint32_t after_count = r.read_u32();
        for (std::uint32_t i = 0; i < after_count; ++i) {
            d.v5extra.unknownafterfileblocks.push_back(r.read_u32());
        }
    }

    if (version == 4) {
        d.extendedheaders.ovlv4unknown.unknowna = r.read_u32();
        d.extendedheaders.ovlv4unknown.unknownb = r.read_u32();
    }

    if (version == 6) {
        // Legacy returns false for v6 (research incomplete). We surface this
        // as a soft failure and leave the structure partially populated.
        return false;
    }

    parse_block_data(r, d, version, offset);
    parse_relocations(r, d);
    parse_stringtable(r, d);
    parse_symbol_references(r, d, version);
    parse_loader_references(r, d);
    parse_symbol_resolves(r, d, version);

    d.postrelocunknown = r.read_u32();
    d.dataend = r.tell();

    link_loader_references(d);
    resolve_relocation_targets(r, d);
    preresolve_loaders(r, side);

    return true;
}

void OvlParser::parse_header(BinaryReader& r, OvlData& d) {
    d.h1.magic         = r.read_u32();
    d.h1.headerversion = r.read_u32();
    d.h1.version       = r.read_u32();

    if (d.h1.magic != kOvlMagic) {
        throw OvlError("File is not an overlay (bad magic): " + d.ovlname);
    }

    // Legacy: only headerversion == 2 has the V2-extra block.
    if (d.h1.headerversion == 2) {
        d.extendedheaders.headerversion2extra.unknown = r.read_u32();
        if (d.extendedheaders.headerversion2extra.unknown != 16) {
            throw OvlError("Bad header v2 extra: expected 16, got " +
                           std::to_string(d.extendedheaders.headerversion2extra.unknown));
        }
        if (d.h1.version == 4) {
            d.num_references = r.read_u32();
        }
    }

    if (d.h1.version == 5 || d.h1.version == 6) {
        d.extendedheaders.ovlv5extra.subversion = r.read_u32();
        if (d.extendedheaders.ovlv5extra.subversion == 1) {
            d.extendedheaders.ovlv5unknown.unknowna = r.read_u32();
            d.extendedheaders.ovlv5unknown.unknownb = r.read_u32();
            d.extendedheaders.ovlv5unknown.unknownc = r.read_u32();
            // Read bytes one-at-a-time until NUL, then pad to 4-byte boundary.
            std::uint8_t tc;
            std::uint32_t dotemp = 0;
            do {
                tc = r.read_u8();
                ++dotemp;
                if (dotemp == 4) dotemp = 0;
            } while (tc != 0);
            for (std::uint32_t j = dotemp; j < 4; ++j) {
                (void)r.read_u8();
            }
        }
        d.num_references = r.read_u32();
    }
}

void OvlParser::parse_references(BinaryReader& r, OvlData& d, std::uint32_t version) {
    if (version == 1) {
        d.num_references = r.read_u32();
    }
    d.references.reserve(d.num_references);
    for (std::uint32_t i = 0; i < d.num_references; ++i) {
        Reference ref;
        ref.length = r.read_u16();
        ref.file   = r.read_ascii(ref.length);
        d.references.push_back(std::move(ref));
    }
}

void OvlParser::parse_loaders(BinaryReader& r, OvlData& d, std::uint32_t version) {
    d.h2.unknown = r.read_u32();

    if (version == 6) {
        // Skip variable-length v6 loader prelude (read but discard).
        for (std::uint32_t i = 0; i < d.h2.unknown; ++i) {
            std::uint16_t val = r.read_u16();
            for (std::uint16_t j = 0; j < val; ++j) {
                (void)r.read_u16();
            }
        }
        (void)r.read_u16();
    }

    d.h2.totalloaders = r.read_u32();
    d.loaders.reserve(d.h2.totalloaders);
    for (std::uint32_t i = 0; i < d.h2.totalloaders; ++i) {
        Loader ldr;
        std::uint16_t len;

        len = r.read_u16();
        ldr.loader = r.read_ascii(len);
        len = r.read_u16();
        ldr.name = r.read_ascii(len);
        ldr.type = r.read_u32();
        len = r.read_u16();
        ldr.tag = r.read_ascii(len);

        d.loaders.push_back(std::move(ldr));
    }
}

void OvlParser::parse_chunks_v5(BinaryReader& r, OvlData& d) {
    for (std::uint32_t i = 0; i < d.h2.totalloaders; ++i) {
        OvlV5LoaderExtra extra;
        extra.count = r.read_u32();
        extra.order = r.read_u32();
        d.v5extra.ovlv5extraloaderinfo.push_back(extra);
    }
    for (std::size_t i = 0; i < 9; ++i) {
        std::uint32_t len  = r.read_u32();
        std::uint32_t temp = r.read_u32();
        (void)temp;
        Chunk chk;
        chk.num_blocks = len;
        d.chunks.push_back(chk);
        if (d.extendedheaders.ovlv5extra.subversion == 1) {
            d.v5extra.unknownperblock[i] = r.read_u32();
        }
        for (std::uint32_t j = 0; j < len; ++j) {
            Block blk{};
            blk.size = r.read_u32();
            d.chunks[i].blocks.push_back(blk);
        }
    }
}

void OvlParser::parse_chunks_v1_v4(BinaryReader& r, OvlData& d, std::uint32_t version) {
    for (std::size_t i = 0; i < 9; ++i) {
        std::uint32_t len = r.read_u32();
        if (version == 4 || version == 6) {
            len = r.read_u32();  // legacy: second read overwrites for v4/v6
        }
        Chunk chk;
        chk.num_blocks = len;
        d.chunks.push_back(chk);
        if (version == 4) {
            for (std::uint32_t j = 0; j < len; ++j) {
                Block blk{};
                blk.size = r.read_u32();
                d.chunks[i].blocks.push_back(blk);
            }
        }
    }
}

void OvlParser::parse_block_data(BinaryReader& r, OvlData& d,
                                 std::uint32_t version, std::int32_t& offset) {
    for (std::size_t i = 0; i < 9; ++i) {
        for (std::uint32_t j = 0; j < d.chunks[i].num_blocks; ++j) {
            if (version == 1) {
                Block blk{};
                blk.size = r.read_u32();
                blk.position = r.tell();
                blk.internal_offset = static_cast<std::uint32_t>(offset);
                offset += static_cast<std::int32_t>(blk.size);
                r.skip(static_cast<std::int64_t>(blk.size));
                d.chunks[i].blocks.push_back(blk);
            } else if (version == 4 || version == 5) {
                d.chunks[i].blocks[j].position = r.tell();
                d.chunks[i].blocks[j].internal_offset = static_cast<std::uint32_t>(offset);
                offset += static_cast<std::int32_t>(d.chunks[i].blocks[j].size);
                r.skip(static_cast<std::int64_t>(d.chunks[i].blocks[j].size));
            }
        }
    }
}

void OvlParser::parse_relocations(BinaryReader& r, OvlData& d) {
    std::uint32_t count = r.read_u32();
    d.relocations.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        d.relocations.push_back(r.read_u32());
    }
}

void OvlParser::parse_stringtable(BinaryReader& r, OvlData& d) {
    if (d.chunks[0].num_blocks < 1) return;
    auto& blk = d.chunks[0].blocks[0];
    if (blk.size == 0) return;

    std::uint32_t stringoffset = 0;
    r.seek(blk.position);
    auto end_pos = blk.size + blk.position;
    while (true) {
        SymbolString ss;
        ss.data = r.read_cstring(4096);
        ss.internal_offset = stringoffset;
        stringoffset += static_cast<std::uint32_t>(ss.data.length() + 1);
        d.symbolstring.push_back(std::move(ss));

        auto pos = r.tell();
        if (pos >= end_pos) break;
    }
}

void OvlParser::parse_symbol_references(BinaryReader& r, OvlData& d, std::uint32_t version) {
    if (d.chunks[2].num_blocks < 1) return;
    auto& blk = d.chunks[2].blocks[0];

    std::uint32_t per_entry = (version == 1) ? 12u : 16u;
    std::uint32_t count = blk.size / per_entry;
    r.seek(blk.position);
    std::uint32_t loaderoffset = blk.internal_offset;

    for (std::uint32_t i = 0; i < count; ++i) {
        SymbolStruct ss{};
        if (version == 1) {
            ss.stringpointer = r.read_u32();
            ss.datapointer   = r.read_u32();
            ss.ispointer     = r.read_u32();
        } else if (version == 4) {
            ss.stringpointer  = r.read_u32();
            ss.datapointer    = r.read_u32();
            ss.ispointer      = r.read_u32();
            ss.loaderpointer  = r.read_u32();
        } else {  // v5
            ss.stringpointer = r.read_u32();
            ss.datapointer   = r.read_u32();
            ss.ispointer     = r.read_u16();
            ss.loaderpointer = r.read_u16();
            ss.hash          = r.read_u32();
        }
        ss.internal_offset = loaderoffset;
        loaderoffset += per_entry;
        d.symbolpointers.push_back(ss);
    }
}

void OvlParser::parse_loader_references(BinaryReader& r, OvlData& d) {
    if (d.chunks[2].num_blocks < 2) return;
    auto& blk = d.chunks[2].blocks[1];
    std::uint32_t count = blk.size / 20u;
    r.seek(blk.position);
    std::uint32_t loaderoffset = blk.internal_offset;

    for (std::uint32_t i = 0; i < count; ++i) {
        LoadReference lr{};
        lr.loadernumber        = r.read_u32();
        lr.datapointer         = r.read_u32();
        lr.hasextradata        = r.read_u32();
        lr.symbolstructpointer = r.read_u32();
        lr.num_symbolsresolve  = r.read_u32();
        lr.internal_offset     = loaderoffset;
        loaderoffset += 20u;
        d.loaderreference.push_back(lr);
        if (lr.num_symbolsresolve > 0) d.hassymbolresolves = true;
    }
}

void OvlParser::parse_symbol_resolves(BinaryReader& r, OvlData& d, std::uint32_t version) {
    if (d.chunks[2].num_blocks < 3 || !d.hassymbolresolves) return;
    auto& blk = d.chunks[2].blocks[2];
    std::uint32_t per_entry = (version == 1) ? 12u : 16u;
    std::uint32_t count = blk.size / per_entry;
    r.seek(blk.position);
    std::uint32_t loaderoffset = blk.internal_offset;

    for (std::uint32_t i = 0; i < count; ++i) {
        SymbolResolve sr{};
        if (version == 1) {
            sr.pointer       = r.read_u32();
            sr.stringpointer = r.read_u32();
            sr.loadpointer   = r.read_u32();
        } else {  // v4 or v5
            sr.pointer       = r.read_u32();
            sr.stringpointer = r.read_u32();
            sr.loadpointer   = r.read_u32();
            sr.stringhash    = r.read_u32();
        }
        sr.internal_offset = loaderoffset;
        loaderoffset += per_entry;
        d.symbolresolves.push_back(sr);
    }
}

void OvlParser::link_loader_references(OvlData& d) {
    for (std::size_t i = 0; i < d.loaderreference.size(); ++i) {
        LinkedFiles lf{};
        lf.loaderreference = d.loaderreference[i];
        bool found = false;
        for (const auto& sp : d.symbolpointers) {
            if (d.loaderreference[i].symbolstructpointer == sp.internal_offset) {
                lf.symbolresolve = sp;
                found = true;
                break;
            }
        }
        if (found) d.linkedfiles.push_back(lf);
    }
}

void OvlParser::resolve_relocation_targets(BinaryReader& r, OvlData& d) {
    for (auto reloc : d.relocations) {
        auto pr = offset_to_position(reloc);
        if (pr.position != static_cast<std::uint64_t>(-1)) {
            r.seek(pr.position);
            std::uint32_t temp = r.read_u32();
            auto pr2 = offset_to_position(temp);
            (void)pr2;
            // Legacy adds to the target side's vector; we mirror that.
            data_[static_cast<std::size_t>(pr2.currentOVL)].relocationspointingto.push_back(temp);
        }
    }
}

namespace {

// Open the OVL file matching the given side, used by preresolve which needs
// random-access reads into a file that might be common OR unique.
BinaryReader open_side(const std::array<OvlData, 2>& data, OvlSide s) {
    return BinaryReader(data[static_cast<std::size_t>(s)].ovlname);
}

}  // namespace

void OvlParser::preresolve_loaders(BinaryReader& r, OvlSide currentSide) {
    auto& d = data_[static_cast<std::size_t>(currentSide)];

    for (std::size_t i = 0; i < d.loaderreference.size(); ++i) {
        Loader ldr = loader_by_id(d.loaderreference[i].loadernumber, currentSide);

        std::uint32_t stringpointer = 0xFEFFFFFFu;
        for (const auto& sp : d.symbolpointers) {
            if (sp.internal_offset == d.loaderreference[i].symbolstructpointer) {
                stringpointer = sp.stringpointer;
            }
        }
        std::string symbol = (stringpointer != 0xFEFFFFFFu) ? string_from_offset(stringpointer) : std::string{};

        auto pr = offset_to_position(d.loaderreference[i].datapointer);
        if (pr.position == static_cast<std::uint64_t>(-1)) continue;

        // The current loader's data may live in either side; seek into the
        // appropriate reader. For the current-side reads we use `r`; for the
        // other side we open a fresh reader.
        BinaryReader* primary = &r;
        BinaryReader fallback = (pr.currentOVL == currentSide)
                                ? open_side(data_, pr.currentOVL)
                                : open_side(data_, pr.currentOVL);
        if (pr.currentOVL != currentSide) primary = &fallback;
        primary->seek(pr.position);

        auto& target_side = data_[static_cast<std::size_t>(pr.currentOVL)];
        std::uint32_t temp = 0, temp2 = 0, temp3 = 0;

        if (ldr.tag == "svd") {
            PreResolved prs{};
            primary->skip(20);
            temp  = primary->read_u32();
            temp2 = primary->read_u32();
            prs.offset = temp2;
            prs.size   = temp;
            prs.name   = "SVD LODPOINTER LIST OF " + symbol;
            target_side.presolvedfurtherdata.push_back(prs);

            auto pr_lod = offset_to_position(temp2);
            if (pr_lod.position != static_cast<std::uint64_t>(-1)) {
                BinaryReader newovl = open_side(data_, pr_lod.currentOVL);
                newovl.seek(pr_lod.position);
                for (std::uint32_t z = 0; z < temp; ++z) {
                    temp3 = newovl.read_u32();
                    prs.offset = temp3;
                    prs.size   = 0;
                    prs.name   = "SVD LOD OF " + symbol;
                    auto pr_inner = offset_to_position(temp3);
                    data_[static_cast<std::size_t>(pr_inner.currentOVL)].presolvedfurtherdata.push_back(prs);
                }
            }
        } else if (ldr.tag == "was") {
            PreResolved prs{};
            primary->skip(40);
            temp = primary->read_u32();
            prs.offset = temp; prs.size = 0;
            prs.name = "WAS UNKNOWN-A DATABLOCK " + symbol;
            auto pra = offset_to_position(temp);
            data_[static_cast<std::size_t>(pra.currentOVL)].presolvedfurtherdata.push_back(prs);

            temp = primary->read_u32();
            prs.offset = temp; prs.size = 0;
            prs.name = "WAS UNKNOWN-B DATABLOCK " + symbol;
            auto prb = offset_to_position(temp);
            data_[static_cast<std::size_t>(prb.currentOVL)].presolvedfurtherdata.push_back(prs);

            if (temp > 0) {
                BinaryReader newovl = open_side(data_, prb.currentOVL);
                newovl.seek(prb.position);
                std::uint32_t sz = newovl.read_u32();
                prs.size = sz;
                std::uint32_t off = newovl.read_u32();
                prs.offset = off;
                prs.name = "WAS UNKNOWN-B ANIMALLIST " + symbol;
                auto pr_al = offset_to_position(off);
                data_[static_cast<std::size_t>(pr_al.currentOVL)].presolvedfurtherdata.push_back(prs);
            }
        } else if (ldr.tag == "asd") {
            PreResolved prs{};
            primary->skip(36);

            auto add = [&](const std::string& label) {
                prs.size   = primary->read_u32();
                prs.offset = primary->read_u32();
                prs.name   = label + symbol;
                auto p = offset_to_position(prs.offset);
                data_[static_cast<std::size_t>(p.currentOVL)].presolvedfurtherdata.push_back(prs);
            };
            add("ASD SPLINELIST(EAT) ");
            add("ASD SPLINELIST(DRINK) ");
            add("ASD SPLINELIST(SLEEP) ");
            primary->skip(8);
            add("ASD UNKNOWN DATABLOCK-A ");
        } else if (ldr.tag == "sid") {
            PreResolved prs{};
            primary->skip(112);
            prs.size   = primary->read_u32();
            prs.offset = primary->read_u32();
            prs.name   = "SID SOUNDDATA " + symbol;
            if (prs.offset > 0) {
                auto pr_sd = offset_to_position(prs.offset);
                data_[static_cast<std::size_t>(pr_sd.currentOVL)].presolvedfurtherdata.push_back(prs);

                BinaryReader newovl = open_side(data_, pr_sd.currentOVL);
                newovl.seek(pr_sd.position);
                newovl.skip(8);
                std::uint32_t times = newovl.read_u32();
                prs.size = times;
                std::uint32_t off = newovl.read_u32();
                prs.offset = off;
                prs.name = "SID SOUNDSCRIPT LIST " + symbol;
                auto pr_ss = offset_to_position(off);
                data_[static_cast<std::size_t>(pr_ss.currentOVL)].presolvedfurtherdata.push_back(prs);

                // Second pass: read `times` SOUNDEVENT SCRIPT entries from
                // the same position (legacy reopens the file, so we do too).
                BinaryReader script_reader = open_side(data_, pr_ss.currentOVL);
                script_reader.seek(pr_ss.position);
                for (std::uint32_t z = 0; z < times; ++z) {
                    std::uint32_t entry_off = script_reader.read_u32();
                    prs.size = 0;
                    prs.offset = entry_off;
                    prs.name = "SID SOUNDEVENT SCRIPT " + symbol;
                    auto pr_inner = offset_to_position(entry_off);
                    data_[static_cast<std::size_t>(pr_inner.currentOVL)].presolvedfurtherdata.push_back(prs);
                }
            }
        } else if (ldr.tag == "mms") {
            PreResolved prs{};
            std::uint32_t vertexcount = primary->read_u32();
            std::uint32_t indicecount = primary->read_u32();
            primary->skip(12);
            std::uint32_t morphcount = primary->read_u32();

            prs.size = vertexcount;
            prs.offset = primary->read_u32();
            prs.name = "MMS VERTEXUV'S " + symbol;
            auto pr_v = offset_to_position(prs.offset);
            data_[static_cast<std::size_t>(pr_v.currentOVL)].presolvedfurtherdata.push_back(prs);

            prs.size = indicecount;
            prs.offset = primary->read_u32();
            prs.name = "MMS INDICES " + symbol;
            auto pr_i = offset_to_position(prs.offset);
            data_[static_cast<std::size_t>(pr_i.currentOVL)].presolvedfurtherdata.push_back(prs);

            primary->skip(4);
            prs.size = morphcount;
            prs.offset = primary->read_u32();
            prs.name = "MMS MORPH DATA " + symbol;
            prs.count1 = vertexcount;
            prs.count2 = indicecount;
            auto pr_m = offset_to_position(prs.offset);
            data_[static_cast<std::size_t>(pr_m.currentOVL)].presolvedfurtherdata.push_back(prs);

            BinaryReader newovl = open_side(data_, pr_m.currentOVL);
            newovl.seek(pr_m.position);
            for (std::uint32_t z = 0; z < morphcount; ++z) {
                newovl.skip(8 * 4);
                std::uint32_t nameptr = newovl.read_u32();
                std::string animname = string_from_offset(nameptr);
                std::uint32_t times = newovl.read_u32();

                prs.size = times;
                prs.offset = newovl.read_u32();
                prs.name = "MMS MORPH TIMES LIST ANIMATION " + animname + " OF SYMBOL " + symbol;
                prs.count1 = vertexcount;
                prs.count2 = indicecount;
                auto pr_t = offset_to_position(prs.offset);
                data_[static_cast<std::size_t>(pr_t.currentOVL)].presolvedfurtherdata.push_back(prs);

                prs.size = times;
                prs.offset = newovl.read_u32();
                prs.name = "MMS MORPH VERTEXPOSITIONS ANIMATION " + animname + " OF SYMBOL " + symbol;
                prs.count3 = times;
                auto pr_p = offset_to_position(prs.offset);
                data_[static_cast<std::size_t>(pr_p.currentOVL)].presolvedfurtherdata.push_back(prs);

                std::uint32_t attach = newovl.read_u32();
                if (attach != 0) {
                    prs.size = times;
                    prs.offset = attach;
                    prs.name = "MMS MORPH ATTACHMENT UNKNOWNS " + animname + " OF SYMBOL " + symbol;
                    auto pr_a = offset_to_position(attach);
                    data_[static_cast<std::size_t>(pr_a.currentOVL)].presolvedfurtherdata.push_back(prs);
                }

                newovl.skip(3 * 4);
            }
        } else if (ldr.tag == "vwg") {
            PreResolved prs{};
            prs.size = 0;
            prs.offset = primary->read_u32();
            prs.name = "VIEWINGGALLERY DATA " + symbol;
            auto p = offset_to_position(prs.offset);
            data_[static_cast<std::size_t>(p.currentOVL)].presolvedfurtherdata.push_back(prs);
        } else if (ldr.tag == "psi") {
            PreResolved prs{};
            std::uint32_t count = primary->read_u32();
            std::uint32_t off   = primary->read_u32();
            prs.offset = off;
            prs.size   = count;
            prs.name   = "PSI UV-COORDLIST OF " + symbol;
            auto pr_l = offset_to_position(off);
            data_[static_cast<std::size_t>(pr_l.currentOVL)].presolvedfurtherdata.push_back(prs);

            if (pr_l.position != static_cast<std::uint64_t>(-1)) {
                BinaryReader newovl = open_side(data_, pr_l.currentOVL);
                newovl.seek(pr_l.position);
                for (std::uint32_t z = 0; z < count; ++z) {
                    std::uint32_t entry_off = newovl.read_u32();
                    prs.offset = entry_off;
                    prs.size   = 16;
                    prs.name   = "PSI UV-COORDINATE " + std::to_string(z + 1) + "/" +
                                 std::to_string(count) + " OF " + symbol;
                    auto pr_e = offset_to_position(entry_off);
                    data_[static_cast<std::size_t>(pr_e.currentOVL)].presolvedfurtherdata.push_back(prs);
                }
            }
        } else if (ldr.tag == "ent") {
            PreResolved prs{};
            auto try_add = [&](const std::string& label) {
                std::uint32_t sz = primary->read_u32();
                std::uint32_t off = primary->read_u32();
                if (sz != 0) {
                    prs.offset = off;
                    prs.size   = sz;
                    prs.name   = label + symbol;
                    auto p = offset_to_position(off);
                    data_[static_cast<std::size_t>(p.currentOVL)].presolvedfurtherdata.push_back(prs);
                }
            };
            primary->skip(4);
            try_add("ENT UNKNOWNFLOATLIST OF ");
            try_add("ENT BODYPART PPG-LIST OF ");
            primary->skip(164 - 12 - 8);
            try_add("ENT VIPSETTINGS OF ");
            primary->skip(4);
            try_add("ENT VIPEXTRAS OF ");
        }
    }
}

// ===== Public lookup helpers =====

Loader OvlParser::loader_by_id(std::uint32_t id, OvlSide side) const {
    const auto& d = data_[static_cast<std::size_t>(side)];
    if (id < d.loaders.size()) return d.loaders[id];
    return Loader{};
}

PositionReturn OvlParser::offset_to_position(std::uint32_t offset) const {
    PositionReturn pr{};
    std::uint32_t offsetLeft = offset;
    for (std::size_t cur = 0; cur < 2; ++cur) {
        for (std::size_t i = 0; i < data_[cur].chunks.size(); ++i) {
            for (std::uint32_t j = 0; j < data_[cur].chunks[i].num_blocks; ++j) {
                const auto& blk = data_[cur].chunks[i].blocks[j];
                if (offsetLeft < blk.size && blk.size > 0) {
                    pr.position   = blk.position + offsetLeft;
                    pr.currentOVL = static_cast<OvlSide>(cur);
                    pr.found      = true;
                    return pr;
                }
                offsetLeft -= blk.size;
            }
        }
    }
    return pr;
}

bool OvlParser::is_relocation(std::uint32_t offset) const {
    for (const auto& d : data_) {
        for (auto r : d.relocations) {
            if (r == offset) return true;
        }
    }
    return false;
}

std::string OvlParser::string_from_offset(std::uint32_t offset) const {
    for (std::size_t s = 0; s < 2; ++s) {
        for (const auto& ss : data_[s].symbolstring) {
            if (ss.internal_offset == offset) return ss.data;
        }
    }
    return "STRINGNOTFOUND";
}

std::string OvlParser::pointer_data_at_offset(std::uint32_t offset) const {
    for (const auto& d : data_) {
        for (const auto& ss : d.symbolstring) {
            if (ss.internal_offset == offset) return "string " + ss.data;
        }
    }
    for (const auto& d : data_) {
        for (const auto& sp : d.symbolpointers) {
            if (sp.internal_offset == offset) {
                return "symbolstruct with string " + string_from_offset(sp.stringpointer);
            }
        }
    }
    for (const auto& d : data_) {
        for (const auto& lf : d.linkedfiles) {
            if (lf.loaderreference.datapointer == offset) {
                return "datablock of " + string_from_offset(lf.symbolresolve.stringpointer);
            }
        }
    }
    for (std::size_t s = 0; s < 2; ++s) {
        for (const auto& lr : data_[s].loaderreference) {
            if (lr.internal_offset == offset) {
                Loader ldr = loader_by_id(lr.loadernumber, static_cast<OvlSide>(s));
                return "loaderrefence " + ldr.name;
            }
        }
    }
    for (std::size_t s = 0; s < 2; ++s) {
        for (const auto& lr : data_[s].loaderreference) {
            if (lr.datapointer == offset) {
                Loader ldr = loader_by_id(lr.loadernumber, static_cast<OvlSide>(s));
                return "datablock of stringless loaderrefence from loader " + ldr.name;
            }
        }
    }
    for (const auto& d : data_) {
        for (const auto& sr : d.symbolresolves) {
            if (sr.pointer == offset) {
                return "symbol resolve data " + string_from_offset(sr.stringpointer);
            }
        }
    }
    for (const auto& d : data_) {
        for (const auto& prs : d.presolvedfurtherdata) {
            if (prs.offset == offset) return "extended data of " + prs.name;
        }
    }
    for (const auto& d : data_) {
        for (const auto& sp : d.symbolpointers) {
            if (sp.datapointer == offset) {
                return "incoming symbolstruct datapointer with string " + string_from_offset(sp.stringpointer);
            }
        }
    }
    return "unknown";
}

std::string OvlParser::datablock_name_from_offset(std::uint32_t offset, bool strip_tag) const {
    for (std::size_t s = 0; s < 2; ++s) {
        for (std::size_t i = 0; i < data_[s].linkedfiles.size(); ++i) {
            const auto& lf = data_[s].linkedfiles[i];
            if (lf.loaderreference.datapointer == offset) {
                std::string rv = string_from_offset(lf.symbolresolve.stringpointer);
                if (strip_tag && i < data_[s].loaderreference.size()) {
                    Loader ldr = loader_by_id(data_[s].loaderreference[i].loadernumber,
                                              static_cast<OvlSide>(s));
                    if (!ldr.tag.empty() && rv.length() > ldr.tag.length() + 1) {
                        rv.erase(rv.length() - ldr.tag.length() - 1);
                    }
                }
                return rv;
            }
        }
    }
    for (const auto& d : data_) {
        for (const auto& sr : d.symbolresolves) {
            if (sr.pointer == offset) return string_from_offset(sr.stringpointer);
        }
    }
    return "UNRESOLVED_NULLPTR";
}

bool OvlParser::has_resource(const std::string& resource_name) const {
    for (const auto& d : data_) {
        for (const auto& lf : d.linkedfiles) {
            if (string_from_offset(lf.symbolresolve.stringpointer) == resource_name) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace ovl
