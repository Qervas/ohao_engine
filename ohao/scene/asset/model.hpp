#pragma once
#include "core/concepts.hpp"
#include <cstdint>
#include <cstddef>
#include <span>
#include <unordered_map>
#include <vector>
#include <string>
#include <string_view>
#include <memory>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vulkan/vulkan_core.h>

namespace ohao{

struct Vertex{

    glm::vec3 position{};
    glm::vec3 color{};
    glm::vec3 normal{};
    glm::vec2 texCoord{};
    glm::vec2 texCoord1{0.0f, 0.0f};    // Second UV set (GLTF texCoord: 1)
    glm::vec4 tangent{0.0f, 0.0f, 0.0f, 1.0f};       // xyz = tangent direction, w = handedness (+1/-1)
    glm::ivec4 boneIndices{0, 0, 0, 0};  // Up to 4 bone influences per vertex
    glm::vec4 boneWeights{1.0f, 0.0f, 0.0f, 0.0f};   // Corresponding weights (sum to 1.0)

    [[nodiscard]] static std::vector<VkVertexInputBindingDescription> getBindingDescriptions();
    [[nodiscard]] static std::vector<VkVertexInputAttributeDescription> getAttributeDescriptions();

};
static_assert(GpuPod<Vertex>, "Vertex must be GPU-uploadable POD");
static_assert(offsetof(Vertex, position) == 0, "Vertex.position must be at offset 0");
static_assert(offsetof(Vertex, color) == sizeof(glm::vec3), "Vertex.color offset");
static_assert(offsetof(Vertex, normal) == 2 * sizeof(glm::vec3), "Vertex.normal offset");

struct MaterialData{
    std::string name;
    glm::vec3 ambient;
    glm::vec3 diffuse;
    glm::vec3 specular;
    glm::vec3 emission;
    float shininess;
    float ior;      // Index of refraction
    float opacity;
    int illum;      // Illumination model

    // Texture maps (OBJ/MTL)
    std::string diffuseTexture;     // map_Kd
    std::string ambientTexture;     // map_Ka
    std::string specularTexture;    // map_Ks
    std::string normalTexture;      // map_Bump or bump
    std::string heightTexture;      // map_d or map_disp

    // PBR fields (GLTF)
    glm::vec4 baseColorFactor{1.0f};
    float metallicFactor{1.0f};
    float roughnessFactor{1.0f};
    std::string baseColorTexture;
    std::string metallicRoughnessTexture;
    std::string occlusionTexture;
    std::string emissiveTexture;
#ifdef OPAQUE
#undef OPAQUE  // Windows wingdi.h conflict
#endif
    enum class AlphaMode { OPAQUE, MASK, BLEND } alphaMode = AlphaMode::OPAQUE;
    float alphaCutoff{0.5f};
    bool doubleSided{false};

    bool isLight{false};
    glm::vec3 lightPosition{0.0f};
    float lightIntensity{1.0f};
};

// LOD level data - reduced-polygon version of a mesh
struct LODLevel {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    float maxDistance{0.0f};  // Max camera distance for this LOD (0 = use as last)
};

class Model{
public:
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::unordered_map<std::string, MaterialData> materials;
    std::vector<std::string> materialAssignments;

    // Per-triangle material index (for RT texture lookup)
    // materialPerTriangle[triangleIdx] = GLTF material index
    std::vector<uint32_t> materialPerTriangle;

    // Per-material base color: vec4(r, g, b, roughness)
    std::vector<glm::vec4> materialColors;
    // Per-material metallic factor (0 = dielectric, 1 = metal)
    std::vector<float> materialMetallic;

    // Per-material texture data (RGBA pixels, decoded from GLTF images)
    struct TextureData {
        std::vector<uint8_t> pixels;  // RGBA8
        int width = 0, height = 0;
        int materialIndex = -1;       // which material this texture belongs to
    };
    std::vector<TextureData> albedoTextures;
    std::vector<TextureData> normalTextures;
    std::vector<TextureData> roughMetalTextures;
    std::vector<TextureData> emissiveTextures;
    std::vector<int> materialTextureIndex;
    std::vector<int> materialNormalTexIndex;
    std::vector<int> materialRoughMetalTexIndex;
    std::vector<int> materialEmissiveTexIndex;

    // LOD levels (index 0 = highest detail, each subsequent = lower detail)
    // Empty means no LOD support (always use main vertices/indices)
    std::vector<LODLevel> lodLevels;

    // Select appropriate LOD level based on distance to camera
    // Returns -1 for base mesh, or LOD level index
    [[nodiscard]] int selectLOD(float distanceToCamera) const {
        for (int i = 0; i < static_cast<int>(lodLevels.size()); i++) {
            if (lodLevels[i].maxDistance <= 0.0f || distanceToCamera <= lodLevels[i].maxDistance) {
                return i;
            }
        }
        return lodLevels.empty() ? -1 : static_cast<int>(lodLevels.size()) - 1;
    }

    [[nodiscard]] bool loadFromOBJ(std::string_view filename);
    [[nodiscard]] bool loadFromGLTF(std::string_view filename);
    [[nodiscard]] bool loadFromFBX(std::string_view filename);  // Assimp-based (FBX, Collada, etc.)
    [[nodiscard]] bool loadMTL(std::string_view filename);
    void setupDefaultMaterial();

    [[nodiscard]] bool hasLOD() const noexcept { return !lodLevels.empty(); }
    [[nodiscard]] bool empty() const noexcept { return vertices.empty() || indices.empty(); }
    [[nodiscard]] std::size_t triangleCount() const noexcept { return indices.size() / 3; }

    [[nodiscard]] std::span<const Vertex> vertexSpan() const noexcept { return vertices; }
    [[nodiscard]] std::span<Vertex> vertexSpan() noexcept { return vertices; }
    [[nodiscard]] std::span<const std::uint32_t> indexSpan() const noexcept { return indices; }
    [[nodiscard]] std::span<std::uint32_t> indexSpan() noexcept { return indices; }

    [[nodiscard]] const std::string& getSourcePath() const noexcept { return sourcePath; }
    void setSourcePath(std::string_view path) { sourcePath = std::string(path); }

private:
    void assignMaterialColors();
    [[nodiscard]] uint32_t getOrCreateVertex(std::string_view vertexStr,
                              const std::vector<glm::vec3>& positions,
                              const std::vector<glm::vec3>& normals,
                              const std::vector<glm::vec2>& texCoords);

    std::string sourcePath;
    std::unordered_map<std::string, uint32_t> vertexMap; // For vertex deduplication
};
}
