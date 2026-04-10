#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <string>
#include <memory>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vulkan/vulkan_core.h>

namespace ohao{

// Forward declarations for animation types
struct Skeleton;
struct AnimationClip;

struct Vertex{

    glm::vec3 position;
    glm::vec3 color;
    glm::vec3 normal;
    glm::vec2 texCoord;
    glm::vec4 tangent{0.0f, 0.0f, 0.0f, 1.0f};       // xyz = tangent direction, w = handedness (+1/-1)
    glm::ivec4 boneIndices{0, 0, 0, 0};  // Up to 4 bone influences per vertex
    glm::vec4 boneWeights{1.0f, 0.0f, 0.0f, 0.0f};   // Corresponding weights (sum to 1.0)

    static std::vector<VkVertexInputBindingDescription> getBindingDescriptions();
    static std::vector<VkVertexInputAttributeDescription> getAttributeDescriptions();

};

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

    // Animation data (populated by GLTF loader for skinned meshes)
    std::shared_ptr<Skeleton> skeleton;
    std::vector<std::shared_ptr<AnimationClip>> animations;

    // LOD levels (index 0 = highest detail, each subsequent = lower detail)
    // Empty means no LOD support (always use main vertices/indices)
    std::vector<LODLevel> lodLevels;

    // Select appropriate LOD level based on distance to camera
    // Returns -1 for base mesh, or LOD level index
    int selectLOD(float distanceToCamera) const {
        for (int i = 0; i < static_cast<int>(lodLevels.size()); i++) {
            if (lodLevels[i].maxDistance <= 0.0f || distanceToCamera <= lodLevels[i].maxDistance) {
                return i;
            }
        }
        return lodLevels.empty() ? -1 : static_cast<int>(lodLevels.size()) - 1;
    }

    bool loadFromOBJ(const std::string& filename);
    bool loadFromGLTF(const std::string& filename);
    bool loadMTL(const std::string& filename);
    void setupDefaultMaterial();

    bool hasSkeleton() const { return skeleton.get() != nullptr; }
    bool hasLOD() const { return !lodLevels.empty(); }

    const std::string& getSourcePath() const { return sourcePath; }
    void setSourcePath(const std::string& path) { sourcePath = path; }

private:
    void assignMaterialColors();
    uint32_t getOrCreateVertex(const std::string& vertexStr,
                              const std::vector<glm::vec3>& positions,
                              const std::vector<glm::vec3>& normals,
                              const std::vector<glm::vec2>& texCoords);

    std::string sourcePath;
    std::unordered_map<std::string, uint32_t> vertexMap; // For vertex deduplication
};
}
