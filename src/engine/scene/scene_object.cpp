#include "scene_object.hpp"
#include "renderer/vulkan_context.hpp"
#include "engine/scene/scene_node.hpp"

namespace ohao {

// Initialize static ID counter
std::atomic<ObjectID> SceneObject::nextObjectID{1};

SceneObject::SceneObject(const std::string& name) : SceneNode(name) {
    // Assign a unique ID to each object at creation time
    objectID = nextObjectID.fetch_add(1);
}

std::shared_ptr<SceneObject> SceneObject::clone() const {
    auto cloned = std::make_shared<SceneObject>(name + "_clone");
    cloned->material = material;
    cloned->model = model;  // Share the model data
    cloned->transform = transform;  // Copy transform data
    // Note: objectID is already unique for the clone
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
