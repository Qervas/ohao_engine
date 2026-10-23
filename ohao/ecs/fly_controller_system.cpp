#include "ecs/fly_controller_system.hpp"

#include "ecs/components.hpp"

#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace ohao::ecs {

void updateFlyControllers(entt::registry& reg, const InputMap& input, float dt) {
    if (!std::isfinite(dt)) return;
    const glm::vec2 mouse = input.mouseDelta();
    const auto view = reg.view<const FlyController, const Transform>();   // patch changes no membership
    for (const entt::entity e : view) {
        const FlyController& fc = view.get<const FlyController>(e);
        const bool turn = input.held(fc.look) && (mouse.x != 0.0f || mouse.y != 0.0f);
        const float forward = input.axis(fc.moveForward), right = input.axis(fc.moveRight),
                    up = input.axis(fc.moveUp);
        const bool move = dt != 0.0f && (forward != 0.0f || right != 0.0f || up != 0.0f);
        if (!turn && !move) continue;
        reg.patch<Transform>(e, [&](Transform& t) {
            if (turn) {
                const float rate = glm::radians(fc.lookDegPerUnit);
                const glm::quat yaw = glm::angleAxis(-mouse.x * rate, glm::vec3(0.0f, 1.0f, 0.0f));
                const glm::quat pitch = glm::angleAxis(-mouse.y * rate, glm::vec3(1.0f, 0.0f, 0.0f));
                t.rotation = glm::normalize(yaw * t.rotation * pitch);
            }
            if (move) {
                const float speed = fc.moveSpeed * (input.held(fc.fast) ? fc.fastMultiplier : 1.0f) * dt;
                const glm::vec3 front = t.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
                const glm::vec3 side = t.rotation * glm::vec3(1.0f, 0.0f, 0.0f);
                t.position += (front * forward + side * right + glm::vec3(0.0f, 1.0f, 0.0f) * up) * speed;
            }
        });
    }
}

} // namespace ohao::ecs
