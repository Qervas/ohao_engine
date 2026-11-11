#pragma once
// Cooked assets: what the engine loads (docs/plans/2026-11-11-step5-assets.md). A cooked file is its header and
// plain arrays, read in place from a mapping; the cooker (ohao/cook) writes them and nothing else reads a source file.
//
// - A model file holds the meshes of one source file: each mesh's vertices, indices, the material slot of each
//   triangle, its materials, and the names of the joints its vertices index.
// - A texture file holds one image as one use of it reads it (TextureKind), named by the hash of that use and its
//   source pixels: every mip level, block-compressed, ready to copy to the GPU, and each channel's smallest value.
#include "asset/content_hash.hpp"
#include "asset/meshlets.hpp"
#include "asset/vertex.hpp"
#include "core/result.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::cooked {

inline constexpr uint32_t kFormatVersion = 3;

enum class TextureSlot : uint32_t {
    Albedo, Normal, RoughMetal, Emissive, Occlusion, Coat, CoatRoughness, FuzzColor, FuzzRoughness, Count
};
inline constexpr std::size_t kTextureSlots = static_cast<std::size_t>(TextureSlot::Count);

// One material of a mesh, as the importer read it.
struct Material {
    glm::vec4 colorRoughness{0.8f, 0.8f, 0.8f, 0.5f};
    glm::vec3 specularColor{1.0f};
    float metallic{0.0f};
    glm::vec3 emissionColor{1.0f};
    float emissionStrength{1.0f};
    glm::vec3 fuzzColor{0.0f};
    float fuzzRoughness{0.0f};
    glm::vec3 transmissionColor{1.0f};
    float transmissionDepth{0.0f};
    float alphaCutoff{0.5f}, baseAlpha{1.0f}, ior{1.5f}, specularWeight{1.0f};
    float coatWeight{0.0f}, coatRoughness{0.0f}, transmission{0.0f};
    uint8_t doubleSided{1}, alphaMode{0};
    uint8_t occlusionIsRoughMetal{0};   // the occlusion texture is the roughMetal one (a packed ORM)
    uint8_t pad{0};
    std::array<ContentHash, kTextureSlots> textures{};   // zero: none
};
static_assert(sizeof(Material) == 112 + 16 * kTextureSlots && alignof(Material) == 8);

// A run of a mesh's triangles with one material slot, and its meshlets in the mesh's list.
struct Submesh {
    uint32_t firstIndex{0}, indexCount{0}, materialSlot{0};
    uint32_t firstMeshlet{0}, meshletCount{0};
};

// A mesh as the engine reads it: views of arrays something else keeps alive (a cooked file's mapping, or the
// vectors of a mesh made in code). The engine holds one through a shared_ptr that owns that something.
struct Mesh {
    // What names this mesh among all cooked meshes: its file's content name and its place in it. Zero for a mesh
    // made in code. What is derived from a mesh and cooked (its collision shape) is named from it.
    ContentHash id{};
    std::string_view name;
    std::string_view error;   // not empty: the importer could not make this mesh, and the rest is empty
    std::span<const Vertex> vertices;
    std::span<const uint32_t> indices;
    std::span<const uint32_t> materialOfTriangle;
    std::span<const Material> materials;
    std::vector<std::string_view> bones;
    // The mesh's submeshes (a mesh without materials: none, it is one run) and their meshlets, as buildMeshlets
    // makes them (offsets into meshletVertices and meshletTriangles). Empty for a mesh made in code: the renderer
    // builds them when it registers the mesh.
    std::span<const Submesh> submeshes;
    std::span<const Meshlet> meshlets;
    std::span<const uint32_t> meshletVertices, meshletTriangles;
};

// What a writer is given for one mesh.
struct MeshSource {
    std::string name, error;
    std::span<const Vertex> vertices;
    std::span<const uint32_t> indices;
    std::span<const uint32_t> materialOfTriangle;
    std::vector<Material> materials;
    std::vector<std::string> bones;
    std::vector<Submesh> submeshes;
    MeshletSet meshlets;
};

// A read-only mapping of a file.
class Mapping {
public:
    [[nodiscard]] static Result<std::shared_ptr<const Mapping>> open(const std::filesystem::path& path);
    ~Mapping();
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    [[nodiscard]] std::span<const uint8_t> bytes() const noexcept { return {m_data, m_size}; }

private:
    Mapping() = default;
    const uint8_t* m_data{nullptr};
    std::size_t m_size{0};
};

class ModelFile {
public:
    // `key`: the file's content name (what the cooker named it by).
    [[nodiscard]] static Result<std::shared_ptr<const ModelFile>> open(const std::filesystem::path& path,
                                                                       const ContentHash& key);
    [[nodiscard]] std::size_t meshCount() const noexcept { return m_meshes.size(); }
    [[nodiscard]] const Mesh& mesh(std::size_t i) const { return m_meshes[i]; }
    // The index of the mesh named `mesh`, or "#<index>"; the error names `path`.
    [[nodiscard]] Result<std::size_t> find(std::string_view path, std::string_view mesh) const;

private:
    std::shared_ptr<const Mapping> m_map;
    std::vector<Mesh> m_meshes;
};

// How a material's slot reads an image, which decides how the image is cooked.
enum class TextureKind : uint32_t {
    Color,        // sRGB colour with alpha: BC7, mips averaged in linear light
    Data,         // linear values in four channels: BC7
    DataOpaque,   // linear values in R, G and B, alpha written 255 (a roughness/metal map): BC7
    Normal,       // a tangent-space normal's x and y in R and G: BC5; the shader rebuilds z
};
[[nodiscard]] constexpr TextureKind kindOf(TextureSlot slot) noexcept {
    switch (slot) {
        case TextureSlot::Albedo: case TextureSlot::Emissive: case TextureSlot::FuzzColor: return TextureKind::Color;
        case TextureSlot::Normal: return TextureKind::Normal;
        case TextureSlot::RoughMetal: return TextureKind::DataOpaque;
        default: return TextureKind::Data;
    }
}

enum class TextureFormat : uint32_t { Rgba8, Bc7, Bc5 };
// The bytes of one level: 4 a pixel, or 16 a block of 4x4 pixels (the last blocks padded).
[[nodiscard]] constexpr uint64_t levelBytes(TextureFormat format, uint32_t width, uint32_t height) noexcept {
    return format == TextureFormat::Rgba8 ? uint64_t{width} * height * 4
                                          : uint64_t{(width + 3) / 4} * ((height + 3) / 4) * 16;
}

struct Texture {
    std::shared_ptr<const Mapping> map;
    uint32_t width{0}, height{0};
    TextureFormat format{TextureFormat::Rgba8};
    bool srgb{false};
    std::array<uint8_t, 4> channelMin{255, 255, 255, 255};   // of the source's level 0
    std::vector<std::span<const uint8_t>> levels;            // level 0 first, down to 1x1
};
[[nodiscard]] Result<Texture> openTexture(const std::filesystem::path& path);

// Written to a temporary beside `path`, then renamed over it: a reader never sees half a file.
[[nodiscard]] VoidResult<> writeModel(const std::filesystem::path& path, std::span<const MeshSource> meshes);
// Any cooked file whose content is its reader's own (a collision shape, the physics backend's): the same way.
[[nodiscard]] VoidResult<> writeFile(const std::filesystem::path& path, std::span<const uint8_t> bytes);
[[nodiscard]] VoidResult<> writeTexture(const std::filesystem::path& path, const Texture& texture);

} // namespace ohao::cooked
