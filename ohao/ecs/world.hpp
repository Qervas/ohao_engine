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

    // Creates an entity with Name, Transform (tagged TransformDirty), an empty Children
    // and its CreationOrder.
    entt::entity create(std::string_view name, const Transform& local = {});
    // Destroys the entity and its whole subtree, detaching it from its parent first.
    // This is the preferred path. registry().destroy(e) is also kept consistent, with
    // different semantics: e's parent forgets it, and e's children are DETACHED (they
    // become roots and keep their local transforms) rather than destroyed.
    void destroy(entt::entity e);
    // parent == entt::null detaches. The local transform is kept (world changes).
    // Returns false and changes nothing if child is invalid or parent == child or a
    // descendant of child (a cycle).
    bool setParent(entt::entity child, entt::entity parent);
    void setTransform(entt::entity e, const Transform& local);

    void addSystem(std::string name, System fn);
    // Inserts right after the system named `after` (appends if there is none).
    void addSystemAfter(std::string_view after, std::string name, System fn);
    void update(float dt);
    [[nodiscard]] std::vector<std::string> systemNames() const;
    // Debug builds: before running systems, update() checks every untagged entity's
    // WorldTransform against its local Transform (verifyTransforms) and logs the first
    // nonzero result once per World. This is that result (0: none seen; always 0 in
    // Release, where the check is compiled out).
    [[nodiscard]] std::size_t transformMismatches() const noexcept { return m_transformMismatches; }

private:
    void onTransformChanged(entt::registry& r, entt::entity e);
    void onParentDestroyed(entt::registry& r, entt::entity child);
    void onChildrenDestroyed(entt::registry& r, entt::entity parent);

    entt::registry m_registry;
    std::vector<std::pair<std::string, System>> m_systems;
    uint64_t m_nextSeq{0};
    std::size_t m_transformMismatches{0};
};

} // namespace ohao::ecs
