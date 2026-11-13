// mod.toml, build, install, uninstall (rct3mod/Mod.hpp).
#include "rct3mod/Mod.hpp"

#include "Util.hpp"

#include <toml++/toml.hpp>

#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace rct3mod {

namespace {

[[noreturn]] void bad(const std::string& where, const std::string& what) {
    throw std::runtime_error("mod.toml: " + where + ": " + what);
}

std::string req_str(const toml::table& t, const char* key, const std::string& where) {
    auto v = t[key].value<std::string>();
    if (!v || v->empty()) bad(where, std::string("missing ") + key);
    return *v;
}

std::string opt_str(const toml::table& t, const char* key, const std::string& def = {}) {
    return t[key].value<std::string>().value_or(def);
}

std::optional<Crop> opt_crop(const toml::table& t, const char* key, const std::string& where) {
    auto* a = t[key].as_array();
    if (!a) return std::nullopt;
    if (a->size() != 3) bad(where, std::string(key) + " = [x, y, size]");
    Crop c;
    int* f[3] = {&c.x, &c.y, &c.size};
    for (std::size_t i = 0; i < 3; ++i) {
        auto v = (*a)[i].value<std::int64_t>();
        if (!v) bad(where, std::string(key) + " = [x, y, size] (integers)");
        *f[i] = int(*v);
    }
    return c;
}

fs::path resolve(const fs::path& dir, const std::string& f) {
    if (f.empty()) return {};
    fs::path p = fs::path(f).is_absolute() ? fs::path(f) : dir / f;
    if (!fs::exists(p)) throw std::runtime_error("mod.toml: file not found: " + p.string());
    return p;
}

fs::path record_dir(const fs::path& game_assets) { return game_assets / ".rct3mod"; }

void check_id(const std::string& id) {
    if (id.empty() || !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; }))
        throw std::runtime_error("mod id \"" + id + "\": use letters, digits, - and _ only");
}

Installed read_record(const fs::path& f) {
    Installed r;
    r.id = strip_suffix(f.filename().string(), ".files");
    std::ifstream in(f);
    for (std::string line; std::getline(in, line);)
        if (!line.empty()) r.files.emplace_back(fs::path(line));
    return r;
}

// Every file under root, relative to it.
std::vector<fs::path> files_under(const fs::path& root) {
    std::vector<fs::path> out;
    if (!fs::exists(root)) return out;
    for (auto& e : fs::recursive_directory_iterator(root))
        if (e.is_regular_file()) out.push_back(e.path().lexically_relative(root));
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

Mod load_mod(const fs::path& dir) {
    const fs::path f = dir / "mod.toml";
    toml::table root;
    try {
        root = toml::parse_file(f.string());
    } catch (const toml::parse_error& e) {
        std::ostringstream s;
        s << f.string() << ":" << e.source().begin.line << ": " << e.description();
        throw std::runtime_error(s.str());
    }
    Mod m;
    m.dir = dir;
    auto* mt = root["mod"].as_table();
    if (!mt) bad("[mod]", "missing");
    m.id = req_str(*mt, "id", "[mod]");
    check_id(m.id);
    m.name = opt_str(*mt, "name", m.id);
    m.version = opt_str(*mt, "version", "0.0.0");
    m.author = opt_str(*mt, "author");

    if (auto* a = root["path"].as_array())
        for (std::size_t i = 0; i < a->size(); ++i) {
            auto* t = (*a)[i].as_table();
            const std::string where = "[[path]] #" + std::to_string(i + 1);
            if (!t) bad(where, "not a table");
            PathType p;
            p.name = req_str(*t, "name", where);
            p.title = opt_str(*t, "title", p.name);
            p.base = opt_str(*t, "base", p.base);
            p.texture = resolve(dir, req_str(*t, "texture", where));
            p.crop = opt_crop(*t, "crop", where);
            p.icon = resolve(dir, opt_str(*t, "icon"));
            p.icon_crop = opt_crop(*t, "icon_crop", where);
            m.paths.push_back(std::move(p));
        }
    if (auto* a = root["scenery"].as_array())
        for (std::size_t i = 0; i < a->size(); ++i) {
            auto* t = (*a)[i].as_table();
            const std::string where = "[[scenery]] #" + std::to_string(i + 1);
            if (!t) bad(where, "not a table");
            SceneryItem s;
            s.name = req_str(*t, "name", where);
            s.title = opt_str(*t, "title", s.name);
            s.visual = req_str(*t, "visual", where);
            s.svd = req_str(*t, "svd", where);
            s.style = req_str(*t, "style", where);
            s.sid = req_str(*t, "sid", where);
            s.folder = opt_str(*t, "folder", "Style/Custom/" + m.id + "/" + s.name);
            s.theme = int((*t)["theme"].value<std::int64_t>().value_or(0));
            if (auto* tint = (*t)["tint"].as_array()) {
                if (tint->size() != 3) bad(where, "tint = [r, g, b]");
                std::array<float, 3> c{};
                for (std::size_t k = 0; k < 3; ++k) c[k] = float((*tint)[k].value<double>().value_or(0));
                s.tint = c;
            }
            if (auto c = (*t)["cost"].value<std::int64_t>()) s.cost = int(*c);
            m.scenery.push_back(std::move(s));
        }
    if (m.paths.empty() && m.scenery.empty()) bad("", "no [[path]] or [[scenery]]");

    // Names must not collide inside the mod (one Style pair per folder).
    std::set<std::string> names;
    std::set<fs::path> folders;
    for (auto& p : m.paths)
        if (!names.insert("path:" + p.name).second) bad("[[path]]", "two path types named " + p.name);
    for (auto& s : m.scenery) {
        if (!names.insert("scenery:" + s.name).second) bad("[[scenery]]", "two items named " + s.name);
        if (!folders.insert(s.folder).second) bad("[[scenery]]", "one item per folder: " + s.folder.generic_string());
    }
    return m;
}

void new_mod(const fs::path& dir, const std::string& id) {
    check_id(id);
    const fs::path f = dir / "mod.toml";
    if (fs::exists(f)) throw std::runtime_error(f.string() + " exists");
    fs::create_directories(dir);
    std::string name;
    for (char c : id)
        if (std::isalnum(static_cast<unsigned char>(c))) name += c;
    if (name.empty()) name = "MyPath";
    name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
    std::ofstream o(f);
    o << "# RCT3 mod: build with `rct3mod build .`, install with `rct3mod install .`\n"
         "[mod]\n"
         "id = \"" << id << "\"\n"
         "name = \"" << name << "\"\n"
         "version = \"1.0.0\"\n"
         "author = \"\"\n"
         "\n"
         "# A path type, copied from a game one (a folder of Assets/Path/), with\n"
         "# your picture on the ground and as its menu icon.\n"
         "[[path]]\n"
         "name = \"" << name << "\"       # letters, digits, _ (used in file and symbol names)\n"
         "title = \"" << name << "\"      # shown in the game\n"
         "base = \"Asphalt\"\n"
         "texture = \"texture.png\"   # TGA, PNG, JPEG or BMP, put it beside mod.toml\n"
         "# crop = [x, y, size]      # square part of the picture (default: centred)\n"
         "# icon = \"icon.png\"        # default: texture\n"
         "# icon_crop = [x, y, size]\n"
         "\n"
         "# A scenery item, copied from a game one:\n"
         "# [[scenery]]\n"
         "# name = \"MyBench\"\n"
         "# title = \"My bench\"\n"
         "# visual = \"Style/Themed/Vanilla/Bench/Bench.common.ovl\"   # pair with the svd\n"
         "# svd = \"Bench\"\n"
         "# style = \"Style/Themed/Vanilla/Style.common.ovl\"          # pair with the sid\n"
         "# sid = \"Bench\"\n"
         "# theme = 0                 # 0 Vanilla, 1 Wild West, 2 Spooky, 3 Space, ...\n"
         "# tint = [255, 120, 40]\n"
         "# cost = 50\n";
}

std::vector<fs::path> build_mod(const Mod& m, const fs::path& game_assets, const fs::path& out_assets,
                                const fs::path& preview) {
    if (!fs::is_directory(game_assets / "Path")) throw std::runtime_error(game_assets.string() + " is not the game's Assets folder");
    fs::remove_all(out_assets);
    fs::create_directories(out_assets);
    if (!preview.empty()) fs::create_directories(preview);
    for (auto& p : m.paths) build_path_type(p, game_assets, out_assets, preview);
    for (auto& s : m.scenery) build_scenery_item(s, game_assets, out_assets);
    return files_under(out_assets);
}

std::vector<Installed> installed_mods(const fs::path& game_assets) {
    std::vector<Installed> out;
    const fs::path d = record_dir(game_assets);
    if (!fs::is_directory(d)) return out;
    for (auto& e : fs::directory_iterator(d))
        if (ends_with(e.path().filename().string(), ".files")) out.push_back(read_record(e.path()));
    std::sort(out.begin(), out.end(), [](const Installed& a, const Installed& b) { return a.id < b.id; });
    return out;
}

Installed install_mod(const Mod& m, const fs::path& game_assets) {
    const fs::path staged = m.dir / "build" / "Assets";
    auto files = build_mod(m, game_assets, staged);

    // Who owns what: a file may be replaced only if this mod installed it.
    std::map<fs::path, std::string> owner;
    for (auto& r : installed_mods(game_assets))
        for (auto& f : r.files) owner[f] = r.id;
    for (auto& f : files) {
        auto it = owner.find(f);
        if (it != owner.end() && it->second != m.id)
            throw std::runtime_error(f.generic_string() + " belongs to mod " + it->second);
        if (it == owner.end() && fs::exists(game_assets / f))
            throw std::runtime_error(f.generic_string() + " already exists in the game (not installed by rct3mod)");
    }
    // Drop the previous version first (it may have had other files).
    for (auto& r : installed_mods(game_assets))
        if (r.id == m.id) uninstall_mod(m.id, game_assets);

    for (auto& f : files) {
        fs::create_directories((game_assets / f).parent_path());
        fs::copy_file(staged / f, game_assets / f, fs::copy_options::overwrite_existing);
    }
    fs::create_directories(record_dir(game_assets));
    std::ofstream o(record_dir(game_assets) / (m.id + ".files"));
    for (auto& f : files) o << f.generic_string() << "\n";
    return {m.id, files};
}

Installed uninstall_mod(const std::string& id, const fs::path& game_assets) {
    check_id(id);
    const fs::path rec = record_dir(game_assets) / (id + ".files");
    if (!fs::exists(rec)) throw std::runtime_error("mod " + id + " is not installed");
    Installed r = read_record(rec);
    std::set<fs::path> dirs;
    for (auto& f : r.files) {
        if (f.is_absolute() || f.lexically_normal().generic_string().rfind("..", 0) == 0) continue;  // never outside Assets
        fs::remove(game_assets / f);
        for (fs::path d = f.parent_path(); !d.empty(); d = d.parent_path()) dirs.insert(d);
    }
    // Folders left empty, deepest first.
    std::vector<fs::path> ds(dirs.begin(), dirs.end());
    std::sort(ds.begin(), ds.end(), [](const fs::path& a, const fs::path& b) {
        return std::distance(a.begin(), a.end()) > std::distance(b.begin(), b.end());
    });
    for (auto& d : ds)
        if (fs::is_directory(game_assets / d) && fs::is_empty(game_assets / d)) fs::remove(game_assets / d);
    fs::remove(rec);
    if (fs::is_empty(record_dir(game_assets))) fs::remove(record_dir(game_assets));
    return r;
}

}  // namespace rct3mod
