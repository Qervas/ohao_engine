#include "material_component.hpp"
#include "scene/actor/actor.hpp"
#include "core/console_widget.hpp"
#include <string>
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

void MaterialComponent::setAlbedoTexture(std::string_view path) {
    material.setAlbedoTexture(std::string(path));
    updateTextureUsage();
}

void MaterialComponent::setNormalTexture(std::string_view path) {
    material.setNormalTexture(std::string(path));
    updateTextureUsage();
}

void MaterialComponent::setMetallicTexture(std::string_view path) {
    material.setMetallicTexture(std::string(path));
    updateTextureUsage();
}

void MaterialComponent::setRoughnessTexture(std::string_view path) {
    material.setRoughnessTexture(std::string(path));
    updateTextureUsage();
}

void MaterialComponent::setAoTexture(std::string_view path) {
    material.setAoTexture(std::string(path));
    updateTextureUsage();
}

void MaterialComponent::setEmissiveTexture(std::string_view path) {
    material.setEmissiveTexture(std::string(path));
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

void MaterialComponent::updateTextureUsage() {
    // Texture loading is handled by VulkanRenderer
    // MaterialComponent just stores the texture paths for the renderer to use
}

} // namespace ohao