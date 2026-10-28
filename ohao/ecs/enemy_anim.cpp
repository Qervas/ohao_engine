#include "ecs/enemy_anim.hpp"

#include "ecs/world.hpp"

#include <glm/glm.hpp>

#include <algorithm>

namespace ohao::ecs {

void driveEnemyAnim(const World& world, entt::entity e, const EnemyState* state, const PlayerState* body,
                    const MoveIntent* intent, std::span<const CombatEvent> stepEvents, AnimParams& out) {
    const auto& reg = world.registry();
    const auto* health = reg.try_get<Health>(e);
    const EnemyMode mode = state ? state->mode : EnemyMode::Idle;
    const bool alive = !(health && !(health->current > 0.0f)) && mode != EnemyMode::Dead;
    const float speed = alive && body ? glm::length(glm::vec2(body->velocity.x, body->velocity.z)) : 0.0f;
    const float pitch = alive && intent && mode == EnemyMode::Attack ? std::clamp(intent->pitchDeg, -60.0f, 60.0f) : 0.0f;
    out.set("speed", speed);
    out.set("aimPitch", pitch);
    out.set("mode", static_cast<float>(mode));
    out.set("alive", alive ? 1.0f : 0.0f);

    std::size_t first = 0;   // after the step's last restart: what came before it was undone
    for (std::size_t i = 0; i < stepEvents.size(); ++i)
        if (stepEvents[i].kind == CombatEvent::Kind::Restart) first = i + 1;
    const uint64_t id = world.idOf(e);
    for (std::size_t i = first; i < stepEvents.size(); ++i) {
        const CombatEvent& c = stepEvents[i];
        switch (c.kind) {
        case CombatEvent::Kind::Shot:
            if (c.sourceId == id) out.events.emplace_back("shot");
            break;
        case CombatEvent::Kind::Damage:
            if (c.targetId == id && alive) out.events.emplace_back("hit");
            break;
        case CombatEvent::Kind::Reload:
            if (c.sourceId == id) out.events.emplace_back("reload");
            break;
        case CombatEvent::Kind::Death:
            if (c.targetId == id) out.events.emplace_back("death");
            break;
        default:
            break;
        }
    }
}

void registerEnemyDriver(AnimationSystem& animation) {
    animation.setDriver("enemy", [](const World& world, entt::entity e, AnimParams& params) {
        const auto& reg = world.registry();
        driveEnemyAnim(world, e, reg.try_get<EnemyState>(e), reg.try_get<PlayerState>(e), reg.try_get<MoveIntent>(e),
                       combatEvents(world), params);
    });
}

} // namespace ohao::ecs
