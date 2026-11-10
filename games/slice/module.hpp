#pragma once
// The slice, the game the engine is built with (step 3i; docs/plans/2026-11-09-step3-engine-game-boundary.md): combat
// with its rounds, enemies and grenades, its channels (slice.*), the first-person view model, the arms and enemy
// animation drivers, its HUD and grenades drawn, its 9 components and its methods (combat.info, combat.damage). A
// game::Module: a host plays it (`--game slice`, the default) or an engine-only world.
#include "game/api.hpp"

#include <span>

namespace ohao::ecs {
class World;
}
namespace ohao::reflect {
class TypeRegistry;
}

namespace ohao::slice {

[[nodiscard]] const game::Module& module();

// The module's parts, one a file.
void registerSliceTypes(reflect::TypeRegistry& types);   // types.cpp
[[nodiscard]] std::span<const game::Method> sliceMethods();   // methods.cpp
void installSlicePresentation(ecs::World& world);       // hud.cpp: its HUD and the grenades in flight
void forgetHud(ecs::World& world);                      // hud.cpp: the HUD's state (its cursor into the log)

} // namespace ohao::slice
