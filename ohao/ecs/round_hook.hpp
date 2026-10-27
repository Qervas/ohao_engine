#pragma once
// How the combat system keeps a round's start and goes back to it (sub-project 7 step 3;
// docs/specs/2026-10-27-combat-design.md, "Death, restart, win"): a whole-world snapshot, which
// needs the reflection layer that ECS system sources may not include. A host that has the type
// registry puts a RoundHook in the registry's context (ecs::installRoundHook, ecs/snapshot.hpp;
// api::Session does it); without one a world never starts a round.
#include <functional>
#include <memory>

namespace ohao::ecs {

struct RoundHook {
    std::function<std::shared_ptr<const void>()> save;                  // the world as it is now
    std::function<void(const std::shared_ptr<const void>&)> restore;    // back to a save
};

} // namespace ohao::ecs
