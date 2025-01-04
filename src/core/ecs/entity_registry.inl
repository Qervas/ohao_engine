#pragma once

namespace ohao {

template<typename T>
std::vector<std::shared_ptr<T>> EntityRegistry::getComponents() {
    std::vector<std::shared_ptr<T>> result;
    for (const auto& [id, entity] : entities) {
        if (auto component = entity->getComponent<T>()) {
            result.push_back(component);
        }
    }
    return result;
}

template<typename T>
std::vector<Entity::Ptr> EntityRegistry::getEntitiesWithComponent() {
    std::vector<Entity::Ptr> result;
    for (const auto& [id, entity] : entities) {
        if (entity->getComponent<T>()) {
            result.push_back(entity);
        }
    }
    return result;
}

} // namespace ohao
