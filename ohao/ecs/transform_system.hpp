#pragma once
#include <entt/entt.hpp>
#include <cstddef>

namespace ohao::ecs {

// Recomputes WorldTransform for every TransformDirty entity and its descendants,
// parents before children. Moved renderables get RenderDirty and MovedLastFrame;
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

} // namespace ohao::ecs
