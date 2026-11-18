#pragma once
#include "ecs/system.hpp"

#include <entt/entt.hpp>
#include <cstddef>
#include <vector>

namespace ohao::ecs {

class World;

// Recomputes WorldTransform for every TransformDirty entity and its descendants,
// parents before children. Every recomputed entity gets RenderDirty, then the tag of each
// move rule (tagMoves) whose holders it has. Moved renderables get MovedLastFrame;
// the next update rewrites their prevWorld (= world) and tags RenderDirty once more,
// so motion vectors return to zero. Clears TransformDirty. The `transform` sync system:
// once per render, so prevWorld is the previous frame's world.
void updateTransforms(entt::registry& reg);

// The same recomputation inside a simulation step (the physics system needs current world
// transforms before it steps; sub-project 6 spec, "Transforms inside a step"), without the
// frame's bookkeeping: an entity's prevWorld is captured the first time it is recomputed
// after a render and kept by later propagations and by the next updateTransforms, so a frame
// of several steps still has motion vectors spanning all of them.
void propagateTransforms(entt::registry& reg);

// Debug check: the number of entities whose WorldTransform disagrees with their
// local Transform composed with the parent's world, while not tagged TransformDirty.
// Nonzero means someone wrote Transform through get<>() instead of patch<>().
[[nodiscard]] std::size_t verifyTransforms(const entt::registry& reg);

// A move rule: every entity the transform system recomputes (updateTransforms, propagateTransforms) that holds one of
// `Holders` gets `Tag`, so the module that owns them follows the move (physics: tagMoves<PhysicsMoved, PhysicsBody,
// ColliderPart>). The rules run in the order they were added, after RenderDirty; their reads and writes join the
// `transform` system's declaration (World::addAccess). Once per World and rule.
struct MoveRule {
    bool (*holds)(const entt::registry&, entt::entity);
    void (*tag)(entt::registry&, entt::entity);
};
void addMoveRule(World& world, MoveRule rule, std::vector<ComponentType> reads, std::vector<ComponentType> writes);

template <class Tag, class... Holders>
void tagMoves(World& world) {
    addMoveRule(world,
                {[](const entt::registry& r, entt::entity e) { return r.any_of<Holders...>(e); },
                 [](entt::registry& r, entt::entity e) { r.emplace_or_replace<Tag>(e); }},
                types<Holders...>(), types<Tag>());
}

} // namespace ohao::ecs
