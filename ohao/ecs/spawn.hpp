#pragma once
#include "ecs/world.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string_view>

namespace ohao::ecs {

// An entity with a Light. Position and direction (-Z) come from the transform.
entt::entity spawnLight(World& world, std::string_view name, const Light& light, const Transform& transform);

// Rotation taking -Z onto `direction` (need not be normalized). Up is +Y; for a
// vertical direction the right vector stays +X.
[[nodiscard]] glm::quat lookRotation(glm::vec3 direction);

} // namespace ohao::ecs
