#pragma once
// The player's arms' animation driver (sub-project 10 step 3, docs/specs/2026-10-28-animation-design.md,
// "The step"): the arms graph's parameters (assets/anim/arms.animgraph.json) from the saved state of the
// arms' owner, the nearest ancestor with a PlayerController and a Weapon (the arms sit below the view
// model's root, below the head), so a restored world drives the same states.
//   moving      on the ground, walking faster than 0.5 m/s
//   sprinting   moving faster than halfway from walkSpeed to sprintSpeed, and not aiming
//   aim         WeaponState.aim (0 from the hip .. 1 down the sights)
//   alive       Health above 0
//   reloadTime  Weapon.reloadTime (the reload clip's rateFrom: its mag_in lands on the weapon's reloaded)
// and the events `shot` (the owner's round) and `reload`, from the step's slice events, in the order they
// happened.
#include "ecs/animation.hpp"
#include "ecs/combat.hpp"
#include "ecs/components.hpp"

#include <entt/entt.hpp>

#include <cstdint>
#include <span>

namespace ohao::ecs {

// Any of the owner's state may be missing (before the first combat step there is no WeaponState): it
// reads as at rest.
void driveArmsAnim(uint64_t ownerId, const PlayerController* controller, const PlayerState* body, const Health* health,
                   const Weapon* weapon, const WeaponState* state, const DriverEvents& events, AnimParams& out);

// The driver `arms` (installSlice registers it), reading the slice's channels of `log`.
void registerArmsDriver(AnimationSystem& animation, EventLog& log);

} // namespace ohao::ecs
