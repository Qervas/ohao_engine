#pragma once
// What the frame draws of the world's effects (sub-project 12, "The frame"): plain data the ECS's
// `vfx_draw` sync system (ecs/vfx_draw) writes into the RenderScene from the step's spawn records,
// and the deferred renderer's VfxRenderer reads. No ECS type: the renderer never sees a World.
#include "vfx/smoke_math.hpp"
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

// One live decal record: its set index, its fade (the last tenth of its life), its world pose (the
// record's pose, followed by what it is on at this frame's alpha) and its id (the sort's tie-break).
struct VfxFrameDecal {
    uint32_t decal{0};
    float fade{1.0f};
    glm::mat4 pose{1.0f};
    uint64_t id{0};
};

// One live tracer record: its set index, the tracer's look (resolved from the set), the round's
// ends and the record's age; the renderer places its head at speed * age, clamped to the end.
struct VfxFrameTracer {
    uint32_t tracer{0};
    float speed{400.0f};
    float length{1.5f};
    float width{0.012f};
    glm::vec3 color{1.0f};          // linear, times the tracer's HDR intensity
    float age{0.0f};
    glm::vec3 from{0.0f}, to{0.0f};
    uint64_t id{0};
};

// One transient light a live flash record gives this frame (a boom's glow): a point light that
// falls off to nothing at its end, lit through the clustered lights like any raster light.
struct VfxFrameLight {
    glm::vec3 position{0.0f};
    glm::vec3 color{1.0f};           // linear
    float intensity{0.0f};           // at this frame's age (the flash's, faded to 0 at its end)
    float range{8.0f};              // m
    uint64_t id{0};
};

struct VfxFrame {
    // The set the records index (null: nothing to draw) and a version that changes with it (the
    // renderer rebuilds its emitter table only then).
    std::shared_ptr<const vfx::VfxSet> set;
    uint64_t setVersion{0};
    // The bindless index of each of the set's textures (vfx::VfxSet::textures; kNoTexture where it
    // could not be loaded), and of each texture's normal map (kNoTexture: none).
    std::vector<uint32_t> textures;
    std::vector<uint32_t> normals;
    std::vector<VfxFrameEffect> effects;   // in record order
    std::vector<VfxFrameDecal> decals;     // in record order
    std::vector<VfxFrameTracer> tracers;   // in record order
    // Smoke's proxy spheres this frame, oldest record first (at most the set's caps.proxies: the newest
    // records' spheres are kept when they do not all fit); RT shadows and GI attenuate through them.
    std::vector<vfx::SmokeSphere> smoke;
    std::vector<VfxFrameLight> lights;     // transient lights, in record order
};

} // namespace ohao::render
