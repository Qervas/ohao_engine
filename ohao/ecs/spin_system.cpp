#include "ecs/spin_system.hpp"

#include "ecs/components.hpp"

#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace ohao::ecs {

void updateSpin(entt::registry& reg, float dt) {
    if (dt == 0.0f || !std::isfinite(dt)) return;
    const auto view = reg.view<const Spin, const Transform>();   // patch changes no membership
    for (const entt::entity e : view) {
        const Spin& spin = view.get<const Spin>(e);
        const float angle = glm::radians(spin.degreesPerSecond * dt);
        const float len = glm::length(spin.axis);
        if (angle == 0.0f || !std::isfinite(angle) || !(len > 0.0f) || !std::isfinite(len)) continue;
        const glm::quat turn = glm::angleAxis(angle, spin.axis / len);
        // Post-multiplied: the axis is in the entity's own (already rotated) frame.
        reg.patch<Transform>(e, [&](Transform& t) { t.rotation = glm::normalize(t.rotation * turn); });
    }
}

} // namespace ohao::ecs
