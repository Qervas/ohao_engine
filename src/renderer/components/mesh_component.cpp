#include "mesh_component.hpp"
#include "engine/actor/actor.hpp"
#include "engine/component/transform_component.hpp"
#include "physics/components/physics_component.hpp"
#include "engine/scene/scene.hpp"

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
    
    // UNIFIED MESH-TO-PHYSICS PIPELINE
    // Automatically sync with any PhysicsComponent on the same actor
    if (auto actor = getOwner()) {
        // DISABLED: Auto-sync was overwriting primitive collision shapes (Box, Sphere, etc.)
        // with triangle meshes, breaking GJK collision detection.
        // Collision shapes should be set explicitly via ComponentFactory::setupPhysicsShape()
        //
        // auto physicsComponent = actor->getComponent<PhysicsComponent>();
        // if (physicsComponent && model) {
        //     physicsComponent->createCollisionShapeFromModel(*model);
        // }

        // Register with scene if part of one
        if (auto scene = actor->getScene()) {
            scene->onMeshComponentChanged(this);
        }
    }
}

std::shared_ptr<Model> MeshComponent::getModel() const {
    return model;
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

uint32_t MeshComponent::getVertexCount() const {
    return model ? static_cast<uint32_t>(model->vertices.size()) : 0;
}

uint32_t MeshComponent::getIndexCount() const {
    return model ? static_cast<uint32_t>(model->indices.size()) : 0;
}

void MeshComponent::setBufferOffsets(uint32_t vOffset, uint32_t iOffset, uint32_t iCount) {
    vertexOffset = vOffset;
    indexOffset = iOffset;
    indexCount = iCount;
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

// Registration moved to central file

} // namespace ohao 