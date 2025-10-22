#include "engine/scene/scene_object.hpp"
#include "renderer/vulkan_context.hpp"
#include <atomic>

namespace ohao {

std::atomic<ObjectID> SceneObject::nextObjectID{1};

SceneObject::SceneObject(const std::string& name) : name(name) {
    objectID = nextObjectID.fetch_add(1);
}

} // namespace ohao
