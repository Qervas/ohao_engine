#include "material.hpp"

namespace ohao {

Material Material::createMetal(const glm::vec3& color, float roughness) {
    Material mat;
    mat.type = Type::Metal;
    mat.baseColor = color;
    mat.metallic = 1.0f;
    mat.roughness = roughness;
    mat.ao = 1.0f;
    mat.emissive = glm::vec3(0.0f);
    mat.ior = 1.0f; // Metals have complex IOR, simplified here
    mat.name = "Metal";
    return mat;
}

Material Material::createPlastic(const glm::vec3& color, float roughness) {
    Material mat;
    mat.type = Type::Plastic;
    mat.baseColor = color;
    mat.metallic = 0.0f;
    mat.roughness = roughness;
    mat.ao = 1.0f;
    mat.emissive = glm::vec3(0.0f);
    mat.ior = 1.5f; // Typical for plastic
    mat.name = "Plastic";
    return mat;
}

Material Material::createGlass(const glm::vec3& tint, float roughness) {
    Material mat;
    mat.type = Type::Glass;
    mat.baseColor = tint;
    mat.metallic = 0.0f;
    mat.roughness = roughness;
    mat.ao = 1.0f;
    mat.emissive = glm::vec3(0.0f);
    mat.ior = 1.52f; // Glass IOR
    mat.transmission = 0.9f; // Highly transmissive
    mat.name = "Glass";
    return mat;
}

Material Material::createRubber(const glm::vec3& color, float roughness) {
    Material mat;
    mat.type = Type::Rubber;
    mat.baseColor = color;
    mat.metallic = 0.0f;
    mat.roughness = roughness;
    mat.ao = 1.0f;
    mat.emissive = glm::vec3(0.0f);
    mat.ior = 1.519f; // Rubber IOR
    mat.subsurface = color * 0.1f; // Slight subsurface scattering
    mat.name = "Rubber";
    return mat;
}

Material Material::createGold() {
    Material mat;
    mat.type = Type::Gold;
    mat.baseColor = glm::vec3(1.0f, 0.766f, 0.336f); // Gold color
    mat.metallic = 1.0f;
    mat.roughness = 0.1f;
    mat.ao = 1.0f;
    mat.emissive = glm::vec3(0.0f);
    mat.ior = 1.0f;
    mat.name = "Gold";
    return mat;
}

Material Material::createSilver() {
    Material mat;
    mat.type = Type::Silver;
    mat.baseColor = glm::vec3(0.972f, 0.960f, 0.915f); // Silver color
    mat.metallic = 1.0f;
    mat.roughness = 0.05f;
    mat.ao = 1.0f;
    mat.emissive = glm::vec3(0.0f);
    mat.ior = 1.0f;
    mat.name = "Silver";
    return mat;
}

Material Material::createChrome() {
    Material mat;
    mat.type = Type::Chrome;
    mat.baseColor = glm::vec3(0.549f, 0.556f, 0.554f); // Chrome color
    mat.metallic = 1.0f;
    mat.roughness = 0.02f; // Very smooth
    mat.ao = 1.0f;
    mat.emissive = glm::vec3(0.0f);
    mat.ior = 1.0f;
    mat.name = "Chrome";
    return mat;
}

void Material::applyPreset() {
    switch (type) {
        case Type::Metal:
            *this = createMetal(baseColor, roughness);
            break;
        case Type::Plastic:
            *this = createPlastic(baseColor, roughness);
            break;
        case Type::Glass:
            *this = createGlass(baseColor, roughness);
            break;
        case Type::Rubber:
            *this = createRubber(baseColor, roughness);
            break;
        case Type::Gold:
            *this = createGold();
            break;
        case Type::Silver:
            *this = createSilver();
            break;
        case Type::Chrome:
            *this = createChrome();
            break;
        case Type::Wood:
            baseColor = glm::vec3(0.48f, 0.33f, 0.23f);
            metallic = 0.0f;
            roughness = 0.8f;
            ao = 0.9f;
            subsurface = glm::vec3(0.1f, 0.05f, 0.02f);
            name = "Wood";
            break;
        case Type::Concrete:
            baseColor = glm::vec3(0.6f, 0.6f, 0.6f);
            metallic = 0.0f;
            roughness = 0.9f;
            ao = 0.8f;
            name = "Concrete";
            break;
        case Type::Fabric:
            baseColor = glm::vec3(0.7f, 0.7f, 0.8f);
            metallic = 0.0f;
            roughness = 1.0f;
            ao = 0.9f;
            subsurface = baseColor * 0.3f;
            name = "Fabric";
            break;
        case Type::Skin:
            baseColor = glm::vec3(0.92f, 0.78f, 0.62f);
            metallic = 0.0f;
            roughness = 0.6f;
            ao = 0.95f;
            subsurface = glm::vec3(0.48f, 0.16f, 0.16f);
            subsurfaceRadius = 2.0f;
            name = "Skin";
            break;
        case Type::Copper:
            baseColor = glm::vec3(0.955f, 0.637f, 0.538f);
            metallic = 1.0f;
            roughness = 0.12f;
            ao = 1.0f;
            name = "Copper";
            break;
        case Type::Custom:
        default:
            // Keep current values
            break;
    }
}

Material Material::createTexturedMaterial(const std::string& albedoPath, 
                                         const std::string& normalPath,
                                         const std::string& roughnessPath,
                                         const std::string& metallicPath) {
    Material mat;
    mat.type = Type::Custom;
    mat.name = "Textured Material";
    
    if (!albedoPath.empty()) {
        mat.setAlbedoTexture(albedoPath);
    }
    if (!normalPath.empty()) {
        mat.setNormalTexture(normalPath);
    }
    if (!roughnessPath.empty()) {
        mat.setRoughnessTexture(roughnessPath);
    }
    if (!metallicPath.empty()) {
        mat.setMetallicTexture(metallicPath);
    }
    
    return mat;
}

void Material::setAlbedoTexture(const std::string& path) {
    albedoTexture = path;
    useAlbedoTexture = !path.empty();
}

void Material::setNormalTexture(const std::string& path) {
    normalTexture = path;
    useNormalTexture = !path.empty();
}

void Material::setMetallicTexture(const std::string& path) {
    metallicTexture = path;
    useMetallicTexture = !path.empty();
}

void Material::setRoughnessTexture(const std::string& path) {
    roughnessTexture = path;
    useRoughnessTexture = !path.empty();
}

void Material::setAoTexture(const std::string& path) {
    aoTexture = path;
    useAoTexture = !path.empty();
}

void Material::setEmissiveTexture(const std::string& path) {
    emissiveTexture = path;
    useEmissiveTexture = !path.empty();
}

bool Material::hasTextures() const {
    return useAlbedoTexture || useNormalTexture || useMetallicTexture || 
           useRoughnessTexture || useAoTexture || useEmissiveTexture || useHeightTexture;
}

} // namespace ohao