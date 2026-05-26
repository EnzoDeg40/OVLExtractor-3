#include "ovl/Error.hpp"
#include "ovl/OvlParser.hpp"
#include "ovl/extract/AtlasExtractor.hpp"
#include "ovl/extract/DumpExtractor.hpp"
#include "ovl/extract/ModelExtractor.hpp"
#include "ovl/extract/SoundExtractor.hpp"
#include "ovl/extract/TextureExtractor.hpp"
#include "ovl/extract/TextureIndex.hpp"

#include "CLI11.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

int do_list_loaders(const ovl::OvlParser& p) {
    for (std::size_t s = 0; s < (p.has_unique() ? 2u : 1u); ++s) {
        const auto& d = p.side(static_cast<ovl::OvlSide>(s));
        std::cout << "=== " << ovl::side_name(static_cast<ovl::OvlSide>(s))
                  << " (" << d.ovlname << ") ===\n";
        std::cout << "Loaders: " << d.loaders.size() << "\n";
        for (std::size_t i = 0; i < d.loaders.size(); ++i) {
            const auto& l = d.loaders[i];
            std::cout << "  [" << i << "] tag=" << l.tag
                      << "  name=" << l.name
                      << "  loader=" << l.loader
                      << "  type=" << l.type << "\n";
        }
        std::cout << "Linked files: " << d.linkedfiles.size() << "\n";
        for (std::size_t i = 0; i < d.linkedfiles.size(); ++i) {
            const auto& lf = d.linkedfiles[i];
            std::cout << "  " << p.string_from_offset(lf.symbolresolve.stringpointer) << "\n";
        }
    }
    return 0;
}

int do_dump(const ovl::OvlParser& p,
            const std::filesystem::path& out_dir,
            bool overwrite,
            bool verbose) {
    ovl::DumpExtractor dump;
    ovl::ExtractContext ctx;
    ctx.output_dir = out_dir;
    ctx.overwrite = overwrite;
    if (verbose) {
        ctx.log = [](std::string_view m) { std::cerr << "[dump] " << m << "\n"; };
    }
    auto r = dump.extract(p, ctx);
    if (r.files_written > 0 || r.errors > 0){
        std::cout << "dump: " << r.files_written << " file(s) written, "
                << r.errors << " error(s)\n";
    }
    return r.errors == 0 ? 0 : 2;
}

int do_extract_sound(const ovl::OvlParser& p,
                     const std::filesystem::path& out_dir,
                     bool overwrite,
                     bool verbose) {
    ovl::SoundExtractor snd;
    ovl::ExtractContext ctx;
    ctx.output_dir = out_dir;
    ctx.overwrite = overwrite;
    if (verbose) {
        ctx.log = [](std::string_view m) { std::cerr << "[sound] " << m << "\n"; };
    }
    auto r = snd.extract(p, ctx);
    if (r.files_written > 0 || r.errors > 0){
        std::cout << "sound: " << r.files_written << " .wav written, "
        << r.errors << " error(s)\n";
    }
    return r.errors == 0 ? 0 : 2;
}

int do_extract_texture(const ovl::OvlParser& p,
                       const std::filesystem::path& out_dir,
                       bool overwrite,
                       bool verbose) {
    ovl::TextureExtractor tex;
    ovl::ExtractContext ctx;
    ctx.output_dir = out_dir;
    ctx.overwrite = overwrite;
    if (verbose) {
        ctx.log = [](std::string_view m) { std::cerr << "[texture] " << m << "\n"; };
    }
    auto r = tex.extract(p, ctx);
    if (r.files_written > 0 || r.errors > 0){
        std::cout << "texture: " << r.files_written << " texture(s) written, "
                << r.errors << " error(s)\n";
    }
    return r.errors == 0 ? 0 : 2;
}

int do_extract_atlas(const ovl::OvlParser& p,
                     const std::filesystem::path& out_dir,
                     bool overwrite,
                     bool verbose) {
    ovl::AtlasExtractor atl;
    ovl::ExtractContext ctx;
    ctx.output_dir = out_dir;
    ctx.overwrite = overwrite;
    if (verbose) {
        ctx.log = [](std::string_view m) { std::cerr << "[atlas] " << m << "\n"; };
    }
    auto r = atl.extract(p, ctx);
    if (r.files_written > 0 || r.errors > 0){
        std::cout << "atlas: " << r.files_written << " sprite(s) written, "
                << r.errors << " error(s)\n";
    }
    return r.errors == 0 ? 0 : 2;
}

int do_extract_model(const ovl::OvlParser& p,
                     const std::filesystem::path& out_dir,
                     bool overwrite,
                     bool verbose,
                     const ovl::TextureIndex* texture_index,
                     bool auto_extract_textures) {
    ovl::ModelExtractor m;
    ovl::ExtractContext ctx;
    ctx.output_dir = out_dir;
    ctx.overwrite = overwrite;
    ctx.texture_index = texture_index;
    ctx.auto_extract_textures = auto_extract_textures;
    if (verbose) {
        ctx.log = [](std::string_view m) { std::cerr << "[model] " << m << "\n"; };
    }
    auto r = m.extract(p, ctx);
    if (r.files_written > 0 || r.errors > 0){
        std::cout << "model: " << r.files_written << " mesh(es) written, "
                << r.errors << " error(s)\n";
    }
    return r.errors == 0 ? 0 : 2;
}

struct Actions {
    bool dump = false;
    bool list = false;
    bool sound = false;
    bool texture = false;
    bool atlas = false;
    bool model = false;
    bool overwrite = false;
    bool verbose = false;
    bool auto_textures = false;
    const ovl::TextureIndex* texture_index = nullptr;
};

// Symbol index entry: where a given resource symbol (e.g. "gigacoaster:ftx")
// is defined as a linked file. Stores the OVL pair's relative path stem (no
// .common.ovl / .unique.ovl suffix), which side declared it, the loader tag,
// and the original-case symbol (the JSON key is normalized to lowercase
// because RCT3's symbol resolution is case-insensitive — e.g. references to
// "StationLights:ftx" target the linked file declared as "stationLights:ftx").
struct IndexEntry {
    std::string symbol;  // original case from the linked-file table
    std::string ovl;     // e.g. "tracks/coasters/Track6/Track6_Textures"
    std::string side;    // "common" or "unique"
    std::string tag;     // e.g. "ftx", "tex", "shs"
};

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Escape a string for JSON output (handles ", \, control chars; assumes UTF-8
// passthrough for all other bytes — symbol names in RCT3 are ASCII anyway).
void json_escape(std::ostream& o, const std::string& s) {
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\b': o << "\\b"; break;
            case '\f': o << "\\f"; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o << buf;
                } else {
                    o << static_cast<char>(c);
                }
        }
    }
}

std::string strip_ovl_suffix(const std::string& name) {
    static const std::string suffixes[] = {".common.ovl", ".unique.ovl", ".ovl"};
    for (const auto& s : suffixes) {
        if (name.size() >= s.size() &&
            name.compare(name.size() - s.size(), s.size(), s) == 0) {
            return name.substr(0, name.size() - s.size());
        }
    }
    return name;
}

int process_one(const std::filesystem::path& input,
                const std::filesystem::path& out_dir,
                const Actions& a) {
    ovl::OvlParser parser;
    parser.parse(input.string());
    if (!parser.valid() && a.verbose) {
        std::cerr << "warning: parse incomplete for " << input.string() << "\n";
    }

    if (a.list) return do_list_loaders(parser);

    int rc = 0;
    bool did_anything = false;
    if (a.dump) {
        rc |= do_dump(parser, out_dir, a.overwrite, a.verbose);
        did_anything = true;
    }
    if (a.sound) {
        rc |= do_extract_sound(parser, out_dir, a.overwrite, a.verbose);
        did_anything = true;
    }
    if (a.texture) {
        rc |= do_extract_texture(parser, out_dir, a.overwrite, a.verbose);
        did_anything = true;
    }
    if (a.atlas) {
        rc |= do_extract_atlas(parser, out_dir, a.overwrite, a.verbose);
        did_anything = true;
    }
    if (a.model) {
        rc |= do_extract_model(parser, out_dir, a.overwrite, a.verbose,
                               a.texture_index, a.auto_textures);
        did_anything = true;
    }
    if (!did_anything) {
        return do_dump(parser, out_dir, a.overwrite, a.verbose);
    }
    return rc;
}

// Walk every .common.ovl under `root`, parse the OVL pair, and add an entry
// for each linked file (both common and unique sides) keyed by its full
// symbol (e.g. "Dice:ftx"). Collisions are kept by first occurrence and
// counted. Writes the resulting map to `out_path` as sorted JSON.
int do_build_index(const std::filesystem::path& root,
                   const std::filesystem::path& out_path,
                   bool verbose) {
    std::vector<std::filesystem::path> files;
    for (auto& e : std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied)) {
        if (!e.is_regular_file()) continue;
        auto name = e.path().filename().string();
        if (name.size() >= 11 &&
            name.compare(name.size() - 11, 11, ".common.ovl") == 0) {
            files.push_back(e.path());
        }
    }
    std::sort(files.begin(), files.end());
    std::cerr << "Indexing " << files.size() << " OVL pair(s) under "
              << root.string() << "\n";

    std::map<std::string, IndexEntry> index;
    std::size_t parsed = 0, parse_errors = 0, dup_count = 0;
    for (std::size_t fi = 0; fi < files.size(); ++fi) {
        const auto& p = files[fi];
        try {
            ovl::OvlParser parser;
            parser.parse(p.string());
            auto rel_dir = std::filesystem::relative(p.parent_path(), root);
            auto stem = strip_ovl_suffix(p.filename().string());
            auto rel_stem = (rel_dir / stem).generic_string();

            for (std::size_t s = 0; s < (parser.has_unique() ? 2u : 1u); ++s) {
                auto side = static_cast<ovl::OvlSide>(s);
                const auto& d = parser.side(side);
                for (const auto& lf : d.linkedfiles) {
                    std::string sym = parser.string_from_offset(
                        lf.symbolresolve.stringpointer);
                    if (sym.empty()) continue;
                    ovl::Loader ldr = parser.loader_by_id(
                        lf.loaderreference.loadernumber, side);
                    IndexEntry e{sym, rel_stem, ovl::side_name(side), ldr.tag};
                    auto [it, inserted] = index.try_emplace(
                        to_lower(sym), std::move(e));
                    if (!inserted) ++dup_count;
                }
            }
            ++parsed;
        } catch (const std::exception& e) {
            ++parse_errors;
            if (verbose) {
                std::cerr << "  skip " << p.string() << ": " << e.what() << "\n";
            }
        }
        if (verbose && (fi % 200) == 0) {
            std::cerr << "  [" << (fi + 1) << "/" << files.size() << "] "
                      << index.size() << " symbol(s)\n";
        }
    }

    std::ofstream o(out_path);
    if (!o) {
        std::cerr << "error: cannot open " << out_path.string()
                  << " for writing\n";
        return 1;
    }
    o << "{\n";
    bool first = true;
    for (const auto& [lc_sym, e] : index) {
        if (!first) o << ",\n";
        first = false;
        o << "  \""; json_escape(o, lc_sym); o << "\": {";
        o << "\"symbol\":\""; json_escape(o, e.symbol); o << "\",";
        o << "\"ovl\":\"";    json_escape(o, e.ovl);    o << "\",";
        o << "\"side\":\"";   json_escape(o, e.side);   o << "\",";
        o << "\"tag\":\"";    json_escape(o, e.tag);    o << "\"}";
    }
    o << "\n}\n";

    std::cout << "Index: " << index.size() << " symbol(s) from "
              << parsed << "/" << files.size() << " OVL pair(s); "
              << dup_count << " duplicate(s), "
              << parse_errors << " parse error(s) → " << out_path.string()
              << "\n";
    return parse_errors == 0 ? 0 : 2;
}

int run_recursive(const std::filesystem::path& root,
                  const std::filesystem::path& out_root,
                  const Actions& a) {
    std::vector<std::filesystem::path> files;
    for (auto& e : std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied)) {
        if (!e.is_regular_file()) continue;
        auto name = e.path().filename().string();
        if (name.size() >= 11 &&
            name.compare(name.size() - 11, 11, ".common.ovl") == 0) {
            files.push_back(e.path());
        }
    }
    std::sort(files.begin(), files.end());

    std::cerr << "Found " << files.size() << " .common.ovl file(s) under "
              << root.string() << "\n";

    std::size_t ok = 0, failed = 0;
    int agg_rc = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& p = files[i];
        auto rel = std::filesystem::relative(p.parent_path(), root);
        std::string stem = strip_ovl_suffix(p.filename().string());
        auto sub_out = out_root / rel / stem;

        if (a.verbose || (i % 100) == 0) {
            std::cerr << "[" << (i + 1) << "/" << files.size() << "] "
                      << p.string() << "\n";
        }
        try {
            int rc = process_one(p, sub_out, a);
            if (rc == 0) ++ok; else { ++failed; agg_rc |= rc; }
        } catch (const std::exception& e) {
            ++failed;
            agg_rc |= 1;
            std::cerr << "error: " << p.string() << ": " << e.what() << "\n";
        }
    }
    std::cout << "Batch: " << ok << "/" << files.size() << " ok, "
              << failed << " failed\n";
    return agg_rc;
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"ovlextract - cross-platform RCT3 OVL extractor"};
    app.set_version_flag("-V,--version", std::string("ovlextract 0.1.0"));

    std::string input;
    std::string output_dir;
    std::string build_index_out;
    std::string texture_index_in;
    std::string assets_root;
    std::vector<std::string> types;
    Actions a;
    bool do_dump_flag = false;

    app.add_option("input", input,
                   "Path to .ovl file, basename without suffix, or a directory "
                   "(recursive batch mode)")
        ->required()
        ->check(CLI::ExistingPath);
    app.add_flag("--dump", do_dump_flag, "Write a structural text dump");
    app.add_flag("--list-loaders", a.list, "List loaders + linked files to stdout");
    app.add_option("--build-index", build_index_out,
                   "Recursively scan the input directory and write a JSON "
                   "index of every linked-file symbol to the given path. "
                   "Used downstream to resolve cross-OVL texture references.")
        ->type_name("FILE");
    app.add_option("--texture-index", texture_index_in,
                   "Load the JSON index from a previous --build-index run. "
                   "ModelExtractor uses it to emit `map_Kd <name>.tga` for "
                   "each shs material whose ftx symbol resolves.")
        ->type_name("FILE")
        ->check(CLI::ExistingFile);
    app.add_option("--assets-root", assets_root,
                   "Root directory the texture index was built against. "
                   "Required with --auto-textures.")
        ->type_name("DIR");
    app.add_flag("--auto-textures", a.auto_textures,
                 "When extracting models with --texture-index, also extract "
                 "each referenced texture from its source OVL into the "
                 "model's output directory so .mtl map_Kd paths resolve. "
                 "Requires --assets-root.");
    app.add_option("-t,--types", types,
                   "Resource types to extract: sound, texture, atlas, model, dump, all (repeatable)")
        ->check(CLI::IsMember({"sound", "texture", "atlas", "model", "dump", "all"}));
    app.add_option("-o,--output-dir", output_dir,
                   "Output directory (default: ./extracted/<basename>/ for single file, "
                   "./extracted/ for directory input)");
    app.add_flag("--overwrite", a.overwrite, "Overwrite existing output files");
    app.add_flag("-v,--verbose", a.verbose, "Verbose logging to stderr");

    CLI11_PARSE(app, argc, argv);

    auto wants = [&](const std::string& t) {
        for (const auto& x : types) if (x == t || x == "all") return true;
        return false;
    };
    a.dump = do_dump_flag || wants("dump");
    a.sound = wants("sound");
    a.texture = wants("texture");
    a.atlas = wants("atlas");
    a.model = wants("model");

    try {
        std::filesystem::path input_path(input);

        if (!build_index_out.empty()) {
            if (!std::filesystem::is_directory(input_path)) {
                std::cerr << "error: --build-index requires a directory input\n";
                return 1;
            }
            return do_build_index(input_path, build_index_out, a.verbose);
        }

        ovl::TextureIndex texture_index;
        if (!texture_index_in.empty()) {
            if (!texture_index.load(texture_index_in)) {
                std::cerr << "error: failed to load texture index from "
                          << texture_index_in << "\n";
                return 1;
            }
            if (!assets_root.empty()) {
                texture_index.set_assets_root(assets_root);
            } else if (a.auto_textures) {
                std::cerr << "error: --auto-textures requires --assets-root\n";
                return 1;
            }
            a.texture_index = &texture_index;
            if (a.verbose) {
                std::cerr << "Loaded texture index: " << texture_index.size()
                          << " symbol(s) from " << texture_index_in << "\n";
            }
        }
        if (a.auto_textures && a.texture_index == nullptr) {
            std::cerr << "error: --auto-textures requires --texture-index\n";
            return 1;
        }

        if (std::filesystem::is_directory(input_path)) {
            std::filesystem::path out_root = output_dir.empty()
                ? std::filesystem::path("extracted")
                : std::filesystem::path(output_dir);
            return run_recursive(input_path, out_root, a);
        }

        if (output_dir.empty()) {
            std::string stem = strip_ovl_suffix(input_path.filename().string());
            output_dir = (std::filesystem::path("extracted") / stem).string();
        }
        return process_one(input_path, output_dir, a);
    } catch (const ovl::OvlError& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
}
