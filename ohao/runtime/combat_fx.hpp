#pragma once
// Particles from combat (sub-project 9 spec, "Particles from combat"): the window host turns the
// combat log's events into GPU particle emits, beside the sounds. A round fired is a muzzle burst at
// the shooter's muzzle along the round (the player's from its drawn muzzle, the light below its
// first-person gun; an enemy's from its eye); a round's hit is sparks along the hit normal and a
// puff by the surface hit (concrete, metal, wood, default). A window's presentation only: emitted
// only while the timeline runs, never in lockstep or headless, never in a lockstep capture
// (LiveConfig::effects; ohao_game --no-fx turns it off).
#include "ecs/combat.hpp"
#include "ecs/world.hpp"
#include "render/particles/particle_system.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ohao {
class DeferredRenderer;
}

namespace ohao::runtime {

// The emits one combat event makes. `playerMuzzle`: where the input-driven player's muzzle is drawn
// this frame, if it has one (its rounds burst there; anyone else's from the round's eye).
[[nodiscard]] std::vector<ParticleEmitterConfig> particlesFor(const ecs::CombatEvent& event, uint64_t playerId,
                                                              std::optional<glm::vec3> playerMuzzle);
// The puff a hit throws by surface ("concrete", "metal", "wood"; anything else: "default").
[[nodiscard]] ParticleEmitterConfig dustFor(const std::string& surface, glm::vec3 point, glm::vec3 normal);

class CombatFx {
public:
    // The emits for what the combat log holds since the last call (the first starts at its end);
    // none when not `realtime` (the cursor still moves: nothing from a lockstep stretch bursts later).
    [[nodiscard]] std::vector<ParticleEmitterConfig> collect(ecs::World& world, bool realtime);
    // collect, then the emits to the renderer. Call once per frame, after the frame's steps.
    void update(ecs::World& world, DeferredRenderer& renderer, bool realtime);
    [[nodiscard]] uint64_t emitted() const noexcept { return m_emitted; }

private:
    uint64_t m_cursor{0};
    bool m_started{false};
    uint64_t m_emitted{0};
};

} // namespace ohao::runtime
