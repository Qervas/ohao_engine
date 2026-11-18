#pragma once
// The world_render module, "world_render": the world into the renderer. Instantiate (MeshSources into meshes,
// world_render/instantiate.hpp), the render extract, the effects' draw (world_render/vfx_draw.hpp), its component
// (world_render/components.hpp: MeshRenderer) and what a game presents (world_render/present.hpp: HUDs, spheres, its
// contributions). It needs the host's renderer: render::RenderScene among the services (and BindlessTextureManager,
// when the host has one), so it waits on the World until a host installs with them (engine/install.hpp).
#include "ecs/module.hpp"

namespace ohao::ecs {

[[nodiscard]] const Module& worldRenderModule();
// Its registrations (world_render/types.cpp).
void registerWorldRenderTypes(reflect::TypeRegistry& types);

} // namespace ohao::ecs
