#pragma once
// Cooked assets: what the engine loads (docs/plans/2026-11-11-step5-assets.md). A cooked file is its header and
// plain arrays, read in place from a mapping; the cooker (ohao/cook) writes them and nothing else reads a source file.
//
// - A model file holds the meshes of one source file: each mesh's vertices, indices, the material slot of each
//   triangle, its materials, and the names of the joints its vertices index.
// - A texture file holds one image, named by the hash of its pixels: 8-bit RGBA, level 0 (the mips are made at
//   upload until 5e cooks them compressed). Whether it is colour or data is its user's to say, not the file's.
#include "asset/content_hash.hpp"
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

inline constexpr uint32_t kFormatVersion = 1;

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

// A mesh as the engine reads it: views of arrays something else keeps alive (a cooked file's mapping, or the
// vectors of a mesh made in code). The engine holds one through a shared_ptr that owns that something.
struct Mesh {
    std::string_view name;
    std::string_view error;   // not empty: the importer could not make this mesh, and the rest is empty
    std::span<const Vertex> vertices;
    std::span<const uint32_t> indices;
    std::span<const uint32_t> materialOfTriangle;
    std::span<const Material> materials;
    std::vector<std::string_view> bones;
};

// What a writer is given for one mesh.
struct MeshSource {
    std::string name, error;
    std::span<const Vertex> vertices;
    std::span<const uint32_t> indices;
    std::span<const uint32_t> materialOfTriangle;
    std::vector<Material> materials;
    std::vector<std::string> bones;
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
    [[nodiscard]] static Result<std::shared_ptr<const ModelFile>> open(const std::filesystem::path& path);
    [[nodiscard]] std::size_t meshCount() const noexcept { return m_meshes.size(); }
    [[nodiscard]] const Mesh& mesh(std::size_t i) const { return m_meshes[i]; }
    // The index of the mesh named `mesh`, or "#<index>"; the error names `path`.
    [[nodiscard]] Result<std::size_t> find(std::string_view path, std::string_view mesh) const;

private:
    std::shared_ptr<const Mapping> m_map;
    std::vector<Mesh> m_meshes;
};

struct Texture {
    std::shared_ptr<const Mapping> map;
    uint32_t width{0}, height{0};
    std::span<const uint8_t> pixels;   // RGBA8, level 0
};
[[nodiscard]] Result<Texture> openTexture(const std::filesystem::path& path);

// Written to a temporary beside `path`, then renamed over it: a reader never sees half a file.
[[nodiscard]] VoidResult<> writeModel(const std::filesystem::path& path, std::span<const MeshSource> meshes);
[[nodiscard]] VoidResult<> writeTexture(const std::filesystem::path& path, uint32_t width, uint32_t height,
                                        std::span<const uint8_t> rgba8);

} // namespace ohao::cooked
