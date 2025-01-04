#pragma once
#include "entity.hpp"
#include <unordered_map>
#include <queue>

namespace ohao {

class EntityRegistry {
public:
    EntityRegistry() = default;
    ~EntityRegistry() = default;

    // Entity creation/destruction
    Entity::Ptr createEntity(const std::string& name = "Entity");
    void destroyEntity(EntityID id);
    void destroyEntity(Entity::Ptr entity);

    // Entity lookup
    Entity::Ptr getEntity(EntityID id);
    std::vector<Entity::Ptr> getEntitiesByName(const std::string& name);

    // Component queries
    template<typename T>
    std::vector<std::shared_ptr<T>> getComponents();

    template<typename T>
    std::vector<Entity::Ptr> getEntitiesWithComponent();

    // System updates
    void update(float dt);
    void clear();

private:
    std::unordered_map<EntityID, Entity::Ptr> entities;
    std::queue<EntityID> recycledIDs;
    EntityID nextEntityID{0};

    EntityID generateEntityID();
    void recycleEntityID(EntityID id);
};

} // namespace ohao
#include "entity_registry.inl"
