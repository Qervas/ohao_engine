#pragma once

#include "core/concepts.hpp"

#include <glm/glm.hpp>
#include <string>
#include <string_view>

namespace ohao {

struct Material {
    // PBR Core Properties
    glm::vec3 baseColor{0.8f, 0.8f, 0.8f};    // Albedo/Diffuse color
    float metallic{0.0f};                      // 0=dielectric, 1=metal
    float roughness{0.5f};                     // 0=mirror, 1=completely rough
    float ao{1.0f};                            // Ambient occlusion factor
    
    // Advanced PBR Properties
    glm::vec3 emissive{0.0f};                  // Self-illumination
    float ior{1.45f};                          // Index of refraction for dielectrics
    float transmission{0.0f};                  // For glass/transparent materials
    float clearCoat{0.0f};                     // Clear coat layer intensity
    float clearCoatRoughness{0.03f};           // Clear coat roughness
    
    // Subsurface Scattering (for skin, wax, etc.)
    glm::vec3 subsurface{0.0f};                // Subsurface scattering color
    float subsurfaceRadius{1.0f};              // Scattering radius
    
    // Additional Properties
    float normalIntensity{1.0f};               // Normal map intensity
    float heightScale{0.05f};                  // Height/displacement scale
    
    // Texture Maps
    std::string albedoTexture;                 // Base color/diffuse texture
    std::string normalTexture;                 // Normal map
    std::string metallicTexture;               // Metallic map
    std::string roughnessTexture;              // Roughness map
    std::string aoTexture;                     // Ambient occlusion map
    std::string emissiveTexture;               // Emissive map
    std::string heightTexture;                 // Height/displacement map
    
    // Combined maps (common in game engines)
    std::string metallicRoughnessTexture;      // R=?, G=roughness, B=metallic
    std::string occlusionRoughnessMetallicTexture; // R=AO, G=roughness, B=metallic (ORM)
    
    // Texture Usage Flags
    bool useAlbedoTexture{false};
    bool useNormalTexture{false};
    bool useMetallicTexture{false};
    bool useRoughnessTexture{false};
    bool useAoTexture{false};
    bool useEmissiveTexture{false};
    bool useHeightTexture{false};
    
    // Material Type Presets
    enum class Type {
        Custom,
        Metal,
        Plastic,
        Glass,
        Rubber,
        Fabric,
        Skin,
        Wood,
        Concrete,
        Gold,
        Silver,
        Copper,
        Chrome
    };
    
    Type type{Type::Custom};
    std::string name{"Default Material"};

    [[nodiscard]] constexpr int typeIndex() const noexcept {
        return static_cast<int>(to_underlying(type));
    }

    // Utility methods for common material types
    [[nodiscard]] static Material createMetal(const glm::vec3& color, float roughness = 0.1f);
    [[nodiscard]] static Material createPlastic(const glm::vec3& color, float roughness = 0.7f);
    [[nodiscard]] static Material createGlass(const glm::vec3& tint = glm::vec3(1.0f), float roughness = 0.0f);
    [[nodiscard]] static Material createRubber(const glm::vec3& color, float roughness = 0.9f);
    [[nodiscard]] static Material createGold();
    [[nodiscard]] static Material createSilver();
    [[nodiscard]] static Material createChrome();
    
    // Texture utility methods
    [[nodiscard]] static Material createTexturedMaterial(std::string_view albedoPath, 
                                         std::string_view normalPath = "",
                                         std::string_view roughnessPath = "",
                                         std::string_view metallicPath = "");
    
    // Apply preset based on type
    void applyPreset();
    
    // Texture management
    void setAlbedoTexture(std::string_view path);
    void setNormalTexture(std::string_view path);
    void setMetallicTexture(std::string_view path);
    void setRoughnessTexture(std::string_view path);
    void setAoTexture(std::string_view path);
    void setEmissiveTexture(std::string_view path);
    
    // Check if material has any textures
    [[nodiscard]] bool hasTextures() const;
};

} // namespace ohao
