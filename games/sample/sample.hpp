#pragma once
// The sample (step 3k): the smallest game, written against the game API only (game/api.hpp), to show what a game is
// and to keep the API from being the slice's shape. Targets spin until a player's round meets one; then it falls.
// docs/games/README.md walks through it.
#include "game/api.hpp"

#include <cstdint>

namespace ohao::sample {

using namespace ohao::ecs;

// A target: it spins (the engine's Spin) on a kinematic body until a player's round meets it; then it is down: its Spin
// goes and its body turns dynamic, so it falls. Saved: the sample keeps no state but its components, so it needs no
// snapshot hook and a reload carries nothing.
struct Target {
    bool down{false};
};

// sample.hit: a player's round met a standing target. Cue: at the point, normal the surface's, source the shooter,
// target the target (an audio bank's or a vfx set's `events` can bind it by name).
inline constexpr const char* kHitChannel = "sample.hit";
struct TargetHit {
    uint64_t shooter{0}, target{0};
    glm::vec3 point{0.0f}, normal{0.0f};
};

inline constexpr float kRange = 100.0f;          // m a round reaches
inline constexpr const char* kFire = "fire";     // the action that fires it (the scene's InputActions bind it)

[[nodiscard]] const game::Module& module();

} // namespace ohao::sample
