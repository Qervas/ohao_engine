#include "component.hpp"
#include "../actor/actor.hpp"

namespace ohao {

uint64_t Component::nextComponentID = 1;

Component::Component() 
    : owner(nullptr), enabled(true), componentID(nextComponentID++) 
{
}

void Component::setOwner(Actor* newOwner) {
    if (owner == newOwner) return;
    
    // If we had a previous owner, handle detachment
    if (owner && !newOwner) {
        onDetached();
    }
    
    owner = newOwner;
    
    // If we have a new owner, handle attachment
    if (owner) {
        onAttached();
    }
}

Actor* Component::getOwner() const {
    return owner;
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

} // namespace ohao 