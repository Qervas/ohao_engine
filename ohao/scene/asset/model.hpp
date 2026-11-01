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

    // The joints Vertex::boneIndices index, by name, in that index order (Assimp numbers bones
    // file-wide in the order it meets them, not in a skin's joint order: an animation skeleton
    // maps them by name, anim::boneToJointMap). Empty for a model without bones.
    std::vector<std::string> boneNames;

    // Per-material base color: vec4(r, g, b, roughness)
    std::vector<glm::vec4> materialColors;
    // Per-material metallic factor (0 = dielectric, 1 = metal)
    std::vector<float> materialMetallic;
    // Per-material sidedness (glTF doubleSided, through Assimp's TWOSIDED): 0 one-sided, its
    // back faces culled. Empty (OBJ, FBX): every material double-sided.
    std::vector<uint8_t> materialDoubleSided;
    // Per-material alpha (glTF alphaMode, alphaCutoff, baseColorFactor.a, through Assimp): mode 0
    // opaque, 1 mask, 2 blend (render::AlphaMode). Empty: every material opaque.
    std::vector<uint8_t> materialAlphaMode;
    std::vector<float> materialAlphaCutoff;
    std::vector<float> materialBaseAlpha;
    // Per-material OpenPBR inputs from glTF's KHR_materials_* (through Assimp; OpenPBR's defaults
    // where absent): specular_ior (KHR_materials_ior), specular_weight and specular_color
    // (KHR_materials_specular's factors), emission_color (emissiveFactor) and its luminance
    // (KHR_materials_emissive_strength, 1 without it).
    std::vector<float> materialIor;
    std::vector<float> materialSpecularWeight;
    std::vector<glm::vec3> materialSpecularColor;
    std::vector<glm::vec3> materialEmissionColor;
    std::vector<float> materialEmissionStrength;
    // OpenPBR step 2 from KHR_materials_clearcoat (coat_weight, coat_roughness; glTF's coat has
    // IOR 1.5 and no darkening) and KHR_materials_sheen (fuzz_color, fuzz_roughness; fuzz_weight 1
    // where the colour is not black): their factors here, their textures below. The coat's normal
    // texture is not read (the coat shares the base's normal).
    std::vector<float> materialCoatWeight;
    std::vector<float> materialCoatRoughness;
    std::vector<glm::vec3> materialFuzzColor;
    std::vector<float> materialFuzzRoughness;
    // OpenPBR step 3 from KHR_materials_transmission (transmission_weight: its factor) and
    // KHR_materials_volume (transmission_color and transmission_depth: its attenuation colour and
    // distance; a volume with no distance, infinite, absorbs nothing). Thickness is not read.
    std::vector<float> materialTransmission;
    std::vector<glm::vec3> materialTransmissionColor;
    std::vector<float> materialTransmissionDepth;

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
    std::vector<TextureData> occlusionTextures;
    // KHR_materials_clearcoat's clearcoatTexture (R: coat_weight) and clearcoatRoughnessTexture
    // (G: coat_roughness); KHR_materials_sheen's sheenColorTexture (RGB, sRGB: fuzz_color) and
    // sheenRoughnessTexture (A: fuzz_roughness). Each an index into its list, -1 none; one image
    // packing two of them is decoded and uploaded once per use.
    std::vector<TextureData> coatTextures;
    std::vector<TextureData> coatRoughnessTextures;
    std::vector<TextureData> fuzzColorTextures;
    std::vector<TextureData> fuzzRoughnessTextures;
    std::vector<int> materialCoatTexIndex;
    std::vector<int> materialCoatRoughnessTexIndex;
    std::vector<int> materialFuzzColorTexIndex;
    std::vector<int> materialFuzzRoughnessTexIndex;
    std::vector<int> materialTextureIndex;
    std::vector<int> materialNormalTexIndex;
    std::vector<int> materialRoughMetalTexIndex;
    std::vector<int> materialEmissiveTexIndex;
    // glTF occlusionTexture (AO in R): an index into occlusionTextures, -1 none, or
    // kOcclusionIsRoughMetal when it is the same image as the material's roughMetal texture
    // (packed ORM), which is then uploaded once and sampled for both.
    static constexpr int kOcclusionIsRoughMetal = -2;
    std::vector<int> materialOcclusionTexIndex;

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
