#pragma once
// The enemy's animation driver (sub-project 10, docs/specs/2026-10-28-animation-design.md, "The
// step"): the enemy graph's parameters (assets/anim/enemy.animgraph.json) from saved state and the
// step's combat events, so a restored world drives the same states.
//   speed     horizontal PlayerState.velocity (0 once dead)
//   aimPitch  MoveIntent.pitchDeg while attacking, within +-60 (0 otherwise)
//   mode      EnemyState.mode (Idle 0, Chase 1, Attack 2, Dead 3)
//   alive     Health above 0
// and the events `shot` (its round), `hit` (damage it took and lived), `reload`, `death`, from the
// step's slice events, in the order they happened.
#include "ecs/animation.hpp"
#include "slice/combat.hpp"
#include "slice/components.hpp"

#include <entt/entt.hpp>

#include <span>

namespace ohao::slice {

// Any of the state may be missing (a dead enemy has lost its character): it reads as at rest.
void driveEnemyAnim(const World& world, entt::entity e, const EnemyState* state, const PlayerState* body,
                    const MoveIntent* intent, const DriverEvents& events, AnimParams& out);

// The driver `enemy` (installSlice registers it), reading the slice's channels of `log`.
void registerEnemyDriver(AnimationSystem& animation, EventLog& log);

} // namespace ohao::slice
