#pragma once
#include "ecs/components.hpp"

#include <entt/entt.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ohao::ecs {

// The game world: an EnTT registry plus an explicitly ordered list of systems.
// Systems run in exactly the order they were added -- no discovery, no sorting.
class World {
public:
    using System = std::function<void(World&, float dt)>;

    World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    [[nodiscard]] entt::registry& registry() noexcept { return m_registry; }
    [[nodiscard]] const entt::registry& registry() const noexcept { return m_registry; }

    // Creates an entity with Name, Transform (tagged TransformDirty) and an empty Children.
    entt::entity create(std::string_view name, const Transform& local = {});
    // Destroys the entity and its whole subtree, detaching it from its parent first.
    void destroy(entt::entity e);
    // parent == entt::null detaches. The local transform is kept (world changes).
    void setParent(entt::entity child, entt::entity parent);
    void setTransform(entt::entity e, const Transform& local);

    void addSystem(std::string name, System fn);
    void update(float dt);
    [[nodiscard]] std::vector<std::string> systemNames() const;

private:
    void onTransformChanged(entt::registry& r, entt::entity e);

    entt::registry m_registry;
    std::vector<std::pair<std::string, System>> m_systems;
};

} // namespace ohao::ecs
