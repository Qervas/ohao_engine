#pragma once
// Tidewall's walkers (docs/plans/2026-11-18-tidewall.md, part N1): characters that walk to the player on the navmesh,
// the first thing the navigation module moves (no AI yet: the `walkers` system sets each one's NavAgent goal to the
// player's feet; navigation plans, follows and steers).
#include "game/api.hpp"

namespace ohao::tidewall {

// A character that walks to stream 0's player: its NavAgent's goal follows the player, re-aimed when the player has
// moved `retarget` metres from it. Saved.
struct Walker {
    float retarget{0.5f};   // m
};

void registerWalkerTypes(reflect::TypeRegistry& r);
// The `walkers` simulate system, before `navigation`.
void installWalkers(ecs::World& world);

} // namespace ohao::tidewall
