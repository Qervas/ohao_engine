#pragma once

// RT direct light (frontier F4): ReSTIR DI over the lights the path tracer samples, in MegaLights'
// structure, for the deferred frame. Compute passes with inline ray queries (VK_KHR_ray_query; without
// it init() fails and lighting loops over the lights with CSM for the sun).
//
// realtime   Resampling and tracing at half resolution (one pixel of each 2x2 block, picked by a hash
//            each frame; `halfResolution` false: every pixel): sample() draws each trace pixel's
//            candidates and reuses last frame's reservoir (di.sample); trace() reuses its neighbours'
//            and traces the final point (di.trace); shade() shades every frame pixel from the trace
//            pixels around it with its own material, demodulated diffuse and specular light
//            (di.shade); denoise() accumulates them over frames and filters them (di.denoise: a
//            temporal accumulation, 3 a-trous steps). rt/restir_di.slang, rt/di_denoise.slang.
// reference  Full resolution; sample() and trace() reuse nothing, shade() keeps the running mean of
//            the frames since the last reset; denoise() does nothing.
//
// Lighting reads the output (diffuse(), specular()) and multiplies the lobes' directional albedos back.

#include "render_technique.hpp"
#include "rt_compute.hpp"
#include "render/frame/frame_resources.hpp"
#include "render/material/openpbr_tables_buffer.hpp"
#include "gpu/shared_layout.hpp"
#include <array>
#include <span>
#include <vector>

namespace ohao::render { class EmissiveLights; }

namespace ohao {

struct DirectInput {
    VkImageView positionBuffer;    // GBuffer0: world position, metallic
    VkImageView normalBuffer;      // GBuffer1: octahedral normal, roughness
    VkImageView albedoBuffer;      // GBuffer2: albedo, AO
    VkImageView specularBuffer;    // the G-buffer's specular_color, specular_weight / 1.5
    VkImageView openpbrBuffer;     // the G-buffer's base_diffuse_roughness, IOR
    VkImageView layersBuffer;      // the G-buffer's OpenPBR layers (RG32_UINT)
    VkImageView velocityBuffer;    // GBuffer3: UV motion
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frameSlot = 0;        // frame-in-flight slot; selects this frame's descriptor sets
    uint32_t frameIndex = 0;       // since the last temporal reset: seeds samples; 0 takes no history
    uint32_t renderSeed = 0;
    RenderQuality quality = RenderQuality::Realtime;
    uint32_t candidates = 8;       // light points a pixel draws a frame (1..kDIMaxCandidates)
    uint32_t spatial = 4;          // neighbours a pixel's spatial reuse combines (0..kDIMaxSpatial)
    bool halfResolution = true;    // realtime: resample and trace at half resolution
    glm::vec3 cameraPos{0.0f};
    glm::vec3 weather{0.0f};       // wetness, snow cover, frost cover (the lighting pass's)

    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    // Sets 1 and 2 (rt/alpha_test.slang): RenderScene's tables and the bindless textures' set.
    VkDescriptorSet alphaTables = VK_NULL_HANDLE;
    VkDescriptorSet textures = VK_NULL_HANDLE;
    VkBuffer lightData = VK_NULL_HANDLE;   // RenderScene::rtLightBuffer: the path tracer's lights
    VkBuffer materials = VK_NULL_HANDLE;   // RenderScene::materialBuffer
    bool layers = true;                    // RenderScene::anyLayeredMaterial: di.shade's layered kernel
    const render::EmissiveLights* emissive = nullptr;   // this frame's table, recorded before sample()
};

class RTDirectTechnique {
public:
    RTDirectTechnique() = default;
    ~RTDirectTechnique();
    RTDirectTechnique(const RTDirectTechnique&) = delete;
    RTDirectTechnique& operator=(const RTDirectTechnique&) = delete;

    // Sets 1 and 2 of the passes (rt/alpha_test.slang). Before init.
    void setAlphaLayouts(VkDescriptorSetLayout tables, VkDescriptorSetLayout textures) noexcept {
        m_alphaTablesLayout = tables;
        m_texturesLayout = textures;
    }
    // The bindless table as binding 12 of set 0 (rt/lights.slang's textures): views[i] is index i.
    // Rewritten into a slot's set only when the table changed since that slot last wrote it.
    void setBindlessTextures(std::span<const VkImageView> views, std::span<const VkSampler> samplers);

    bool init(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t width, uint32_t height);
    void resize(uint32_t width, uint32_t height);
    void destroy();

    // di.sample, then (each in its own pass) di.trace, di.shade and, realtime, di.denoise.
    void sample(VkCommandBuffer cmd, const DirectInput& input);
    void trace(VkCommandBuffer cmd);
    void shade(VkCommandBuffer cmd);
    void denoise(VkCommandBuffer cmd);
    [[nodiscard]] bool denoises() const noexcept { return m_rendered && m_quality == RenderQuality::Realtime; }
    // True when this frame's sample() ran; false: nothing was written, lighting must not read it.
    [[nodiscard]] bool rendered() const noexcept { return m_rendered; }
    // The light lighting reads (SHADER_READ_ONLY_OPTIMAL once this frame's passes ran).
    [[nodiscard]] VkImageView diffuse() const noexcept;
    [[nodiscard]] VkImageView specular() const noexcept;

    void resetHistory() noexcept {
        m_historyValid = false;
        m_refFrames = 0;
    }

private:
    using Image = rtc::Image;
    bool createScreenImages();
    void destroyScreenImages();
    bool ensureReferenceImages();
    bool createDescriptorResources();
    bool createPipelines();
    void writeSampleSet(const DirectInput& input);

    void dispatchSample(VkCommandBuffer cmd, VkPipeline pipeline, uint32_t width, uint32_t height);

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    uint32_t m_width = 0, m_height = 0;
    // This frame's trace image: the frame's size >> m_shift (the images are the frame's size; half
    // resolution uses their top-left quarter). A changed shift leaves the reservoirs' history.
    uint32_t m_shift = 1;
    uint32_t m_traceWidth = 0, m_traceHeight = 0;

    // Reservoirs (A, B), the trace image's: the final ones ping-pong (this frame's, last frame's), the
    // temporal ones between the passes.
    std::array<Image, 2> m_resA{}, m_resB{};
    Image m_midA{}, m_midB{};
    // The light: noisy, the accumulations (ping-pong), the a-trous intermediate and output.
    Image m_noisyDiffuse{}, m_noisySpecular{};
    std::array<Image, 2> m_histDiffuse{}, m_histSpecular{};
    // The trace image's compact surfaces (restir_di.slang's Surf: the guides of reuse), ping-pong.
    std::array<Image, 2> m_surfA{}, m_surfB{};
    std::array<Image, 2> m_guide{};   // the denoiser's: distance, normal and spread (RG32F, the frame's)
    std::array<Image, 2> m_moments{};   // the denoiser's: mean square luminances and frames (RGBA32F), ping-pong
    Image m_tmpDiffuse{}, m_tmpSpecular{};
    Image m_outDiffuse{}, m_outSpecular{};
    // Reference: the running means (created on first use) and 1x1 stand-ins.
    Image m_meanDiffuse{}, m_meanSpecular{}, m_meanDummy{};
    bool m_imagesReady = false;
    uint32_t m_cur = 0;
    bool m_historyValid = false;
    uint32_t m_refFrames = 0;

    // This frame (sample() to denoise()).
    bool m_rendered = false;
    RenderQuality m_quality = RenderQuality::Realtime;
    uint32_t m_slot = 0;
    gpu::shared::DIPush m_push{};
    VkDescriptorSet m_alphaTables = VK_NULL_HANDLE;
    VkDescriptorSet m_textures = VK_NULL_HANDLE;
    VkImageView m_velocityView = VK_NULL_HANDLE;

    // The bindless table (binding 12) and the version each slot's set holds.
    std::vector<VkDescriptorImageInfo> m_textureInfos;
    uint64_t m_textureVersion = 1;
    std::array<uint64_t, MAX_FRAMES_IN_FLIGHT> m_slotTextureVersion{};
    uint32_t m_textureCapacity = 4096;

    render::OpenPBRTablesBuffer m_openpbrTables;

    VkDescriptorSetLayout m_alphaTablesLayout = VK_NULL_HANDLE, m_texturesLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_sampleSetLayout = VK_NULL_HANDLE, m_denoiseSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_sampleLayout = VK_NULL_HANDLE, m_denoiseLayout = VK_NULL_HANDLE;
    std::vector<VkPipelineLayout> m_extraLayouts;   // the other pipelines' own layouts, kept until teardown
    VkPipeline m_initial = VK_NULL_HANDLE, m_spatial = VK_NULL_HANDLE, m_shade = VK_NULL_HANDLE;
    VkPipeline m_shadeLayered = VK_NULL_HANDLE;   // shade with OpenPBR's layers (restir_di.slang)
    bool m_layers = true;
    VkPipeline m_temporal = VK_NULL_HANDLE, m_atrous = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_sampleSets{};
    std::array<std::array<VkDescriptorSet, 4>, MAX_FRAMES_IN_FLIGHT> m_denoiseSets{};   // temporal, atrous x3
};

} // namespace ohao
