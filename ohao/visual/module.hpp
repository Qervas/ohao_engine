#pragma once
// The visual module, "visual": what an entity looks like and where the world is seen from, as data and CPU code only
// (its components: visual/components.hpp; a primitive's mesh: visual/primitive_mesh.hpp; the view a host applies:
// visual/scene_view.hpp). Headless: physics fits colliders to a MeshSource, audio hears from the Camera, a level writes
// them, without linking the renderer (world_render draws them). It installs no system.
#include "ecs/module.hpp"

namespace ohao::ecs {

[[nodiscard]] const Module& visualModule();
// Its registrations (visual/types.cpp).
void registerVisualTypes(reflect::TypeRegistry& types);

} // namespace ohao::ecs
