#pragma once
// The nav_show module, "nav_show": nav.show {on}, the navmesh drawn into the frames a session renders (render.capture,
// a window) as lines over the final image: every polygon's edges, the links and the active obstacles' boxes
// (navigation/navigation.hpp, navDrawing), through world_render's presented lines. Presentation only: nothing it draws
// is saved, hashed or read by a step. Its own module (one feature over navigation and world_render): it waits, as
// world_render does, for a host's renderer, so its method is in the catalogue once a session renders.
#include "ecs/module.hpp"

namespace ohao::ecs {

[[nodiscard]] const Module& navShowModule();

} // namespace ohao::ecs
