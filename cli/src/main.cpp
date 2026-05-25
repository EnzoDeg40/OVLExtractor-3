#include "ovl/Error.hpp"
#include "ovl/OvlParser.hpp"
#include "ovl/extract/AtlasExtractor.hpp"
#include "ovl/extract/DumpExtractor.hpp"
#include "ovl/extract/SoundExtractor.hpp"
#include "ovl/extract/TextureExtractor.hpp"

#include "CLI11.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
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
    std::cout << "dump: " << r.files_written << " file(s) written, "
              << r.errors << " error(s)\n";
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
    std::cout << "sound: " << r.files_written << " .wav written, "
              << r.errors << " error(s)\n";
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
    std::cout << "texture: " << r.files_written << " texture(s) written, "
              << r.errors << " error(s)\n";
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
    std::cout << "atlas: " << r.files_written << " sprite(s) written, "
              << r.errors << " error(s)\n";
    return r.errors == 0 ? 0 : 2;
}

struct Actions {
    bool dump = false;
    bool list = false;
    bool sound = false;
    bool texture = false;
    bool atlas = false;
    bool overwrite = false;
    bool verbose = false;
};

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
    if (!did_anything) {
        return do_dump(parser, out_dir, a.overwrite, a.verbose);
    }
    return rc;
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
    app.add_option("-t,--types", types,
                   "Resource types to extract: sound, texture, atlas, dump, all (repeatable)")
        ->check(CLI::IsMember({"sound", "texture", "atlas", "dump", "all"}));
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

    try {
        std::filesystem::path input_path(input);

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
