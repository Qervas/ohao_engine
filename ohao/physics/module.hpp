#pragma once
// The physics module, "physics": the physics scene (physics/physics_scene.hpp) and its systems (the `physics` sync and
// simulate systems, `player_spawn`, `interpolate`), the players (physics/player.hpp), their components
// (physics/components.hpp), events (physics/events.hpp) and API methods (physics.raycast, physics.info,
// physics.contacts, player.info, player.teleport). Its backend (Jolt) is its own (physics/private/backend).
#include "ecs/module.hpp"

namespace ohao::ecs {

[[nodiscard]] const Module& physicsModule();
// Its registrations (physics/types.cpp) and methods (physics/methods.cpp).
void registerPhysicsTypes(reflect::TypeRegistry& types);
[[nodiscard]] std::span<const Method> physicsMethods();

} // namespace ohao::ecs
