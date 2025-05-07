#include "mesh_component.hpp"
#include "../actor/actor.hpp"
#include "../component/transform_component.hpp"
#include "../scene/scene.hpp"

namespace ohao {

MeshComponent::MeshComponent()
    : visible(true)
    , renderMode(RenderMode::SOLID)
    , vertexOffset(0)
    , indexOffset(0)
    , indexCount(0)
{
}

MeshComponent::~MeshComponent() {
    destroy();
}

void MeshComponent::setModel(std::shared_ptr<Model> newModel) {
    // If we already have this model, don't do anything
    if (model == newModel) return;
    
    model = newModel;
    
    // Reset buffer info - scene will need to rebuild buffers
    vertexOffset = 0;
    indexOffset = 0;
    indexCount = model ? static_cast<uint32_t>(model->indices.size()) : 0;
    
    // Register with scene if part of one
    if (auto actor = getOwner()) {
        if (auto scene = actor->getScene()) {
            scene->onMeshComponentChanged(this);
        }
    }
}

std::shared_ptr<Model> MeshComponent::getModel() const {
    return model;
}

void MeshComponent::setMaterial(const Material& newMaterial) {
    material = newMaterial;
    
    // Notify scene if material changes
    if (auto actor = getOwner()) {
        if (auto scene = actor->getScene()) {
            scene->onMeshComponentChanged(this);
        }
    }
}

Material& MeshComponent::getMaterial() {
    return material;
}

const Material& MeshComponent::getMaterial() const {
    return material;
}

void MeshComponent::setVisible(bool isVisible) {
    visible = isVisible;
}

bool MeshComponent::isVisible() const {
    return visible;
}

void MeshComponent::setRenderMode(RenderMode mode) {
    renderMode = mode;
}

MeshComponent::RenderMode MeshComponent::getRenderMode() const {
    return renderMode;
}

const char* MeshComponent::getTypeName() const {
    return "MeshComponent";
}

void MeshComponent::initialize() {
    // Register with the scene
    if (auto actor = getOwner()) {
        if (auto scene = actor->getScene()) {
            scene->onMeshComponentAdded(this);
        }
    }
}

void MeshComponent::render() {
    // Actual rendering happens in the renderer, not in the component
}

void MeshComponent::destroy() {
    // Unregister from the scene
    if (auto actor = getOwner()) {
        if (auto scene = actor->getScene()) {
            scene->onMeshComponentRemoved(this);
        }
    }
}

void MeshComponent::serialize(class Serializer& serializer) const {
    // TODO: Implement serialization
}

void MeshComponent::deserialize(class Deserializer& deserializer) {
    // TODO: Implement deserialization
}

} // namespace ohao 