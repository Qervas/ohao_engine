#pragma once
// How a system keeps the whole world as it is and goes back to it later (a game's round start and its restart): a
// whole-world snapshot needs the reflection layer, which ECS system sources may not include, so a host that has the
// type registry puts a WorldSnapshots in the registry's context (ecs::installWorldSnapshots, ecs/snapshot.hpp;
// api::Session does it). Without one a world cannot keep a moment. Both are called through World::defer: between
// steps, never inside one.
#include <functional>
#include <memory>

namespace ohao::ecs {

struct WorldSnapshots {
    std::function<std::shared_ptr<const void>()> save;                  // the world as it is now
    std::function<void(const std::shared_ptr<const void>&)> restore;    // back to a save
};

} // namespace ohao::ecs
