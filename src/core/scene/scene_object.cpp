#include "scene_object.hpp"
#include "renderer/vulkan_context.hpp"
#include "scene/scene_node.hpp"

namespace ohao {

SceneObject::SceneObject(const std::string& name) : SceneNode(name) {
}

std::shared_ptr<SceneObject> SceneObject::clone() const {
    auto cloned = std::make_shared<SceneObject>(name + "_clone");
    cloned->material = material;
    cloned->model = model;  // Share the model data
    cloned->transform = transform;  // Copy transform data
    return cloned;
}

void SceneObject::onAddedToScene() {
    // Override if needed
}

void SceneObject::onRemovedFromScene() {
    // Override if needed
}

void SceneObject::setTransform(const Transform& transform) {
    SceneNode::setTransform(transform);
    markTransformDirty();
}

void SceneObject::markTransformDirty() {
    SceneNode::markTransformDirty();
    // Notify VulkanContext that scene needs update
    if (auto context = VulkanContext::getContextInstance()) {
        context->markSceneModified();
    }
}

} // namespace ohao
