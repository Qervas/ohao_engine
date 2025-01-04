#pragma once
#include "ecs_types.hpp"
#include "scene/transform.hpp"
#include "ecs/component.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <typeindex>
#include <vector>


namespace ohao {


class Entity : public std::enable_shared_from_this<Entity> {
public:
    using Ptr = std::shared_ptr<Entity>;

    Entity(EntityID id, const std::string& name = "Entity");
    ~Entity();

    // Component management
    template<typename T, typename... Args>
    std::shared_ptr<T> addComponent(Args&&... args);

    template<typename T>
    std::shared_ptr<T> getComponent();

    template<typename T>
    void removeComponent();

    // Hierarchy
    void setParent(Ptr parent);
    void addChild(Ptr child);
    void removeChild(Ptr child);

    // Transform
    Transform& getTransform() { return transform; }
    const Transform& getTransform() const { return transform; }

    // Getters
    EntityID getID() const { return id; }
    const std::string& getName() const { return name; }
    Ptr getParent() const { return parent.lock(); }
    const std::vector<Ptr>& getChildren() const { return children; }

    // Component access
    const std::unordered_map<std::type_index, std::shared_ptr<Component>>&
    getComponents() const { return components; }

private:
    EntityID id;
    std::string name;
    Transform transform;
    std::weak_ptr<Entity> parent;
    std::vector<Ptr> children;
    std::unordered_map<std::type_index, std::shared_ptr<Component>> components;
};

} // namespace ohao

#include "components/mesh_component.hpp"
#include "components/light_component.hpp"

#include "entity.inl"
