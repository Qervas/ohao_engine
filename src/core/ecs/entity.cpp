#include "ecs/entity.hpp"
#include <algorithm>

namespace ohao {

Entity::Entity(EntityID id, const std::string& name)
    : id(id), name(name) {}

Entity::~Entity() {
    // Detach all components
    for (auto& [type, component] : components) {
        component->onDetach();
    }
    components.clear();
}

void Entity::setParent(Ptr newParent) {
    // Remove from old parent
    if (auto oldParent = parent.lock()) {
        oldParent->removeChild(shared_from_this());
    }

    // Set new parent
    parent = newParent;

    // Add to new parent's children
    if (newParent) {
        newParent->addChild(shared_from_this());
    }

    // Update transform
    transform.setDirty();
}

void Entity::addChild(Ptr child) {
    if (!child || child.get() == this) return;

    // Remove from old parent
    if (auto oldParent = child->getParent()) {
        oldParent->removeChild(child);
    }

    // Add to children
    children.push_back(child);
    child->parent = shared_from_this();

    // Update transform
    child->transform.setDirty();
}

void Entity::removeChild(Ptr child) {
    if (!child) return;

    auto it = std::find(children.begin(), children.end(), child);
    if (it != children.end()) {
        (*it)->parent.reset();
        children.erase(it);

        // Update transform
        child->transform.setDirty();
    }
}

template std::shared_ptr<MeshComponent> Entity::addComponent();
template std::shared_ptr<LightComponent> Entity::addComponent();
template std::shared_ptr<MeshComponent> Entity::getComponent();
template std::shared_ptr<LightComponent> Entity::getComponent();

} // namespace ohao
