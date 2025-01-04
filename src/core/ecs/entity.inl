#pragma once
#include "entity.hpp"

namespace ohao {

template<typename T, typename... Args>
std::shared_ptr<T> Entity::addComponent(Args&&... args) {
    static_assert(std::is_base_of<Component, T>::value,
                  "T must inherit from Component");

    auto typeIndex = std::type_index(typeid(T));
    if (components.find(typeIndex) != components.end()) {
        return std::dynamic_pointer_cast<T>(components[typeIndex]);
    }

    auto component = std::make_shared<T>(std::forward<Args>(args)...);
    component->setOwner(this);
    components[typeIndex] = component;
    component->onAttach();

    return component;
}

template<typename T>
std::shared_ptr<T> Entity::getComponent() {
    auto typeIndex = std::type_index(typeid(T));
    auto it = components.find(typeIndex);
    if (it != components.end()) {
        return std::dynamic_pointer_cast<T>(it->second);
    }
    return nullptr;
}

template<typename T>
void Entity::removeComponent() {
    auto typeIndex = std::type_index(typeid(T));
    auto it = components.find(typeIndex);
    if (it != components.end()) {
        it->second->onDetach();
        components.erase(it);
    }
}

} // namespace ohao
