#pragma once
// The navigation module, "navigation" (docs/specs/2026-11-18-tidewall-design.md, 3.1): the navmesh of the World's static
// collision on Recast/Detour (navigation/navigation.hpp), the `navigation` simulate system that walks NavAgents on it,
// its components (navigation/components.hpp: NavAgent, NavPath, NavObstacle, NavLink) and its API methods (nav.info,
// nav.path, nav.nearest, nav.reach). Recast and Detour are its own (navigation/private).
#include "ecs/module.hpp"

namespace ohao::ecs {

[[nodiscard]] const Module& navigationModule();
// Its registrations (navigation/types.cpp) and methods (navigation/methods.cpp).
void registerNavigationTypes(reflect::TypeRegistry& types);
[[nodiscard]] std::span<const Method> navigationMethods();

} // namespace ohao::ecs
