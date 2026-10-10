#include "ovl/extract/SceneryExtractor.hpp"

#include "ovl/OvlParser.hpp"
#include "ovl/extract/GltfWriter.hpp"
#include "ovl/extract/ShapeReader.hpp"
#include "ovl/extract/TextureExtractor.hpp"
#include "ovl/extract/TextureIndex.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>

namespace ovl {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string strip_tag(std::string s) {
    auto cut = s.rfind(':');
    if (cut != std::string::npos) s.resize(cut);
    return s;
}

std::string tag_of(const std::string& s) {
    auto cut = s.rfind(':');
    return cut == std::string::npos ? std::string() : lower(s.substr(cut + 1));
}

std::string sanitize(std::string s) {
    for (auto& c : s)
        if (c == '/' || c == '\\' || c == ':' || c == '?' || c == '*' ||
            c == '"' || c == '<' || c == '>' || c == '|' || c == ' ') c = '_';
    return s.empty() ? std::string("unnamed") : s;
}

std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (static_cast<unsigned char>(c) >= 0x20) o += c;
    }
    return o + "\"";
}

// ------------------------------------------------------- handedness / maths
//
// RCT3 data is Direct3D: left-handed, Y-up, row vectors (v' = v·M, translation
// in elements 12..14). glTF is right-handed, Y-up, column vectors. Mirroring
// Z (S = diag(1,1,-1)) fixes the handedness; conjugating a matrix by S negates
// the elements where exactly one of row/column is Z. A row-vector matrix
// stored row-major has the same 16 floats as its column-vector transpose
// stored column-major, so after the mirror the array can go to glTF as is.

using Mat = std::array<float, 16>;

Mat mirror(const float* m) {
    Mat o{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            o[r * 4 + c] = ((r == 2) != (c == 2)) ? -m[r * 4 + c] : m[r * 4 + c];
    return o;
}

Mat invert(const Mat& m) {
    // General 4×4 inverse (cofactors), layout-agnostic.
    Mat inv{};
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (std::fabs(det) < 1e-12f) return Mat{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    for (float& f : inv) f /= det;
    // Affine input: pin the last column exactly (glTF validators reject
    // 0.99999994 in an inverse bind matrix).
    inv[3] = inv[7] = inv[11] = 0.f;
    inv[15] = 1.f;
    return inv;
}

// Decompose a mirrored row-vector matrix (rows 0..2 = basis vectors scaled,
// row 3 = translation) into glTF TRS.
void decompose(const Mat& m, gltf::Node& n) {
    n.translation = {m[12], m[13], m[14]};
    float s[3];
    for (int r = 0; r < 3; ++r)
        s[r] = std::sqrt(m[r*4]*m[r*4] + m[r*4+1]*m[r*4+1] + m[r*4+2]*m[r*4+2]);
    // Column-vector rotation R[i][j] = M[j][i] / s[j].
    auto R = [&](int i, int j) { return s[j] > 0 ? m[j * 4 + i] / s[j] : (i == j ? 1.f : 0.f); };
    float tr = R(0,0) + R(1,1) + R(2,2);
    float x, y, z, w;
    if (tr > 0) {
        float k = 0.5f / std::sqrt(tr + 1.0f);
        w = 0.25f / k; x = (R(2,1) - R(1,2)) * k; y = (R(0,2) - R(2,0)) * k; z = (R(1,0) - R(0,1)) * k;
    } else if (R(0,0) > R(1,1) && R(0,0) > R(2,2)) {
        float k = 2.0f * std::sqrt(1.0f + R(0,0) - R(1,1) - R(2,2));
        w = (R(2,1) - R(1,2)) / k; x = 0.25f * k; y = (R(0,1) + R(1,0)) / k; z = (R(0,2) + R(2,0)) / k;
    } else if (R(1,1) > R(2,2)) {
        float k = 2.0f * std::sqrt(1.0f + R(1,1) - R(0,0) - R(2,2));
        w = (R(0,2) - R(2,0)) / k; x = (R(0,1) + R(1,0)) / k; y = 0.25f * k; z = (R(1,2) + R(2,1)) / k;
    } else {
        float k = 2.0f * std::sqrt(1.0f + R(2,2) - R(0,0) - R(1,1));
        w = (R(1,0) - R(0,1)) / k; x = (R(0,2) + R(2,0)) / k; y = (R(1,2) + R(2,1)) / k; z = 0.25f * k;
    }
    float len = std::sqrt(x*x + y*y + z*z + w*w);
    n.rotation = {x / len, y / len, z / len, w / len};
    if (std::fabs(s[0] - 1) > 1e-4f || std::fabs(s[1] - 1) > 1e-4f || std::fabs(s[2] - 1) > 1e-4f)
        n.scale = {s[0], s[1], s[2]};
}

// ban rotation key → glTF quaternion. The key is an axis-angle vector
// (|v| = angle) in D3D space; rct3dump's rotmath() builds the row-vector form
// of the same rotation. Mirroring Z turns rotation (a, θ) into ((-ax,-ay,az), θ).
std::array<float, 4> axis_angle_quat(float x, float y, float z) {
    float a = std::sqrt(x*x + y*y + z*z);
    if (a < 1e-9f) return {0, 0, 0, 1};
    float s = std::sin(a / 2) / a;
    return {-x * s, -y * s, z * s, std::cos(a / 2)};
}

// ------------------------------------------------------------ textures

struct DecodedTex {
    bool                      ok = false;
    std::uint32_t             w = 0, h = 0;
    std::vector<std::uint8_t> bgra;
    bool                      has_alpha = false;
    std::uint32_t             recolorable = 0;
    std::string               source;   // OVL it came from
};

// Finds linked files by symbol — in the OVL pair being extracted first, then
// anywhere in the install through the global index — and decodes textures
// and texts from them, with per-extract caches.
class AssetSource {
public:
    AssetSource(const OvlParser& local, const TextureIndex* index, const int* recolor = nullptr)
        : local_(local), index_(index), recolor_(recolor) {}

    const DecodedTex& texture(const std::string& symbol_with_tag) {
        std::string key = lower(symbol_with_tag);
        auto it = textures_.find(key);
        if (it != textures_.end()) return it->second;
        DecodedTex& t = textures_[key];
        locate(symbol_with_tag, [&](const OvlParser& p, const LinkedRef& ref, const std::string& where) {
            t.ok = TextureExtractor::decode_linked_bgra(p, ref.side, ref.index, t.w, t.h,
                                                        t.bgra, &t.has_alpha, &t.recolorable, recolor_);
            t.source = where;
            return t.ok;
        });
        return t;
    }

    // Most stalls' `sta` lives in the theme's Style OVL, named after the svd
    // ("HotDogStall:sta"); look it up through the index.
    template <class ReadFn>
    bool find_and_read(const std::string& symbol_with_tag, ReadFn&& fn) {
        bool ok = false;
        locate(symbol_with_tag, [&](const OvlParser& p, const LinkedRef& ref, const std::string&) {
            ok = fn(p, ref);
            return ok;
        });
        return ok;
    }

    // A `txt` resource as UTF-8 ("" if not found).
    std::string text(const std::string& symbol_with_tag) {
        std::string key = lower(symbol_with_tag);
        auto it = texts_.find(key);
        if (it != texts_.end()) return it->second;
        std::string& out = texts_[key];
        locate(symbol_with_tag, [&](const OvlParser& p, const LinkedRef& ref, const std::string&) {
            out = read_text(p, ref.side, ref.index);
            return !out.empty();
        });
        return out;
    }

private:
    template <class Fn>
    void locate(const std::string& symbol_with_tag, Fn&& fn) {
        if (auto ref = find_linked(local_, symbol_with_tag)) {
            auto where = std::filesystem::path(local_.side(ref->side).ovlname).filename().string();
            if (fn(local_, *ref, where)) return;
        }
        if (!index_ || index_->assets_root().empty()) return;
        const auto* e = index_->lookup(lower(symbol_with_tag));
        if (!e) return;
        OvlParser* p = parser_for(index_->resolve_ovl_path(*e));
        if (!p) return;
        if (auto ref = find_linked(*p, symbol_with_tag)) fn(*p, *ref, e->ovl);
    }

    OvlParser* parser_for(const std::filesystem::path& path) {
        auto key = path.string();
        auto it = parsers_.find(key);
        if (it != parsers_.end()) return it->second.get();
        std::unique_ptr<OvlParser> p;
        try {
            p = std::make_unique<OvlParser>();
            p->parse(key);
        } catch (const std::exception&) {
            p.reset();
        }
        return parsers_.emplace(key, std::move(p)).first->second.get();
    }

    const OvlParser&                                  local_;
    const TextureIndex*                               index_;
    const int*                                        recolor_ = nullptr;
    std::map<std::string, std::unique_ptr<OvlParser>> parsers_;
    std::map<std::string, DecodedTex>                 textures_;
    std::map<std::string, std::string>                texts_;
};

// Shop metadata from the pair's `sta` (Stall) or `sat` (SpecialAttraction:
// toilets, ATM, first aid): its symbol resolves name the display name and
// description texts, the items sold (`cid`) and the scenery item (`sid`).
struct ShopInfo {
    std::string              symbol;      // "IceCream:sta"
    std::string              name, description;
    std::vector<std::string> items;       // "IceCream:cid", …
    std::string              sid;
};

ShopInfo read_shop(const OvlParser& parser, OvlSide side, std::size_t i, AssetSource& assets) {
    const auto& d = parser.side(side);
    const auto& lf = d.linkedfiles[i];
    ShopInfo shop;
    shop.symbol = linked_symbol(parser, side, i, true);
    for (const auto& sr : d.symbolresolves) {
        if (sr.loadpointer != lf.loaderreference.internal_offset) continue;
        std::string sym = parser.string_from_offset(sr.stringpointer);
        std::string t = tag_of(sym);
        std::string base = lower(strip_tag(sym));
        if (t == "txt") {
            bool desc = base.size() >= 11 && base.compare(base.size() - 11, 11, "description") == 0;
            (desc ? shop.description : shop.name) = assets.text(sym);
        } else if (t == "cid") {
            shop.items.push_back(sym);
        } else if (t == "sid") {
            shop.sid = sym;
        }
    }
    return shop;
}

// Shops declared in the pair itself (a few stalls ship their own `sta`).
std::vector<ShopInfo> read_shops(const OvlParser& parser, AssetSource& assets) {
    std::vector<ShopInfo> out;
    for (std::size_t s = 0; s < (parser.has_unique() ? 2u : 1u); ++s) {
        auto side = static_cast<OvlSide>(s);
        const auto& d = parser.side(side);
        for (std::size_t i = 0; i < d.linkedfiles.size(); ++i) {
            std::string tag = parser.loader_by_id(d.linkedfiles[i].loaderreference.loadernumber, side).tag;
            if (tag == "sta" || tag == "sat") out.push_back(read_shop(parser, side, i, assets));
        }
    }
    return out;
}

// ------------------------------------------------------------ scene builder

enum class TxsClass { Opaque, Cutout, Blend };

// Same collapse of the txs style table (docs §12) as the OBJ path.
TxsClass classify_txs(const std::string& txs_symbol) {
    std::string n = lower(strip_tag(txs_symbol));
    if (n == "siglass") return TxsClass::Blend;
    if (n.rfind("sialpha", 0) == 0 || n == "billboardstandard" || n == "guiicon")
        return TxsClass::Cutout;
    return TxsClass::Opaque;
}

class SceneBuilder {
public:
    SceneBuilder(std::string name, AssetSource& tex) : tex_(tex) { sc_.name = std::move(name); }

    gltf::Scene& scene() { return sc_; }
    const std::set<std::string>& unresolved() const { return unresolved_; }
    const std::vector<std::string>& material_info() const { return material_info_; }

    // Add a mesh (and, if skinned, its skeleton + the given animations) under
    // a fresh root node.
    void add_shape(const Shape& shape, const std::vector<BoneAnimation>& anims) {
        gltf::Mesh mesh;
        mesh.name = shape.symbol;
        for (const auto& part : shape.parts) {
            if (part.texture_flags & kNonVisualFlags) continue;   // shadow / collision mesh
            gltf::Primitive p;
            p.material = material(part.ftx_symbol, part.txs_symbol);
            std::size_t n = part.verts.size();
            p.positions.reserve(n * 3);
            for (const auto& v : part.verts) {
                p.positions.insert(p.positions.end(), {v.pos[0], v.pos[1], -v.pos[2]});
                p.normals.insert(p.normals.end(), {v.normal[0], v.normal[1], -v.normal[2]});
                p.uvs.insert(p.uvs.end(), {v.uv[0], v.uv[1]});
                if (shape.skinned) {
                    float sum = v.weights[0] + v.weights[1] + v.weights[2] + v.weights[3];
                    for (int k = 0; k < 4; ++k) {
                        bool used = v.weights[k] > 0;
                        p.joints.push_back(used ? v.bones[k] : 0);
                        p.weights.push_back(used && sum > 0 ? v.weights[k] / sum : (k == 0 && sum <= 0 ? 1.f : 0.f));
                    }
                }
            }
            // Mirroring flips the winding: swap two corners per triangle.
            p.indices = part.indices;
            for (std::size_t t = 0; t + 2 < p.indices.size(); t += 3)
                std::swap(p.indices[t + 1], p.indices[t + 2]);
            normalize_normals(p.normals);
            mesh.primitives.push_back(std::move(p));
        }
        sc_.meshes.push_back(std::move(mesh));
        int mesh_id = static_cast<int>(sc_.meshes.size()) - 1;

        gltf::Node root;
        root.name = shape.symbol;
        sc_.nodes.push_back(root);
        int root_id = static_cast<int>(sc_.nodes.size()) - 1;

        if (!shape.skinned || shape.bones.empty()) {
            sc_.nodes[root_id].mesh = mesh_id;
            return;
        }

        // Skeleton: one node per bone under the root, hierarchy from `parent`.
        int base = static_cast<int>(sc_.nodes.size());
        gltf::Skin skin;
        skin.name = shape.symbol;
        for (std::size_t b = 0; b < shape.bones.size(); ++b) {
            const auto& bone = shape.bones[b];
            gltf::Node jn;
            jn.name = bone.name.empty() ? "bone" + std::to_string(b) : bone.name;
            decompose(mirror(bone.local), jn);
            jn.parent = bone.parent >= 0 ? base + bone.parent : root_id;
            sc_.nodes.push_back(jn);
            skin.joints.push_back(base + static_cast<int>(b));
            skin.inverse_bind.push_back(invert(mirror(bone.world)));
        }
        sc_.skins.push_back(std::move(skin));
        gltf::Node mn;
        mn.name = shape.symbol + "_mesh";
        mn.mesh = mesh_id;
        mn.skin = static_cast<int>(sc_.skins.size()) - 1;
        sc_.nodes.push_back(mn);   // a root of its own: skinned mesh transforms are ignored

        for (const auto& anim : anims) add_animation(shape, base, anim);
    }

private:
    static void normalize_normals(std::vector<float>& n) {
        for (std::size_t i = 0; i + 2 < n.size(); i += 3) {
            float l = std::sqrt(n[i]*n[i] + n[i+1]*n[i+1] + n[i+2]*n[i+2]);
            if (l > 1e-8f) { n[i] /= l; n[i+1] /= l; n[i+2] /= l; }
            else { n[i] = 0; n[i+1] = 1; n[i+2] = 0; }
        }
    }

    void add_animation(const Shape& shape, int base, const BoneAnimation& anim) {
        gltf::Animation ga;
        ga.name = anim.symbol;
        for (const auto& track : anim.bones) {
            for (std::size_t b = 0; b < shape.bones.size(); ++b) {
                if (lower(shape.bones[b].name) != lower(track.name)) continue;
                int node = base + static_cast<int>(b);
                if (!track.translate.empty()) {
                    gltf::Channel c;
                    c.node = node;
                    for (const auto& k : track.translate) {
                        c.times.push_back(k.t);
                        c.values.insert(c.values.end(), {k.x, k.y, -k.z});
                    }
                    ga.channels.push_back(std::move(c));
                }
                if (!track.rotate.empty()) {
                    gltf::Channel c;
                    c.node = node;
                    c.rotation = true;
                    std::array<float, 4> prev{0, 0, 0, 1};
                    for (std::size_t i = 0; i < track.rotate.size(); ++i) {
                        const auto& k = track.rotate[i];
                        auto q = axis_angle_quat(k.x, k.y, k.z);
                        // Keep consecutive keys in the same hemisphere so
                        // LINEAR (slerp) takes the short way round.
                        if (i && q[0]*prev[0] + q[1]*prev[1] + q[2]*prev[2] + q[3]*prev[3] < 0)
                            for (float& f : q) f = -f;
                        prev = q;
                        c.times.push_back(k.t);
                        c.values.insert(c.values.end(), q.begin(), q.end());
                    }
                    ga.channels.push_back(std::move(c));
                }
            }
        }
        if (!ga.channels.empty()) sc_.animations.push_back(std::move(ga));
    }

    int image(const std::string& ftx, bool with_alpha) {
        std::string key = lower(ftx) + (with_alpha ? "#rgba" : "");
        auto it = images_.find(key);
        if (it != images_.end()) return it->second;
        const DecodedTex& t = tex_.texture(ftx);
        int id = -1;
        if (t.ok) {
            gltf::Image im;
            im.name = strip_tag(ftx) + (with_alpha ? "_rgba" : "");
            im.width = t.w;
            im.height = t.h;
            im.rgba.resize(t.bgra.size());
            for (std::size_t i = 0; i + 3 < t.bgra.size(); i += 4) {
                im.rgba[i] = t.bgra[i + 2];
                im.rgba[i + 1] = t.bgra[i + 1];
                im.rgba[i + 2] = t.bgra[i];
                im.rgba[i + 3] = with_alpha ? t.bgra[i + 3] : 255;
            }
            sc_.images.push_back(std::move(im));
            id = static_cast<int>(sc_.images.size()) - 1;
        } else {
            unresolved_.insert(ftx);
        }
        images_[key] = id;
        return id;
    }

    int material(const std::string& ftx, const std::string& txs) {
        std::string key = lower(ftx) + "|" + lower(txs);
        auto it = materials_.find(key);
        if (it != materials_.end()) return it->second;
        gltf::Material m;
        std::string txs_name = strip_tag(txs);
        m.name = strip_tag(ftx) + (txs_name.empty() ? "" : "__" + txs_name);
        TxsClass cls = txs.empty() ? TxsClass::Opaque : classify_txs(txs);
        const DecodedTex* t = ftx.empty() ? nullptr : &tex_.texture(ftx);
        bool alpha = cls != TxsClass::Opaque;
        if (alpha && t && t->ok && !t->has_alpha) alpha = false;   // nothing to cut out
        if (!ftx.empty()) m.image = image(ftx, alpha);
        m.alpha_mode = cls == TxsClass::Blend ? gltf::AlphaMode::Blend
                     : (cls == TxsClass::Cutout && alpha) ? gltf::AlphaMode::Mask
                                                         : gltf::AlphaMode::Opaque;
        m.double_sided = cls != TxsClass::Opaque;
        std::string n = lower(txs_name);
        if (n.find("specular") != std::string::npos) m.roughness = 0.5f;
        if (n.find("chrome") != std::string::npos || n.find("reflection") != std::string::npos) {
            m.metallic = 0.8f;
            m.roughness = 0.3f;
            if (m.image < 0) m.base_color = {0.85f, 0.85f, 0.88f, 1.f};
        } else if (m.image < 0 && !ftx.empty()) {
            m.base_color = {0.6f, 0.6f, 0.6f, 1.f};
        }
        std::ostringstream ex;
        ex << "{\"ftx\":" << jstr(ftx) << ",\"txs\":" << jstr(txs);
        if (t && t->ok && t->recolorable) ex << ",\"recolorable\":" << t->recolorable;
        if (t && !t->ok) ex << ",\"unresolved\":true";
        ex << "}";
        m.extras_json = ex.str();
        sc_.materials.push_back(m);
        int id = static_cast<int>(sc_.materials.size()) - 1;
        materials_[key] = id;
        const char* mode = m.alpha_mode == gltf::AlphaMode::Mask ? "MASK"
                         : m.alpha_mode == gltf::AlphaMode::Blend ? "BLEND" : "OPAQUE";
        material_info_.push_back("{\"name\":" + jstr(m.name) + ",\"ftx\":" + jstr(ftx) +
                                 ",\"txs\":" + jstr(txs) + ",\"alpha_mode\":\"" + mode +
                                 "\",\"texture_source\":" +
                                 jstr(t && t->ok ? t->source : std::string()) + "}");
        return id;
    }

    gltf::Scene                 sc_;
    AssetSource&                tex_;
    std::map<std::string, int>  images_;
    std::map<std::string, int>  materials_;
    std::set<std::string>       unresolved_;
    std::vector<std::string>    material_info_;
};

std::size_t triangle_count(const Shape& s) {
    std::size_t n = 0;
    for (const auto& p : s.parts) n += p.indices.size() / 3;
    return n;
}

struct LoadedLod {
    VisualLod                  lod;
    bool                       ok = false;
    Shape                      shape;
    std::vector<BoneAnimation> anims;
    std::string                error;
};

bool load_shape_by_symbol(const OvlParser& parser, const std::string& sym, Shape& out,
                          std::string& err) {
    auto ref = find_linked(parser, sym);
    if (!ref) { err = "mesh " + sym + " not in this OVL pair"; return false; }
    std::string tag = tag_of(sym);
    if (tag != "shs" && tag != "bsh") { err = "unsupported mesh tag " + tag; return false; }
    return read_shape(parser, ref->side, ref->index, tag == "bsh", out, err);
}

}  // namespace

ExtractResult SceneryExtractor::extract(const OvlParser& parser, const ExtractContext& ctx) {
    ExtractResult res{};
    const bool recolor = ctx.recolor[0] >= 0 || ctx.recolor[1] >= 0 || ctx.recolor[2] >= 0;
    AssetSource textures(parser, ctx.texture_index, recolor ? ctx.recolor : nullptr);
    const auto shops = read_shops(parser, textures);
    std::set<std::string> used_meshes;   // lowercased "name:tag"
    std::filesystem::create_directories(ctx.output_dir);

    auto write_scene = [&](SceneBuilder& b, const std::filesystem::path& path) {
        if (!ctx.overwrite && std::filesystem::exists(path)) {
            ctx.log("skip (exists): " + path.filename().string());
            return true;
        }
        std::string err;
        if (!gltf::write_glb(path, b.scene(), err)) {
            ctx.log("glb: " + err);
            ++res.errors;
            return false;
        }
        ++res.files_written;
        ctx.log("wrote " + path.filename().string());
        return true;
    };

    for (std::size_t s = 0; s < (parser.has_unique() ? 2u : 1u); ++s) {
        auto side = static_cast<OvlSide>(s);
        const auto& d = parser.side(side);
        for (std::size_t i = 0; i < d.linkedfiles.size(); ++i) {
            const auto& lf = d.linkedfiles[i];
            if (parser.loader_by_id(lf.loaderreference.loadernumber, side).tag != "svd") continue;
            SceneryVisual svd;
            std::string err;
            if (!read_scenery_visual(parser, side, i, svd, err)) {
                ctx.log("svd " + linked_symbol(parser, side, i) + ": " + err);
                ++res.errors;
                continue;
            }

            auto svd_shops = shops;
            if (svd_shops.empty()) {
                for (const char* tag : {":sta", ":sat"}) {
                    textures.find_and_read(svd.symbol + tag, [&](const OvlParser& p, const LinkedRef& ref) {
                        svd_shops.push_back(read_shop(p, ref.side, ref.index, textures));
                        return true;
                    });
                    if (!svd_shops.empty()) break;
                }
            }

            std::vector<LoadedLod> lods;
            for (const auto& lod : svd.lods) {
                LoadedLod L;
                L.lod = lod;
                used_meshes.insert(lower(lod.mesh_symbol));
                L.ok = load_shape_by_symbol(parser, lod.mesh_symbol, L.shape, L.error);
                if (!L.ok) {
                    // Counted as an error: silently falling back to a coarser LOD
                    // would ship the wrong model.
                    ctx.log("svd " + svd.symbol + ": LOD " + lod.mesh_symbol + " unreadable: " + L.error);
                    ++res.errors;
                }
                if (L.ok && L.shape.skinned) {
                    for (const auto& a : lod.animations) {
                        auto ref = find_linked(parser, a);
                        BoneAnimation ba;
                        std::string aerr;
                        if (ref && read_bone_animation(parser, ref->side, ref->index, ba, aerr))
                            L.anims.push_back(std::move(ba));
                        else
                            ctx.log("svd " + svd.symbol + ": animation " + a + " unreadable");
                    }
                }
                lods.push_back(std::move(L));
            }

            // Most detailed LOD = most triangles (names and distances vary
            // between content packs; the triangle count never lies).
            int best = -1;
            for (std::size_t k = 0; k < lods.size(); ++k)
                if (lods[k].ok && (best < 0 || triangle_count(lods[k].shape) >
                                               triangle_count(lods[best].shape)))
                    best = static_cast<int>(k);
            if (best < 0) {
                ctx.log("svd " + svd.symbol + ": no readable LOD");
                ++res.errors;
                continue;
            }

            std::string base = sanitize(svd.symbol);
            std::set<std::string> unresolved;
            std::vector<std::string> mat_info;
            for (std::size_t k = 0; k < lods.size(); ++k) {
                if (!lods[k].ok) continue;
                bool is_best = static_cast<int>(k) == best;
                if (!is_best && !ctx.all_lods) continue;
                SceneBuilder b(svd.symbol, textures);
                b.add_shape(lods[k].shape, lods[k].anims);
                std::ostringstream ex;
                ex << "{\"rct3_svd\":" << jstr(svd.symbol) << ",\"lod\":" << jstr(lods[k].lod.name)
                   << ",\"lod_distance\":" << lods[k].lod.distance;
                if (!svd_shops.empty() && !svd_shops[0].name.empty())
                    ex << ",\"name\":" << jstr(svd_shops[0].name);
                ex << "}";
                b.scene().extras_json = ex.str();
                auto path = ctx.output_dir /
                    (is_best ? base + ".glb" : base + "_" + sanitize(strip_tag(lods[k].lod.mesh_symbol)) + ".glb");
                write_scene(b, path);
                unresolved.insert(b.unresolved().begin(), b.unresolved().end());
                if (is_best) mat_info = b.material_info();
            }

            // Manifest.
            std::ofstream j(ctx.output_dir / (base + ".json"));
            j << "{\n  \"svd\": " << jstr(svd.symbol) << ",\n  \"flags\": " << svd.flags
              << ",\n  \"glb\": " << jstr(base + ".glb")
              << ",\n  \"best_lod\": " << jstr(lods[best].lod.name) << ",\n  \"lods\": [\n";
            for (std::size_t k = 0; k < lods.size(); ++k) {
                const auto& L = lods[k];
                j << "    {\"name\": " << jstr(L.lod.name) << ", \"mesh\": " << jstr(L.lod.mesh_symbol)
                  << ", \"mesh_type\": " << L.lod.mesh_type << ", \"distance\": " << L.lod.distance;
                if (L.ok) {
                    std::size_t verts = 0;
                    for (const auto& p : L.shape.parts) verts += p.verts.size();
                    j << ", \"vertices\": " << verts << ", \"triangles\": " << triangle_count(L.shape)
                      << ", \"submeshes\": " << L.shape.parts.size() << ", \"bones\": " << L.shape.bones.size();
                } else {
                    j << ", \"error\": " << jstr(L.error);
                }
                j << ", \"animations\": [";
                for (std::size_t a = 0; a < L.lod.animations.size(); ++a)
                    j << (a ? ", " : "") << jstr(L.lod.animations[a]);
                j << "]}" << (k + 1 < lods.size() ? "," : "") << "\n";
            }
            j << "  ],\n  \"materials\": [\n";
            for (std::size_t m = 0; m < mat_info.size(); ++m)
                j << "    " << mat_info[m] << (m + 1 < mat_info.size() ? "," : "") << "\n";
            j << "  ],\n  \"shops\": [";
            for (std::size_t m = 0; m < svd_shops.size(); ++m) {
                const auto& sh = svd_shops[m];
                j << (m ? "," : "") << "\n    {\"symbol\": " << jstr(sh.symbol)
                  << ", \"name\": " << jstr(sh.name) << ", \"description\": " << jstr(sh.description)
                  << ", \"sid\": " << jstr(sh.sid) << ", \"items\": [";
                for (std::size_t q = 0; q < sh.items.size(); ++q)
                    j << (q ? ", " : "") << jstr(sh.items[q]);
                j << "]}";
            }
            j << (svd_shops.empty() ? "" : "\n  ") << "],\n  \"unresolved_textures\": [";
            std::size_t u = 0;
            for (const auto& x : unresolved) j << (u++ ? ", " : "") << jstr(x);
            j << "]\n}\n";
            for (const auto& x : unresolved)
                ctx.log("svd " + svd.symbol + ": texture " + x + " not found");
        }
    }

    // Meshes no svd points at (track pieces, standalone shapes…).
    for (std::size_t s = 0; s < (parser.has_unique() ? 2u : 1u); ++s) {
        auto side = static_cast<OvlSide>(s);
        const auto& d = parser.side(side);
        for (std::size_t i = 0; i < d.linkedfiles.size(); ++i) {
            std::string tag = parser.loader_by_id(d.linkedfiles[i].loaderreference.loadernumber, side).tag;
            if (tag != "shs" && tag != "bsh") continue;
            std::string sym = linked_symbol(parser, side, i, true);
            if (used_meshes.count(lower(sym))) continue;
            Shape shape;
            std::string err;
            if (!read_shape(parser, side, i, tag == "bsh", shape, err)) {
                ctx.log(sym + ": " + err);
                ++res.errors;
                continue;
            }
            SceneBuilder b(shape.symbol, textures);
            b.add_shape(shape, {});
            write_scene(b, ctx.output_dir / (sanitize(shape.symbol) + ".glb"));
        }
    }
    return res;
}

}  // namespace ovl
