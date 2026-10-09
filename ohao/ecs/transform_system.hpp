#pragma once
#include <entt/entt.hpp>
#include <cstddef>

namespace ohao::ecs {

// Recomputes WorldTransform for every TransformDirty entity and its descendants,
// parents before children. Moved renderables get RenderDirty and MovedLastFrame;
// the next update rewrites their prevWorld (= world) and tags RenderDirty once more,
// so motion vectors return to zero. Clears TransformDirty.
void updateTransforms(entt::registry& reg);

// Debug check: the number of entities whose WorldTransform disagrees with their
// local Transform composed with the parent's world, while not tagged TransformDirty.
// Nonzero means someone wrote Transform through get<>() instead of patch<>().
[[nodiscard]] std::size_t verifyTransforms(const entt::registry& reg);

} // namespace ohao::ecs
