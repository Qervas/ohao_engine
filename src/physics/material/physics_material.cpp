#include "physics_material.hpp"
#include <algorithm>

namespace ohao {
namespace physics {

// === PHYSICS MATERIAL IMPLEMENTATION ===

PhysicsMaterial::PhysicsMaterial(const std::string& name) 
    : m_name(name) {
}

float PhysicsMaterial::combineRestitution(const PhysicsMaterial* matA, const PhysicsMaterial* matB) {
    if (!matA) return matB ? matB->getRestitution() : 0.0f;
    if (!matB) return matA->getRestitution();
    
    // Use the more restrictive combine mode
    CombineMode mode = (matA->getRestitutionCombine() == CombineMode::MINIMUM || 
                       matB->getRestitutionCombine() == CombineMode::MINIMUM) ? 
                       CombineMode::MINIMUM : matA->getRestitutionCombine();
    
    return combineValues(matA->getRestitution(), matB->getRestitution(), mode);
}

float PhysicsMaterial::combineStaticFriction(const PhysicsMaterial* matA, const PhysicsMaterial* matB) {
    if (!matA) return matB ? matB->getStaticFriction() : 0.5f;
    if (!matB) return matA->getStaticFriction();
    
    CombineMode mode = matA->getFrictionCombine();
    return combineValues(matA->getStaticFriction(), matB->getStaticFriction(), mode);
}

float PhysicsMaterial::combineDynamicFriction(const PhysicsMaterial* matA, const PhysicsMaterial* matB) {
    if (!matA) return matB ? matB->getDynamicFriction() : 0.3f;
    if (!matB) return matA->getDynamicFriction();
    
    CombineMode mode = matA->getFrictionCombine();
    return combineValues(matA->getDynamicFriction(), matB->getDynamicFriction(), mode);
}

float PhysicsMaterial::combineValues(float a, float b, CombineMode mode) {
    switch (mode) {
        case CombineMode::AVERAGE:
            return (a + b) * 0.5f;
        case CombineMode::MINIMUM:
            return std::min(a, b);
        case CombineMode::MAXIMUM:
            return std::max(a, b);
        case CombineMode::MULTIPLY:
            return a * b;
        default:
            return (a + b) * 0.5f;
    }
}

// === MATERIAL LIBRARY IMPLEMENTATION ===

MaterialLibrary& MaterialLibrary::getInstance() {
    static MaterialLibrary instance;
    return instance;
}

void MaterialLibrary::initializePredefinedMaterials() {
    // Clear existing materials
    m_materials.clear();
    
    // Create predefined materials with realistic properties
    // Format: (name, density_kg/m³, restitution, static_friction, dynamic_friction, roughness)
    
    createPredefinedMaterial("Default", 1000.0f, 0.3f, 0.6f, 0.4f, 0.5f);
    
    // Metals
    createPredefinedMaterial("Steel", 7850.0f, 0.2f, 0.8f, 0.6f, 0.3f);
    createPredefinedMaterial("Aluminum", 2700.0f, 0.25f, 0.7f, 0.5f, 0.4f);
    createPredefinedMaterial("Iron", 7870.0f, 0.15f, 0.9f, 0.7f, 0.3f);
    
    // Organic materials
    createPredefinedMaterial("Wood", 600.0f, 0.4f, 0.5f, 0.3f, 0.7f);
    createPredefinedMaterial("Rubber", 1200.0f, 0.9f, 1.2f, 0.8f, 0.9f);
    createPredefinedMaterial("Plastic", 950.0f, 0.3f, 0.4f, 0.3f, 0.6f);
    
    // Stone/Ceramic materials  
    createPredefinedMaterial("Concrete", 2400.0f, 0.1f, 0.8f, 0.6f, 0.4f);
    createPredefinedMaterial("Stone", 2700.0f, 0.05f, 0.9f, 0.7f, 0.3f);
    createPredefinedMaterial("Glass", 2500.0f, 0.05f, 0.6f, 0.4f, 0.1f);
    
    // Special materials
    createPredefinedMaterial("Ice", 917.0f, 0.02f, 0.1f, 0.05f, 0.1f);
    createPredefinedMaterial("Mud", 1800.0f, 0.0f, 0.8f, 0.9f, 1.0f);
    createPredefinedMaterial("Sand", 1600.0f, 0.1f, 0.7f, 0.5f, 0.8f);
    
    // Set special combine modes for specific materials
    if (auto ice = getMaterial("Ice")) {
        ice->setFrictionCombine(PhysicsMaterial::CombineMode::MINIMUM);
    }
    if (auto rubber = getMaterial("Rubber")) {
        rubber->setRestitutionCombine(PhysicsMaterial::CombineMode::MAXIMUM);
        rubber->setFrictionCombine(PhysicsMaterial::CombineMode::MAXIMUM);
    }
}

std::shared_ptr<PhysicsMaterial> MaterialLibrary::createMaterial(const std::string& name) {
    auto material = std::make_shared<PhysicsMaterial>(name);
    m_materials[name] = material;
    return material;
}

std::shared_ptr<PhysicsMaterial> MaterialLibrary::getMaterial(const std::string& name) {
    auto it = m_materials.find(name);
    if (it != m_materials.end()) {
        return it->second;
    }
    
    // If material doesn't exist, create a default one
    return createMaterial(name);
}

bool MaterialLibrary::hasMaterial(const std::string& name) const {
    return m_materials.find(name) != m_materials.end();
}

std::vector<std::string> MaterialLibrary::getAllMaterialNames() const {
    std::vector<std::string> names;
    names.reserve(m_materials.size());
    
    for (const auto& pair : m_materials) {
        names.push_back(pair.first);
    }
    
    std::sort(names.begin(), names.end());
    return names;
}

void MaterialLibrary::createPredefinedMaterial(const std::string& name, float density, 
                                              float restitution, float staticFriction, 
                                              float dynamicFriction, float roughness) {
    auto material = createMaterial(name);
    material->setDensity(density);
    material->setRestitution(restitution);
    material->setStaticFriction(staticFriction);
    material->setDynamicFriction(dynamicFriction);
    material->setRoughness(roughness);
}

} // namespace physics
} // namespace ohao