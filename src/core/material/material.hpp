#pragma once
#include <glm/glm.hpp>
#include <string>

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
    
    // Utility methods for common material types
    static Material createMetal(const glm::vec3& color, float roughness = 0.1f);
    static Material createPlastic(const glm::vec3& color, float roughness = 0.7f);
    static Material createGlass(const glm::vec3& tint = glm::vec3(1.0f), float roughness = 0.0f);
    static Material createRubber(const glm::vec3& color, float roughness = 0.9f);
    static Material createGold();
    static Material createSilver();
    static Material createChrome();
    
    // Apply preset based on type
    void applyPreset();
};

} // namespace ohao
