#pragma once

#include "render/material/openpbr_tables_buffer.hpp"
#include "render/shader_bindings/core_deferred_lighting.hpp"
#include "render_pass_base.hpp"
#include "gbuffer_pass.hpp"
#include "gpu/vulkan/gpu_allocator.hpp"
#include "render/frame/frame_resources.hpp"
#include "render/graph/pass.hpp"

#include <array>

namespace ohao {

// Deferred lighting pass - reads G-Buffer and outputs lit HDR image
class DeferredLightingPass : public RenderPassBase {
public:
    DeferredLightingPass() = default;
    ~DeferredLightingPass() override;

    [[nodiscard]] bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override { record(render::Frame{cmd, nullptr, frameIndex}); }
    // The fullscreen draw, timed as lighting.
    void record(const render::Frame& frame);
    void onResize(uint32_t width, uint32_t height) override;
    [[nodiscard]] const char* getName() const override { return "DeferredLightingPass"; }

    // Configuration
    void setGBufferPass(GBufferPass* gbufferPass);
    // Forget the inputs that other passes own and re-supply every frame (RT GI,
    // RT direct light). Call it when those passes may have recreated
    // their images -- a resize -- BEFORE anything rewrites this pass's
    // descriptors; the bindings fall back to the dummy view until render()
    // hands the new views back. Does not itself touch descriptors.
    void clearPerFrameInputs();
    // The CSM fallback's map (null: none) and the sampler that compares against it.
    void setShadowMap(gpu::Image* shadowMap, VkSampler shadowSampler);
    // RT GI's diffuse (binding 11) and its reflection (binding 22, frontier F5b); both or neither.
    void setSSGITexture(gpu::Image* ssgi, gpu::Image* reflection, VkSampler ssgiSampler);
    // RT direct light's demodulated diffuse and specular (render/rt/rt_direct_technique.hpp):
    // lighting multiplies them by the albedos and leaves every light but flashes out of its loop.
    // Null: none this frame (the loop lights everything; CSM, if bound, shadows the sun).
    void setDirectLight(gpu::Image* diffuse, gpu::Image* specular);
    // The weather the lighting applies (wetness, snow cover, frost cover), for RT direct light's target.
    [[nodiscard]] glm::vec3 weather() const noexcept {
        return {m_params.wetness, m_params.snowCover, m_params.frostCover};
    }
    // Image-based lighting (render/deferred/env_prefilter.hpp): the environment GGX-prefiltered
    // by roughness (binding 15, kEnvPrefilterLevels mips) and its irradiance (binding 7), with
    // their sampler, scaled by `intensity` (Environment.intensity). Null images: no IBL.
    void setEnvironment(gpu::Image* prefiltered, gpu::Image* irradiance, VkSampler sampler, float intensity);

    // The lit image (RGBA16F): the lit scene, the environment in sky pixels.
    [[nodiscard]] gpu::Image& lit() { return m_lit; }
    // A subsurface pixel's diffuse irradiance and albedo (packSubsurfaceDiffuse; 0 elsewhere), for the SSS pass.
    [[nodiscard]] gpu::Image& subsurfaceDiffuse() { return m_subsurfaceDiffuse; }

    // The light table (LightGpu records, binding 5; null: none) and its light count.
    void setLights(GpuBuffer* lightBuffer, uint32_t count) {
        m_lightBuffer = lightBuffer;
        m_lightCount = count;
    }
    // The material table (binding 24): OpenPBR's layer rows of each pixel's material.
    void setMaterials(GpuBuffer* materials) { m_materialBuffer = materials; }
    // The froxel lists (binding 16) and the slice mapping they were built with; a null buffer
    // makes every pixel sum every light.
    void setClusters(GpuBuffer* clusters, float sliceScale, float sliceBias) {
        m_clusterBuffer = clusters;
        m_params.sliceScale = sliceScale;
        m_params.sliceBias = sliceBias;
    }

    // Camera data
    void setCameraData(const glm::vec3& position, const glm::mat4& view, const glm::mat4& invViewProj);

    // Cascade shadow map data (UBO from CSMPass, not owned here)
    void setCascadeBuffer(GpuBuffer* buf) { m_cascadeBuffer = buf; }

    // Wetness (0=dry, 1=fully wet) — set each frame before record()
    void setWetness(float w) { m_params.wetness = glm::clamp(w, 0.0f, 1.0f); }
    // Snow cover (0=bare, 1=fully covered) — set each frame before record()
    void setSnowCover(float s) { m_params.snowCover = glm::clamp(s, 0.0f, 1.0f); }
    // Frost cover (0=bare, 1=fully frosted) — set each frame before record()
    void setFrostCover(float f) { m_params.frostCover = glm::clamp(f, 0.0f, 1.0f); }

private:
    [[nodiscard]] bool createPipeline();
    [[nodiscard]] bool createOutputImages();
    static constexpr VkFormat kLitFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kSubsurfaceDiffuseFormat = VK_FORMAT_R32G32B32A32_UINT;
    void destroyOutputImages();
    [[nodiscard]] bool createDescriptors();

    // G-Buffer reference
    GBufferPass* m_gbufferPass{nullptr};

    // The color attachments: the lit HDR image, the subsurface pixels' diffuse light
    gpu::Image m_lit;
    gpu::Image m_subsurfaceDiffuse;

    // Pipeline
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Descriptors
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    void writeDescriptorSet(uint32_t slot);
    // One set per frame in flight. record() rewrites only its own slot's set, whose
    // previous use the frame fence has already retired; a single shared set would be
    // rewritten while the previous frame's commands still read it.
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_descriptorSets{};
    VkSampler m_gbufferSampler{VK_NULL_HANDLE};

    // External resources
    GpuBuffer* m_lightBuffer{nullptr};
    uint32_t m_lightCount{0};
    GpuBuffer* m_clusterBuffer{nullptr};
    GpuBuffer* m_materialBuffer{nullptr};
    gpu::Image* m_shadowMap{nullptr};
    // The shadow map when one is bound and exists.
    [[nodiscard]] gpu::Image* boundShadowMap() const { return m_shadowMap && *m_shadowMap ? m_shadowMap : nullptr; }
    VkSampler m_shadowSampler{VK_NULL_HANDLE};
    gpu::Image* m_irradiance{nullptr};
    gpu::Image* m_prefiltered{nullptr};
    // Both of the environment's images are bound and exist.
    [[nodiscard]] bool hasEnvironment() const {
        return m_irradiance && *m_irradiance && m_prefiltered && *m_prefiltered;
    }
    VkSampler m_iblSampler{VK_NULL_HANDLE};
    gpu::Image* m_ssgi{nullptr};
    gpu::Image* m_giReflection{nullptr};
    VkSampler m_ssgiSampler{VK_NULL_HANDLE};
    GpuBuffer* m_cascadeBuffer{nullptr};
    gpu::Image* m_directDiffuse{nullptr};
    gpu::Image* m_directSpecular{nullptr};

    // Dummy resources for unbound descriptor bindings (prevents Vulkan UB): a 1x1 RGBA8 image and a
    // zeroed buffer
    gpu::Image m_dummyImage;
    GpuBuffer m_dummyBuffer;
    render::OpenPBRTablesBuffer m_openpbrTables;   // binding 20
    [[nodiscard]] bool createDummyResources();
    void destroyDummyResources();

    // Dimensions
    uint32_t m_width{0};
    uint32_t m_height{0};

    // Push constants (must stay <= 256 bytes — NVIDIA limit)
    struct LightingParams {
        glm::mat4 invViewProj{1.0f};
        glm::mat4 view{1.0f};
        glm::vec3 cameraPos{0.0f};
        float pad0{0.0f};
        glm::vec2 screenSize{0.0f};
        uint32_t lightCount{0};
        uint32_t flags{0};
        float wetness{0.0f};      // Default to dry
        float sliceScale{0.0f};   // the froxel slice of a distance d: floor(log(d) * sliceScale + sliceBias)
        float snowCover{0.0f};    // Default to clear
        float sliceBias{0.0f};
        float frostCover{0.0f};   // Default to no frost
        float envIntensity{1.0f}; // Environment.intensity: scales the IBL terms
    };
    OHAO_CHECK_PUSH(LightingParams, core_deferred_lighting)   // shaders/core/deferred_lighting.slang's
    LightingParams m_params{};
};

} // namespace ohao
