#include "tidewall/walker.hpp"

#include <glm/glm.hpp>

namespace ohao::tidewall {

using namespace ohao::ecs;

void registerWalkerTypes(reflect::TypeRegistry& r) {
    r.type<Walker>("Walker").component().host(makeComponentOps<Walker>())
        .doc("A character (Tidewall) that walks to the player on the navmesh: the walkers system aims its NavAgent at "
             "stream 0's player's feet, again whenever the player has moved `retarget` from its goal.")
        .field("retarget", &Walker::retarget).doc("How far the player moves before the goal follows").unit("m").min(0.0).max(50.0);
}

void installWalkers(World& world) {
    world.addSimulate("walkers", [](SystemContext& ctx, float) {
        const entt::entity player = playerOf(ctx, 0);
        if (player == entt::null) return;
        const auto* t = ctx.try_get<Transform>(player);
        if (!t) return;
        const glm::vec3 feet = t->position;
        for (auto [e, w, agent] : ctx.view<const Walker, const NavAgent>().each()) {
            if (agent.active && glm::length(agent.goal - feet) < w.retarget) continue;
            ctx.patch<NavAgent>(e, [&](NavAgent& a) {
                a.goal = feet;
                a.active = true;
            });
        }
    }, {.before = {"navigation"}, .reads = types<Walker, PlayerController, Transform>(), .writes = types<NavAgent>()});
}

} // namespace ohao::tidewall
