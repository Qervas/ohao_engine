#pragma once
#include <entt/entt.hpp>

namespace ohao::ecs {

// The spin system (engine-provided; every World runs it, right before the transform
// system). Each entity with Spin turns about its normalized local axis by
// degreesPerSecond * dt:
//   rotation = normalize(rotation * angleAxis(radians(degreesPerSecond * dt), axis))
// written through patch<Transform>, so the transform system and the render extract pick
// it up the same update. An entity with dt, rate or axis zero (or not finite) is not
// touched: no TransformDirty, nothing re-uploaded. Deterministic for a given dt sequence.
void updateSpin(entt::registry& reg, float dt);

} // namespace ohao::ecs
