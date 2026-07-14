#pragma once

#include <glm/glm.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <memory>
#include <vector>

namespace ohao {
namespace physics {

// === PHYSICS MATERIAL CLASS ===
class PhysicsMaterial {
public:
    PhysicsMaterial(std::string_view name = "Default");
    
    // === MATERIAL PROPERTIES ===
    
    // Density (kg/m³) - used for automatic mass calculation from volume
    void setDensity(float density) { m_density = glm::max(density, 0.001f); }
    [[nodiscard]] float getDensity() const noexcept { return m_density; }
    
    // Restitution (bounciness) - 0 = no bounce, 1 = perfect bounce
    void setRestitution(float restitution) { m_restitution = glm::clamp(restitution, 0.0f, 1.0f); }
    [[nodiscard]] float getRestitution() const noexcept { return m_restitution; }
    
    // Static friction coefficient - resistance when not moving
    void setStaticFriction(float friction) { m_staticFriction = glm::max(friction, 0.0f); }
    [[nodiscard]] float getStaticFriction() const noexcept { return m_staticFriction; }
    
    // Dynamic friction coefficient - resistance when sliding
    void setDynamicFriction(float friction) { m_dynamicFriction = glm::max(friction, 0.0f); }
    [[nodiscard]] float getDynamicFriction() const noexcept { return m_dynamicFriction; }
    
    // Surface properties
    void setRoughness(float roughness) { m_roughness = glm::clamp(roughness, 0.0f, 1.0f); }
    [[nodiscard]] float getRoughness() const noexcept { return m_roughness; }
    
    // Material identification
    [[nodiscard]] const std::string& getName() const noexcept { return m_name; }
    void setName(std::string_view name) { m_name = std::string(name); }
    
    // === MATERIAL COMBINATION RULES ===
    
    // How to combine two materials when they collide
    enum class CombineMode {
        AVERAGE,    // (a + b) / 2
        MINIMUM,    // min(a, b)  
        MAXIMUM,    // max(a, b)
        MULTIPLY    // a * b
    };
    
    void setRestitutionCombine(CombineMode mode) { m_restitutionCombine = mode; }
    void setFrictionCombine(CombineMode mode) { m_frictionCombine = mode; }
    
    CombineMode getRestitutionCombine() const { return m_restitutionCombine; }
    CombineMode getFrictionCombine() const { return m_frictionCombine; }
    
    // === COMBINATION CALCULATIONS ===
    
    // Combine two materials for contact resolution
    static float combineRestitution(const PhysicsMaterial* matA, const PhysicsMaterial* matB);
    static float combineStaticFriction(const PhysicsMaterial* matA, const PhysicsMaterial* matB);
    static float combineDynamicFriction(const PhysicsMaterial* matA, const PhysicsMaterial* matB);
    
private:
    // Material name
    std::string m_name;
    
    // Physical properties
    float m_density{1000.0f};          // Water density as default
    float m_restitution{0.3f};         // Moderate bounciness
    float m_staticFriction{0.6f};      // Typical static friction
    float m_dynamicFriction{0.4f};     // Lower than static
    float m_roughness{0.5f};           // Medium surface roughness
    
    // Combination modes
    CombineMode m_restitutionCombine{CombineMode::AVERAGE};
    CombineMode m_frictionCombine{CombineMode::AVERAGE};
    
    // Helper function for combination calculations
    static float combineValues(float a, float b, CombineMode mode);
};

// === MATERIAL LIBRARY ===
class MaterialLibrary {
public:
    static MaterialLibrary& getInstance();
    
    // Predefined materials
    void initializePredefinedMaterials();
    
    // Material management
    std::shared_ptr<PhysicsMaterial> createMaterial(std::string_view name);
    [[nodiscard]] std::shared_ptr<PhysicsMaterial> getMaterial(std::string_view name);
    [[nodiscard]] bool hasMaterial(std::string_view name) const;
    
    // Predefined material getters
    std::shared_ptr<PhysicsMaterial> getDefault() { return getMaterial("Default"); }
    std::shared_ptr<PhysicsMaterial> getSteel() { return getMaterial("Steel"); }
    std::shared_ptr<PhysicsMaterial> getWood() { return getMaterial("Wood"); }
    std::shared_ptr<PhysicsMaterial> getRubber() { return getMaterial("Rubber"); }
    std::shared_ptr<PhysicsMaterial> getIce() { return getMaterial("Ice"); }
    std::shared_ptr<PhysicsMaterial> getConcrete() { return getMaterial("Concrete"); }
    
    // Get all material names
    std::vector<std::string> getAllMaterialNames() const;
    
private:
    MaterialLibrary() = default;
    std::unordered_map<std::string, std::shared_ptr<PhysicsMaterial>> m_materials;
    
    void createPredefinedMaterial(std::string_view name, float density,
                                 float restitution, float staticFriction,
                                 float dynamicFriction, float roughness = 0.5f);
};

} // namespace physics
} // namespace ohao