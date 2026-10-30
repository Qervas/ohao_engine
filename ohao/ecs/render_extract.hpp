#pragma once
#include "ecs/world.hpp"

#include <cstdint>

namespace ohao::render { class RenderScene; }

namespace ohao::ecs {

// The RenderScene instance slot of a renderable entity. Owned by the extract system:
// it emplaces and removes it; nothing else should write it.
struct RenderSlot { uint32_t slot; };

// Adds the "render_extract" system right after "transform" (after "interpolate" when the physics
// scene is installed first). An entity with a RenderPose is drawn there, not at its
// WorldTransform. Each update it
//   1. releases the slots of entities whose MeshRenderer (or RenderSlot) went away,
//   2. creates an instance for every renderable without a slot (in creation order),
//   3. re-uploads every RenderDirty renderable (moved, or MeshRenderer patched),
//   4. clears RenderDirty,
//   5. rebuilds the light list when a Light changed, appeared or
//      was destroyed.
// The RenderScene must outlive the World: destroying the World releases its slots
// and clears the lights it set. Install at most once per World.
void installRenderExtract(World& world, render::RenderScene& scene);

} // namespace ohao::ecs
