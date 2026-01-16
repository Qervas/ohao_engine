#include "material_component.hpp"
#include "engine/actor/actor.hpp"
#include "ui/components/console_widget.hpp"
// Registration moved to central file

namespace ohao {

MaterialComponent::MaterialComponent() {
    // Initialize with default material
    material = Material();
    material.name = "Default Material";
}

MaterialComponent::~MaterialComponent() {
    // Cleanup handled by base Component
}

void MaterialComponent::setMaterial(const Material& mat) {
    material = mat;
    updateTextureUsage();
}

Material& MaterialComponent::getMaterial() {
    return material;
}

const Material& MaterialComponent::getMaterial() const {
    return material;
}

void MaterialComponent::setAlbedoTexture(const std::string& path) {
    material.setAlbedoTexture(path);
    updateTextureUsage();
}

void MaterialComponent::setNormalTexture(const std::string& path) {
    material.setNormalTexture(path);
    updateTextureUsage();
}

void MaterialComponent::setMetallicTexture(const std::string& path) {
    material.setMetallicTexture(path);
    updateTextureUsage();
}

void MaterialComponent::setRoughnessTexture(const std::string& path) {
    material.setRoughnessTexture(path);
    updateTextureUsage();
}

void MaterialComponent::setAoTexture(const std::string& path) {
    material.setAoTexture(path);
    updateTextureUsage();
}

void MaterialComponent::setEmissiveTexture(const std::string& path) {
    material.setEmissiveTexture(path);
    updateTextureUsage();
}

void MaterialComponent::applyPreset(Material::Type type) {
    material.type = type;
    material.applyPreset();
    updateTextureUsage();
}

bool MaterialComponent::hasTextures() const {
    return material.hasTextures();
}

const std::string& MaterialComponent::getAlbedoTexture() const {
    return material.albedoTexture;
}

const std::string& MaterialComponent::getNormalTexture() const {
    return material.normalTexture;
}

const std::string& MaterialComponent::getMetallicTexture() const {
    return material.metallicTexture;
}

const std::string& MaterialComponent::getRoughnessTexture() const {
    return material.roughnessTexture;
}

const std::string& MaterialComponent::getAoTexture() const {
    return material.aoTexture;
}

const std::string& MaterialComponent::getEmissiveTexture() const {
    return material.emissiveTexture;
}

const char* MaterialComponent::getTypeName() const {
    return "MaterialComponent";
}

void MaterialComponent::initialize() {
    // Component initialization
    OHAO_LOG("MaterialComponent initialized for actor: " + (owner ? owner->getName() : "Unknown"));
}

void MaterialComponent::render() {
    // Materials don't render themselves - they're used by renderers
}

void MaterialComponent::destroy() {
    // Cleanup
}

void MaterialComponent::serialize(class Serializer& serializer) const {}
void MaterialComponent::deserialize(class Deserializer& deserializer) {}

void MaterialComponent::updateTextureUsage() {
    // Texture loading is handled by OffscreenRenderer
    // MaterialComponent just stores the texture paths for the renderer to use
}

} // namespace ohao