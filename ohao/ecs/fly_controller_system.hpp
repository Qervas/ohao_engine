#pragma once
#include "core/input.hpp"

#include <entt/entt.hpp>

namespace ohao::ecs {

// The fly_controller system (engine-provided; a simulate system, once per fixed step).
// Each entity with FlyController, from the world's input:
//   look held: rotation = normalize(angleAxis(-dx * rate, +Y) * rotation * angleAxis(-dy * rate, +X)),
//              dx, dy the step's mouse delta, rate = radians(lookDegPerUnit): moving the mouse
//              right turns right, down pitches down;
//   then position += (front * forward + right * right + worldUp * up) * moveSpeed * dt,
//              times fastMultiplier while `fast` is held; front = rotation * -Z,
//              right = rotation * +X (the turn of this step included).
// Written through patch<Transform>, and only when something moved: an idle controller
// leaves its entity untouched. Deterministic for a given input and dt sequence.
void updateFlyControllers(entt::registry& reg, const InputMap& input, float dt);

} // namespace ohao::ecs
