#include "runtime/game_audio.hpp"

#include "ecs/components.hpp"
#include "ecs/physics_scene.hpp"
#include "ecs/scene_view.hpp"

#include <cmath>

namespace ohao::runtime {

namespace {

// The character a body belongs to (the entity or an ancestor: a living character holds a
// PlayerController, a dead enemy keeps Enemy and its body boxes), or null.
entt::entity characterOf(const entt::registry& reg, entt::entity e) {
    for (int depth = 0; depth < 64 && e != entt::null && reg.valid(e); ++depth) {
        if (reg.all_of<ecs::PlayerController>(e) || reg.all_of<ecs::Enemy>(e)) return e;
        const auto* p = reg.try_get<ecs::Parent>(e);
        e = p ? p->entity : entt::null;
    }
    return entt::null;
}

} // namespace

SoundShape shapeFor(SoundKind kind) noexcept {
    switch (kind) {
    case SoundKind::Gunfire: return {1.0f, 60.0f, 1.0f, false};
    case SoundKind::Near: break;
    }
    return {0.5f, 20.0f, 1.5f, false};
}

std::optional<Listener> listenerOf(const ecs::World& world) {
    const auto view = ecs::resolveSceneView(world).camera;
    if (!view) return std::nullopt;
    Listener l;
    const float p = glm::radians(view->pitchDeg), y = glm::radians(view->yawDeg);
    l.position = view->position;
    l.forward = glm::vec3(std::cos(y) * std::cos(p), std::sin(p), std::sin(y) * std::cos(p));
    for (auto [e, pc] : world.registry().view<const ecs::PlayerController>().each()) {
        const uint64_t id = world.idOf(e);
        if (pc.input && (l.playerId == 0 || id < l.playerId)) l.playerId = id, l.body = e;
    }
    return l;
}

bool soundOccluded(const ecs::World& world, const Listener& listener, glm::vec3 source) {
    const ecs::PhysicsScene* physics = ecs::physicsScene(world);
    if (!physics) return false;
    const glm::vec3 toSource = source - listener.position;
    float remaining = glm::length(toSource);
    if (remaining < 1e-3f) return false;
    const glm::vec3 dir = toSource / remaining;
    glm::vec3 from = listener.position;
    entt::entity ignore = listener.body;
    // A character in the way (the source's own capsule at the end of the ray, another enemy, a
    // body box) is passed through: the ray goes on from where it met it, that character skipped.
    for (int hop = 0; hop < 4 && remaining > 0.0f; ++hop) {
        const auto hit = physics->raycastFrom(from, dir, remaining, ignore);
        if (!hit) return false;
        const entt::entity character = characterOf(world.registry(), hit->entity);
        if (character == entt::null) return true;
        constexpr float kStepPast = 0.01f;
        from = hit->point + dir * kStepPast;
        remaining -= hit->dist + kStepPast;
        ignore = character;
    }
    return false;
}

} // namespace ohao::runtime
