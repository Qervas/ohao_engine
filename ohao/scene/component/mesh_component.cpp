#include "mesh_component.hpp"

#include "scene/actor/actor.hpp"
#include "scene/scene.hpp"

namespace ohao {

MeshComponent::MeshComponent() = default;

MeshComponent::~MeshComponent() {
    destroy();
}

void MeshComponent::setModel(std::shared_ptr<Model> newModel) {
    if (model == newModel) return;

    model = std::move(newModel);

    buffer.vertexOffset = 0;
    buffer.indexOffset = 0;
    buffer.indexCount = model ? static_cast<std::uint32_t>(model->indices.size()) : 0;
    buffer.vertexCount = model ? static_cast<std::uint32_t>(model->vertices.size()) : 0;

    if (auto* actor = getOwner()) {
        if (auto* scene = actor->getScene()) {
            scene->onMeshComponentChanged(this);
        }
    }
}

std::uint32_t MeshComponent::getVertexCount() const {
    return model ? static_cast<std::uint32_t>(model->vertices.size()) : 0;
}

std::uint32_t MeshComponent::getIndexCount() const {
    return model ? static_cast<std::uint32_t>(model->indices.size()) : 0;
}

void MeshComponent::setBufferOffsets(std::uint32_t vOffset, std::uint32_t iOffset, std::uint32_t iCount) {
    buffer.vertexOffset = vOffset;
    buffer.indexOffset = iOffset;
    buffer.indexCount = iCount;
    if (model) {
        buffer.vertexCount = static_cast<std::uint32_t>(model->vertices.size());
    }
}

void MeshComponent::setBufferInfo(const MeshBufferInfo& info) noexcept {
    buffer = info;
}

MeshBufferInfo MeshComponent::getBufferInfo() const noexcept {
    return buffer;
}

const char* MeshComponent::getTypeName() const {
    return "MeshComponent";
}

void MeshComponent::initialize() {
    if (auto* actor = getOwner()) {
        if (auto* scene = actor->getScene()) {
            scene->onMeshComponentAdded(this);
        }
    }
}

void MeshComponent::render() {
    // Rendering is driven by VulkanRenderer / deferred path, not the component.
}

void MeshComponent::destroy() {
    if (auto* actor = getOwner()) {
        if (auto* scene = actor->getScene()) {
            scene->onMeshComponentRemoved(this);
        }
    }
}

} // namespace ohao
