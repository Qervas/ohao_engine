#pragma once
// What the frame draws of the world's effects (sub-project 12, "The frame"): plain data the ECS's
// `vfx_draw` sync system (ecs/vfx_draw) writes into the RenderScene from the step's spawn records,
// and the deferred renderer's VfxRenderer reads. No ECS type: the renderer never sees a World.
#include "vfx/vfx_set.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace ohao::render {

// One live effect record: its effect in the set, its seed, its pose, its age this frame
// ((step - spawnStep - 1 + alpha) * dt: 0 at the step it was written, alpha 1), and its record
// sequence number (object ids for the sort's tie-break).
struct VfxFrameEffect {
    uint32_t effect{0};
    uint32_t seed{0};
    float age{0.0f};
    glm::mat4 pose{1.0f};
    uint64_t id{0};
};

struct VfxFrame {
    // The set the records index (null: nothing to draw) and a version that changes with it (the
    // renderer rebuilds its emitter table only then).
    std::shared_ptr<const vfx::VfxSet> set;
    uint64_t setVersion{0};
    // The bindless index of each of the set's textures (vfx::VfxSet::textures; kNoTexture where it
    // could not be loaded).
    std::vector<uint32_t> textures;
    std::vector<VfxFrameEffect> effects;   // in record order
};

} // namespace ohao::render
