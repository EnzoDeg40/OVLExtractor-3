#include "ovl/Error.hpp"
#include "ovl/OvlParser.hpp"
#include "ovl/extract/DumpExtractor.hpp"
#include "ovl/extract/SoundExtractor.hpp"
#include "ovl/extract/TextureExtractor.hpp"

#include "CLI11.hpp"

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

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"ovlextract - cross-platform RCT3 OVL extractor"};
    app.set_version_flag("-V,--version", std::string("ovlextract 0.1.0"));

    std::string input;
    std::string output_dir;
    std::vector<std::string> types;
    bool do_dump_flag = false;
    bool do_list = false;
    bool overwrite = false;
    bool verbose = false;

    app.add_option("input", input, "Path to .ovl file (or basename without suffix)")
        ->required()
        ->check(CLI::ExistingPath);
    app.add_flag("--dump", do_dump_flag, "Write a structural text dump");
    app.add_flag("--list-loaders", do_list, "List loaders + linked files to stdout");
    app.add_option("-t,--types", types,
                   "Resource types to extract: sound, texture, dump, all (repeatable)")
        ->check(CLI::IsMember({"sound", "texture", "dump", "all"}));
    app.add_option("-o,--output-dir", output_dir,
                   "Output directory (default: ./extracted/<basename>/)");
    app.add_flag("--overwrite", overwrite, "Overwrite existing output files");
    app.add_flag("-v,--verbose", verbose, "Verbose logging to stderr");

    CLI11_PARSE(app, argc, argv);

    try {
        ovl::OvlParser parser;
        parser.parse(input);
        if (!parser.valid()) {
            std::cerr << "warning: parse incomplete (v6 OVL not fully supported)\n";
        }

        if (output_dir.empty()) {
            std::filesystem::path inp(input);
            std::string stem = inp.filename().string();
            auto cut = stem.find(".common.ovl");
            if (cut == std::string::npos) cut = stem.find(".unique.ovl");
            if (cut == std::string::npos) cut = stem.find(".ovl");
            if (cut != std::string::npos) stem = stem.substr(0, cut);
            output_dir = (std::filesystem::path("extracted") / stem).string();
        }

        if (do_list) return do_list_loaders(parser);

        auto wants = [&](const std::string& t) {
            for (const auto& x : types) if (x == t || x == "all") return true;
            return false;
        };

        int rc = 0;
        bool did_anything = false;
        if (do_dump_flag || wants("dump")) {
            rc |= do_dump(parser, output_dir, overwrite, verbose);
            did_anything = true;
        }
        if (wants("sound")) {
            rc |= do_extract_sound(parser, output_dir, overwrite, verbose);
            did_anything = true;
        }
        if (wants("texture")) {
            rc |= do_extract_texture(parser, output_dir, overwrite, verbose);
            did_anything = true;
        }
        if (!did_anything) {
            // No action specified -> default to dump.
            return do_dump(parser, output_dir, overwrite, verbose);
        }
        return rc;
    } catch (const ovl::OvlError& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
}
