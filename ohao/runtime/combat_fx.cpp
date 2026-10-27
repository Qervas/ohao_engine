#include "runtime/combat_fx.hpp"

#include "ecs/components.hpp"
#include "render/deferred/deferred_renderer.hpp"

#include <utility>

namespace ohao::runtime {

namespace {

// The player's muzzle as this frame draws it: the Muzzle below the input-driven player, at its
// RenderPose (the first-person gun's drawn pose) or else its step pose.
std::optional<glm::vec3> drawnMuzzle(const ecs::World& world, entt::entity player) {
    const auto& reg = world.registry();
    for (auto [e, m, wt] : reg.view<const ecs::Muzzle, const ecs::WorldTransform>().each()) {
        entt::entity up = e;
        for (int depth = 0; depth < 64 && up != entt::null && reg.valid(up); ++depth) {
            if (up == player) {
                const auto* rp = reg.try_get<ecs::RenderPose>(e);
                return glm::vec3((rp ? rp->world : wt.world)[3]);
            }
            const auto* p = reg.try_get<ecs::Parent>(up);
            up = p ? p->entity : entt::null;
        }
    }
    return std::nullopt;
}

} // namespace

ParticleEmitterConfig dustFor(const std::string& surface, glm::vec3 point, glm::vec3 normal) {
    // From the smoke preset: a short puff along the normal, coloured and sized by the material.
    ParticleEmitterConfig c = ParticleSystem::presetSmoke(point + normal * 0.02f);
    c.direction = normal;
    c.spreadAngle = 0.7f;
    c.minSpeed = 0.4f;
    c.maxSpeed = 1.6f;
    c.minLifetime = 0.4f;
    c.maxLifetime = 1.2f;
    c.startSize = 0.05f;
    c.endSize = 0.25f;
    c.gravity = 0.5f;
    c.drag = 3.0f;
    c.emitCount = 10;
    if (surface == "concrete") {
        c.colorStart = {0.62f, 0.6f, 0.56f, 0.55f};
        c.colorEnd = {0.5f, 0.48f, 0.45f, 0.0f};
    } else if (surface == "wood") {
        c.colorStart = {0.45f, 0.32f, 0.2f, 0.6f};
        c.colorEnd = {0.35f, 0.25f, 0.15f, 0.0f};
        c.gravity = 3.0f;   // chips: heavier, they fall
        c.startSize = 0.03f;
        c.endSize = 0.08f;
    } else if (surface == "metal") {
        // A brighter, longer spray of sparks and only a wisp of dust.
        c = ParticleSystem::presetImpactSpark(point, normal);
        c.emitCount = 14;
        c.minSpeed = 4.0f;
        c.maxSpeed = 11.0f;
        c.colorStart = {1.0f, 0.95f, 0.75f, 1.0f};
    } else {
        c.colorStart = {0.7f, 0.7f, 0.7f, 0.45f};
        c.colorEnd = {0.6f, 0.6f, 0.6f, 0.0f};
    }
    return c;
}

std::vector<ParticleEmitterConfig> particlesFor(const ecs::CombatEvent& e, uint64_t playerId,
                                                std::optional<glm::vec3> playerMuzzle) {
    using K = ecs::CombatEvent::Kind;
    switch (e.kind) {
    case K::Shot: {
        const bool own = playerId != 0 && e.sourceId == playerId && playerMuzzle;
        return {ParticleSystem::presetMuzzleFlash(own ? *playerMuzzle : e.point, e.normal)};
    }
    case K::Hit: {
        const std::string surface = e.surface.empty() ? std::string("default") : e.surface;
        return {ParticleSystem::presetImpactSpark(e.point, e.normal), dustFor(surface, e.point, e.normal)};
    }
    default: return {};
    }
}

std::vector<ParticleEmitterConfig> CombatFx::collect(ecs::World& world, bool realtime) {
    std::vector<ParticleEmitterConfig> out;
    const ecs::CombatSystem* combat = ecs::combatSystem(world);
    if (!combat) return out;
    if (!std::exchange(m_started, true)) m_cursor = combat->nextEventSeq();   // nothing from before
    uint64_t playerId = 0;
    entt::entity player = entt::null;
    for (auto [e, pc] : world.registry().view<const ecs::PlayerController>().each()) {
        const uint64_t id = world.idOf(e);
        if (pc.input && (playerId == 0 || id < playerId)) playerId = id, player = e;
    }
    const std::optional<glm::vec3> muzzle = player != entt::null ? drawnMuzzle(world, player) : std::nullopt;
    for (;;) {
        uint64_t dropped = 0;
        const auto events = combat->eventsSince(m_cursor, 64, dropped);
        if (events.empty()) break;
        for (const ecs::CombatEvent& e : events) {
            m_cursor = e.seq + 1;
            if (!realtime) continue;
            for (ParticleEmitterConfig& c : particlesFor(e, playerId, muzzle)) out.push_back(std::move(c));
        }
    }
    return out;
}

void CombatFx::update(ecs::World& world, DeferredRenderer& renderer, bool realtime) {
    for (const ParticleEmitterConfig& c : collect(world, realtime)) {
        renderer.emitParticles(c);
        ++m_emitted;
    }
}

} // namespace ohao::runtime
