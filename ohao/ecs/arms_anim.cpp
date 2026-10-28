#include "ecs/arms_anim.hpp"

#include "ecs/world.hpp"

#include <glm/glm.hpp>

namespace ohao::ecs {

void driveArmsAnim(uint64_t ownerId, const PlayerController* controller, const PlayerState* body, const Health* health,
                   const Weapon* weapon, const WeaponState* state, std::span<const CombatEvent> stepEvents,
                   AnimParams& out) {
    const bool alive = !(health && !(health->current > 0.0f));
    const float speed = body ? glm::length(glm::vec2(body->velocity.x, body->velocity.z)) : 0.0f;
    const bool moving = alive && body && body->onGround && speed > 0.5f;
    const float aim = alive && state ? state->aim : 0.0f;
    const float sprintAt = controller ? 0.5f * (controller->walkSpeed + controller->sprintSpeed) : 5.25f;
    out.set("moving", moving ? 1.0f : 0.0f);
    out.set("sprinting", moving && speed > sprintAt && aim == 0.0f ? 1.0f : 0.0f);
    out.set("aim", aim);
    out.set("alive", alive ? 1.0f : 0.0f);
    out.set("reloadTime", weapon ? weapon->reloadTime : 0.0f);

    std::size_t first = 0;   // after the step's last restart: what came before it was undone
    for (std::size_t i = 0; i < stepEvents.size(); ++i)
        if (stepEvents[i].kind == CombatEvent::Kind::Restart) first = i + 1;
    for (std::size_t i = first; i < stepEvents.size(); ++i) {
        const CombatEvent& c = stepEvents[i];
        if (c.sourceId != ownerId) continue;
        if (c.kind == CombatEvent::Kind::Shot) out.events.emplace_back("shot");
        else if (c.kind == CombatEvent::Kind::Reload) out.events.emplace_back("reload");
    }
}

void registerArmsDriver(AnimationSystem& animation) {
    animation.setDriver("arms", [](const World& world, entt::entity e, AnimParams& params) {
        const auto& reg = world.registry();
        // The owner: the nearest ancestor holding the weapon and the controller.
        entt::entity owner = entt::null;
        for (entt::entity at = e; owner == entt::null;) {
            const auto* p = reg.try_get<Parent>(at);
            if (!p || p->entity == entt::null || !reg.valid(p->entity)) break;
            at = p->entity;
            if (reg.all_of<PlayerController, Weapon>(at)) owner = at;
        }
        if (owner == entt::null) {
            driveArmsAnim(0, nullptr, nullptr, nullptr, nullptr, nullptr, {}, params);
            return;
        }
        driveArmsAnim(world.idOf(owner), reg.try_get<PlayerController>(owner), reg.try_get<PlayerState>(owner),
                      reg.try_get<Health>(owner), reg.try_get<Weapon>(owner), reg.try_get<WeaponState>(owner),
                      combatEvents(world), params);
    });
}

} // namespace ohao::ecs
