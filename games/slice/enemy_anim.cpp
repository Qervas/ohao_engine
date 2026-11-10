#include "slice/enemy_anim.hpp"

#include "ecs/world.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace ohao::slice {

void driveEnemyAnim(const World& world, entt::entity e, const EnemyState* state, const PlayerState* body,
                    const MoveIntent* intent, const DriverEvents& events, AnimParams& out) {
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

    // Its rounds, the damage it took and lived, its reloads and its death, in the order they happened.
    const uint64_t id = world.idOf(e);
    thread_local std::vector<std::pair<uint64_t, const char*>> mine;
    mine.clear();
    for (const auto& [meta, v] : events.shots)
        if (v.shooter == id) mine.emplace_back(meta.seq, "shot");
    for (const auto& [meta, v] : events.damage)
        if (v.target == id && alive) mine.emplace_back(meta.seq, "hit");
    for (const auto& [meta, v] : events.reloads)
        if (v.holder == id) mine.emplace_back(meta.seq, "reload");
    for (const auto& [meta, v] : events.deaths)
        if (v.target == id) mine.emplace_back(meta.seq, "death");
    std::sort(mine.begin(), mine.end());
    for (const auto& [seq, name] : mine) out.events.emplace_back(name);
}

void registerEnemyDriver(AnimationSystem& animation, EventLog& log) {
    animation.setDriver("enemy", [readers = DriverReaders(log)](const World& world, entt::entity e, AnimParams& params) {
        const auto& reg = world.registry();
        driveEnemyAnim(world, e, reg.try_get<EnemyState>(e), reg.try_get<PlayerState>(e), reg.try_get<MoveIntent>(e),
                       readers.read(), params);
    });
}

} // namespace ohao::slice
