#pragma once
// The player (step 3f): the input-driven PlayerController on a stream with the lowest id. One rule, one place:
// the listener, the HUD, the effects, the enemies' target, the round and the grenade ask here.
#include <entt/entt.hpp>

#include <cstddef>

namespace ohao::ecs {

class World;

// The player on input stream `stream` (World::input), or entt::null.
[[nodiscard]] entt::entity playerOf(const World& world, std::size_t stream = 0);

} // namespace ohao::ecs
