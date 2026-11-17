#pragma once

// The hybrid frame's quality and RT GI's inputs and outputs (render/rt/rt_gi_technique.hpp).

#include "gpu/vulkan/gpu_image.hpp"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <span>
#include <string>
#include <cstdint>

namespace ohao::render { struct LightDesc; class RenderScene; }
namespace ohao::rtc { class AlphaTables; }

namespace ohao {

// The hybrid frame's quality (sub-project 2b spec, "Reference quality"). Realtime is the
// budgeted frame; reference resamples and traces without reuse or denoising (RT direct light,
// RT GI) and averages the frames since the last temporal reset, the realtime frame's converged
// answer.
enum class RenderQuality : uint8_t { Realtime, Reference };

// ─── Global Illumination Technique ───────────────────────────────────

struct GIInput {
    gpu::Image* positionBuffer = nullptr;    // GBuffer0: world position, metallic
    gpu::Image* normalBuffer = nullptr;      // GBuffer1: octahedral normal
    gpu::Image* albedoBuffer = nullptr;      // GBuffer2: albedo (the output's diffuse albedo)
    gpu::Image* layersBuffer = nullptr;      // the G-buffer's OpenPBR layers (RG32_UINT): the coat's roughness
    gpu::Image* velocityBuffer = nullptr;    // GBuffer3: UV motion, the temporal reprojection
    glm::vec3 cameraPos{0.0f};
    uint32_t width;
    uint32_t height;
    uint32_t frameIndex = 0;       // since the last temporal reset: seeds samples; 0 takes no history
    uint32_t renderSeed = 0;       // the render seed: with frameIndex, pixel and sample, seeds each ray
    uint32_t frameSlot = 0;        // frame-in-flight slot; selects this frame's descriptor set
    RenderQuality quality = RenderQuality::Realtime;

    // The scene: its TLAS (custom index = slot) and its instance table (InstanceGpu[]).
    const render::RenderScene* scene = nullptr;
    // Set 1 and 2 of the trace (rt/alpha_test.slang): RenderScene's tables (this slot's rtc::AlphaTables set)
    // and the bindless textures, for alpha-tested and blended candidates.
    rtc::AlphaTables* alphaTables = nullptr;
    VkDescriptorSet textures = VK_NULL_HANDLE;

    // The scene's lights; a bounce is lit by every one of them (flashes excluded).
    // None: hits bring back no light.
    std::span<const render::LightDesc> lights;
    // The environment a ray that leaves the scene sees (equirectangular); null: black.
    gpu::Image* envMap = nullptr;
    float envIntensity = 1.0f;     // Environment.intensity, as the path tracer's miss applies it
    // RT direct light runs this frame and lights emissive surfaces: a hit's emission is direct light
    // it already counts, so the bounce does not bring it back.
    bool emissionIsDirect = false;
};

struct GIOutput {
    gpu::Image* indirectLight;     // RGBA16F, full resolution: rgb incoming radiance (no albedo), a the environment's visibility
    gpu::Image* reflection;        // RGBA16F, full resolution: rgb the radiance over the GGX lobe (no albedo), a roughness
};

} // namespace ohao
