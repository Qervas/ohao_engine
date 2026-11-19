#pragma once
// The world_render module's systems (world_render/module.hpp), installed by its install in this order: instantiate,
// the render extract, the effects' draw.
#include "ecs/asset_resolver.hpp"
#include "ecs/world.hpp"

namespace ohao { class BindlessTextureManager; }
namespace ohao::render { class RenderScene; }

namespace ohao::ecs {

// Adds the "render_extract" system right after "transform" (after "interpolate" when the physics
// scene is installed first). An entity with a RenderPose is drawn there, not at its
// WorldTransform. Each update it
//   1. releases the slots of entities whose MeshRenderer (or RenderSlot) went away,
//   2. creates an instance for every renderable without a slot (in creation order),
//   3. re-uploads every RenderDirty renderable (moved, or MeshRenderer patched),
//   4. clears RenderDirty,
//   5. rebuilds the light list when a Light changed, appeared or
//      was destroyed,
//   6. writes the view (the world's Camera and Environment, visual/scene_view.hpp; an hdri resolved by `assets`).
// All of it goes into the world's frame packet (world_render/frame_packet.hpp), by instance id; the scene is read
// for its meshes, and a skinned entity's own mesh registered in it. The RenderScene must outlive the World, and
// a World's instances go with the renderer. Install at most once per World.
void installRenderExtract(World& world, render::RenderScene& scene, const AssetResolver& assets);

// Adds the "instantiate" system before "transform" (so a new mesh renders in the frame
// its MeshSource appears; world_render/instantiate.hpp). Install once per World; the scene, texture manager and
// resolver must outlive the World.
void installInstantiate(World& world, render::RenderScene& scene, BindlessTextureManager* textures,
                        AssetResolver& assets);

// Installs `vfx_draw` (after render_extract, else last; world_render/vfx_draw.hpp): the effects into the frame
// packet. `textures` may be null (no flipbooks: a textured emitter draws a soft dot).
void installVfxDraw(World& world, BindlessTextureManager* textures, AssetResolver& assets);

} // namespace ohao::ecs
