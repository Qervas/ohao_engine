#include "component.hpp"
#include "../actor/actor.hpp"
#include "../scene/scene.hpp"

namespace ohao {

uint64_t Component::nextComponentID = 1;

Component::Component(Actor* owner)
    : owner(owner)
    , enabled(true)
    , componentID(nextComponentID++)
    , modified(false)
{
}

void Component::setEnabled(bool isEnabled) {
    enabled = isEnabled;
}

bool Component::isEnabled() const {
    return enabled;
}

const char* Component::getTypeName() const {
    return "Component";
}

std::type_index Component::getTypeIndex() const {
    return std::type_index(typeid(*this));
}

Scene* Component::getScene() const {
    return owner ? owner->getScene() : nullptr;
}

void Component::beginModification() {
    if (!modified) {
        oldState = serialize();
        modified = true;
    }
}

void Component::endModification() {
    if (modified) {
        auto scene = getScene();
        if (scene) {
            scene->trackComponentModified(this, oldState, serialize());
        }
        modified = false;
    }
}

} // namespace ohao 