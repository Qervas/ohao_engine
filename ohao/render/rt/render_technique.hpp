#pragma once

// The hybrid frame's quality and RT GI's inputs and outputs (render/rt/rt_gi_technique.hpp).

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <span>
#include <string>
#include <cstdint>

namespace ohao::render { struct LightDesc; }

namespace ohao {

// The hybrid frame's quality (sub-project 2b spec, "Reference quality"). Realtime is the
// budgeted frame; reference resamples and traces without reuse or denoising (RT direct light,
// RT GI) and averages the frames since the last temporal reset, the realtime frame's converged
// answer.
enum class RenderQuality : uint8_t { Realtime, Reference };

// ─── Global Illumination Technique ───────────────────────────────────

struct GIInput {
    VkImageView positionBuffer;    // GBuffer0: world position, metallic
    VkImageView normalBuffer;      // GBuffer1: octahedral normal
    VkImageView albedoBuffer;      // GBuffer2: albedo (the output's diffuse albedo)
    VkImageView layersBuffer = VK_NULL_HANDLE;   // the G-buffer's OpenPBR layers (RG32_UINT): the coat's roughness
    VkImageView velocityBuffer = VK_NULL_HANDLE;   // GBuffer3: UV motion, the temporal reprojection
    glm::vec3 cameraPos{0.0f};
    uint32_t width;
    uint32_t height;
    uint32_t frameIndex = 0;       // since the last temporal reset: seeds samples; 0 takes no history
    uint32_t renderSeed = 0;       // the render seed: with frameIndex, pixel and sample, seeds each ray
    uint32_t frameSlot = 0;        // frame-in-flight slot; selects this frame's descriptor set
    RenderQuality quality = RenderQuality::Realtime;

    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;   // RenderScene TLAS (custom index = slot)
    // Set 1 and 2 of the trace (rt/alpha_test.slang): RenderScene's tables (rtc::AlphaTables)
    // and the bindless textures, for alpha-tested and blended candidates.
    VkDescriptorSet alphaTables = VK_NULL_HANDLE;
    VkDescriptorSet textures = VK_NULL_HANDLE;
    VkBuffer instances = VK_NULL_HANDLE;                // RenderScene instance table (InstanceGpu[])

    // The scene's lights; a bounce is lit by every one of them (flashes excluded).
    // None: hits bring back no light.
    std::span<const render::LightDesc> lights;
    // The environment a ray that leaves the scene sees (equirectangular, in
    // SHADER_READ_ONLY_OPTIMAL); null: black.
    VkImageView envMap = VK_NULL_HANDLE;
    float envIntensity = 1.0f;     // Environment.intensity, as the path tracer's miss applies it
    // RT direct light runs this frame and lights emissive surfaces: a hit's emission is direct light
    // it already counts, so the bounce does not bring it back.
    bool emissionIsDirect = false;
};

struct GIOutput {
    VkImage indirectLight;         // RGBA16F, full resolution: rgb incoming radiance (no albedo), a the environment's visibility
    VkImageView indirectLightView;
    VkImageView reflectionView;    // RGBA16F, full resolution: rgb the radiance over the GGX lobe (no albedo), a roughness
};

} // namespace ohao
