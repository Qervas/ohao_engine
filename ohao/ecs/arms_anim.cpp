#include "ecs/arms_anim.hpp"

#include "ecs/world.hpp"

#include <glm/glm.hpp>

namespace ohao::ecs {

void driveArmsAnim(uint64_t ownerId, const PlayerController* controller, const PlayerState* body, const Health* health,
                   const Weapon* weapon, const WeaponState* state, const DriverEvents& events, AnimParams& out) {
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

    // The owner's rounds and reloads, in the order they happened.
    std::size_t s = 0, r = 0;
    while (s < events.shots.size() || r < events.reloads.size()) {
        const bool shot = r == events.reloads.size() ||
                          (s < events.shots.size() && events.shots[s].meta.seq < events.reloads[r].meta.seq);
        if (shot) {
            if (events.shots[s++].value.shooter == ownerId) out.events.emplace_back("shot");
        } else if (events.reloads[r++].value.holder == ownerId) {
            out.events.emplace_back("reload");
        }
    }
}

void registerArmsDriver(AnimationSystem& animation, EventLog& log) {
    animation.setDriver("arms", [readers = DriverReaders(log)](const World& world, entt::entity e, AnimParams& params) {
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
            driveArmsAnim(0, nullptr, nullptr, nullptr, nullptr, nullptr, DriverEvents{}, params);
            return;
        }
        driveArmsAnim(world.idOf(owner), reg.try_get<PlayerController>(owner), reg.try_get<PlayerState>(owner),
                      reg.try_get<Health>(owner), reg.try_get<Weapon>(owner), reg.try_get<WeaponState>(owner),
                      readers.read(), params);
    });
}

} // namespace ohao::ecs
