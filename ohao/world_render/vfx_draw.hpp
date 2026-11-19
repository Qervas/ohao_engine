#pragma once
// The frame's effects (sub-project 12, docs/specs/2026-10-28-vfx-design.md, "The frame"): the
// `vfx_draw` sync system, after the render extract, gives the frame packet (world_render/frame_packet.hpp) the
// live effect records of the `vfx` system (vfx/vfx.hpp) as a render::VfxFrame, with their flashes and a game's
// presented spheres: each record's effect, seed and pose, and its age at this frame, (steps - spawnStep - 1 +
// alpha) steps of the step's dt (0 at the step it was written, alpha 1). The set is copied, and its textures loaded into the bindless table, only when it
// changes. Every host installs it (the CLI context, LiveHost), so a window and a headless capture of
// the same world draw the same effects; no clock or frame counter reaches the frame.

#include "render/scene/vfx_frame.hpp"

namespace ohao { class BindlessTextureManager; }

namespace ohao::ecs {

class World;
class AssetResolver;


// The frame `world` draws at its render alpha (the set left null when the world has no vfx system).
// `textures` and `normals` as the set's bindless indices (empty: none loaded). Tests and benches.
[[nodiscard]] render::VfxFrame vfxFrameOf(const World& world, std::shared_ptr<const vfx::VfxSet> set,
                                          std::vector<uint32_t> textures, std::vector<uint32_t> normals = {});

} // namespace ohao::ecs
