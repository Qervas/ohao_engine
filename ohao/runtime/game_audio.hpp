#pragma once
// How the window host's sound players place a sound (sub-project 9 spec, "Audio quick wins"): one
// AudioSystem for footsteps and combat, capped at kVoiceLimit SFX voices; a distance curve per kind
// of sound; and one occlusion ray, from the listener, for each sound an enemy starts (never per
// frame, never for the player's own). All of it is presentation in the window host: nothing here
// runs in a step, and headless hosts play nothing.
#include "audio/audio_system.hpp"
#include "ecs/world.hpp"

#include <cstdint>
#include <optional>

namespace ohao::runtime {

constexpr uint32_t kVoiceLimit = 24;

enum class SoundKind : uint8_t {
    Gunfire,   // rounds: inverse, 1 m to 60 m, rolloff 1
    Near,      // footsteps, landings, impacts and the rest: 0.5 m to 20 m, rolloff 1.5
};
[[nodiscard]] SoundShape shapeFor(SoundKind kind) noexcept;

// Who hears: the world's camera, and the input-driven player whose head it is (its body is skipped
// by the occlusion ray; its sounds are its own).
struct Listener {
    glm::vec3 position{0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    entt::entity body{entt::null};
    uint64_t playerId{0};
};
[[nodiscard]] std::optional<Listener> listenerOf(const ecs::World& world);

// Whether a wall stands between the listener and `source`: one ray from the listener toward it
// (PhysicsScene::raycastFrom, the listener's body skipped), passing through characters (their
// capsules and body boxes, the source's own included). False without physics.
[[nodiscard]] bool soundOccluded(const ecs::World& world, const Listener& listener, glm::vec3 source);

} // namespace ohao::runtime
