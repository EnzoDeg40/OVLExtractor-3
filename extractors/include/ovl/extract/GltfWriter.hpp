#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ovl::gltf {

// Minimal glTF 2.0 scene model + binary (.glb) writer. No external
// dependencies: images are embedded as PNG (stored/uncompressed deflate),
// JSON is emitted by hand. All data must already be in glTF conventions
// (right-handed, Y-up, column-vector matrices stored column-major).

struct Image {
    std::string               name;
    std::uint32_t             width = 0, height = 0;
    std::vector<std::uint8_t> rgba;          // 4 B/px, row-major, top-down
};

enum class AlphaMode { Opaque, Mask, Blend };

struct Material {
    std::string            name;
    int                    image = -1;       // index into Scene::images, -1 = none
    std::array<float, 4>   base_color{1, 1, 1, 1};
    AlphaMode              alpha_mode = AlphaMode::Opaque;
    float                  alpha_cutoff = 0.5f;
    bool                   double_sided = false;
    float                  metallic = 0.0f;
    float                  roughness = 0.9f;
    std::string            extras_json;      // optional raw JSON object for "extras"
};

struct Primitive {
    std::vector<float>         positions;    // 3 per vertex
    std::vector<float>         normals;      // 3 per vertex (optional)
    std::vector<float>         uvs;          // 2 per vertex (optional)
    std::vector<std::uint16_t> joints;       // 4 per vertex (optional, skinned)
    std::vector<float>         weights;      // 4 per vertex (optional, skinned)
    std::vector<std::uint32_t> indices;      // triangle list
    int                        material = -1;
};

struct Mesh {
    std::string            name;
    std::vector<Primitive> primitives;
};

struct Node {
    std::string            name;
    int                    parent = -1;
    std::array<float, 3>   translation{0, 0, 0};
    std::array<float, 4>   rotation{0, 0, 0, 1};   // x y z w
    std::array<float, 3>   scale{1, 1, 1};
    int                    mesh = -1;
    int                    skin = -1;
    std::string            extras_json;
};

struct Skin {
    std::string                         name;
    std::vector<int>                    joints;            // node indices
    std::vector<std::array<float, 16>>  inverse_bind;      // column-major, one per joint
};

struct Channel {
    int                 node = -1;
    bool                rotation = false;   // false = translation
    std::vector<float>  times;
    std::vector<float>  values;             // 3 (translation) or 4 (rotation xyzw) per key
};

struct Animation {
    std::string           name;
    std::vector<Channel>  channels;
};

struct Scene {
    std::string             name;
    std::vector<Image>      images;
    std::vector<Material>   materials;
    std::vector<Mesh>       meshes;
    std::vector<Node>       nodes;
    std::vector<Skin>       skins;
    std::vector<Animation>  animations;
    std::string             extras_json;     // optional raw JSON object on the scene
};

bool write_glb(const std::filesystem::path& path, const Scene& scene,
               std::string& err);

// Encode RGBA pixels to a PNG byte stream (stored deflate — larger than a
// compressed PNG but dependency-free and valid for every decoder).
std::vector<std::uint8_t> encode_png(std::uint32_t width, std::uint32_t height,
                                     const std::vector<std::uint8_t>& rgba);

}  // namespace ovl::gltf
