#pragma once

#include "render/material/openpbr_tables_buffer.hpp"
#include "render_pass_base.hpp"
#include "gbuffer_pass.hpp"
#include "render/frame/frame_resources.hpp"

#include <array>

namespace ohao {

// Deferred lighting pass - reads G-Buffer and outputs lit HDR image
class DeferredLightingPass : public RenderPassBase {
public:
    DeferredLightingPass() = default;
    ~DeferredLightingPass() override;

    [[nodiscard]] bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
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
    void setShadowMap(VkImageView shadowMap, VkSampler shadowSampler);
    // RT GI's diffuse (binding 11) and its reflection (binding 22, frontier F5b); both or neither.
    void setSSGITexture(VkImageView ssgi, VkImageView reflection, VkSampler ssgiSampler);
    // RT direct light's demodulated diffuse and specular (render/rt/rt_direct_technique.hpp):
    // lighting multiplies them by the albedos and leaves every light but flashes out of its loop.
    // Null: none this frame (the loop lights everything; CSM, if bound, shadows the sun).
    void setDirectLight(VkImageView diffuse, VkImageView specular);
    // The weather the lighting applies (wetness, snow cover, frost cover), for RT direct light's target.
    [[nodiscard]] glm::vec3 weather() const noexcept {
        return {m_params.wetness, m_params.snowCover, m_params.frostCover};
    }
    // Image-based lighting (render/deferred/env_prefilter.hpp): the environment GGX-prefiltered
    // by roughness (binding 15, kEnvPrefilterLevels mips) and its irradiance (binding 7), with
    // their sampler, scaled by `intensity` (Environment.intensity). Null views: no IBL.
    void setEnvironment(VkImageView prefiltered, VkImageView irradiance, VkSampler sampler, float intensity);

    // Get output
    [[nodiscard]] VkImageView getOutputView() const { return m_hdrOutput.view; }
    [[nodiscard]] VkImage getOutputImage() const { return m_hdrOutput.image; }
    [[nodiscard]] VkRenderPass getRenderPass() const { return m_renderPass; }

    // The light table (LightGpu records, binding 5) and its light count.
    void setLights(VkBuffer lightBuffer, uint32_t count) {
        m_lightBuffer = lightBuffer;
        m_lightCount = count;
    }
    // The material table (binding 24): OpenPBR's layer rows of each pixel's material.
    void setMaterials(VkBuffer materials) { m_materialBuffer = materials; }
    // The froxel lists (binding 16) and the slice mapping they were built with; a null buffer
    // makes every pixel sum every light.
    void setClusters(VkBuffer clusters, float sliceScale, float sliceBias) {
        m_clusterBuffer = clusters;
        m_params.sliceScale = sliceScale;
        m_params.sliceBias = sliceBias;
    }

    // Camera data
    void setCameraData(const glm::vec3& position, const glm::mat4& view, const glm::mat4& invViewProj);

    // Cascade shadow map data (UBO from CSMPass, not owned here)
    void setCascadeBuffer(VkBuffer buf) { m_cascadeBuffer = buf; }

    // Wetness (0=dry, 1=fully wet) — set each frame before execute()
    void setWetness(float w) { m_params.wetness = glm::clamp(w, 0.0f, 1.0f); }
    // Snow cover (0=bare, 1=fully covered) — set each frame before execute()
    void setSnowCover(float s) { m_params.snowCover = glm::clamp(s, 0.0f, 1.0f); }
    // Frost cover (0=bare, 1=fully frosted) — set each frame before execute()
    void setFrostCover(float f) { m_params.frostCover = glm::clamp(f, 0.0f, 1.0f); }

private:
    [[nodiscard]] bool createRenderPass();
    [[nodiscard]] bool createFramebuffer();
    [[nodiscard]] bool createPipeline();
    [[nodiscard]] bool createOutputImages();
    [[nodiscard]] bool createTarget(RenderTarget& target, VkImageUsageFlags usage);
    void destroyOutputImages();
    [[nodiscard]] bool createDescriptors();

    // G-Buffer reference
    GBufferPass* m_gbufferPass{nullptr};

    // The color attachment: the lit HDR image
    RenderTarget m_hdrOutput;

    // Render pass and framebuffer
    VkRenderPass m_renderPass{VK_NULL_HANDLE};
    VkFramebuffer m_framebuffer{VK_NULL_HANDLE};

    // Pipeline
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Descriptors
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    void writeDescriptorSet(uint32_t slot);
    // One set per frame in flight. execute() rewrites only its own slot's set, whose
    // previous use the frame fence has already retired; a single shared set would be
    // rewritten while the previous frame's commands still read it.
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_descriptorSets{};
    VkSampler m_gbufferSampler{VK_NULL_HANDLE};

    // External resources
    VkBuffer m_lightBuffer{VK_NULL_HANDLE};
    uint32_t m_lightCount{0};
    VkBuffer m_clusterBuffer{VK_NULL_HANDLE};
    VkBuffer m_materialBuffer{VK_NULL_HANDLE};
    VkImageView m_shadowMapView{VK_NULL_HANDLE};
    VkSampler m_shadowSampler{VK_NULL_HANDLE};
    VkImageView m_irradianceView{VK_NULL_HANDLE};
    VkImageView m_prefilteredView{VK_NULL_HANDLE};
    VkSampler m_iblSampler{VK_NULL_HANDLE};
    VkImageView m_ssgiView{VK_NULL_HANDLE};
    VkImageView m_giReflectionView{VK_NULL_HANDLE};
    VkSampler m_ssgiSampler{VK_NULL_HANDLE};
    VkBuffer m_cascadeBuffer{VK_NULL_HANDLE};
    VkImageView m_directDiffuseView{VK_NULL_HANDLE};
    VkImageView m_directSpecularView{VK_NULL_HANDLE};

    // Dummy resources for unbound descriptor bindings (prevents Vulkan UB)
    VkImage m_dummyImage{VK_NULL_HANDLE};
    VkDeviceMemory m_dummyMemory{VK_NULL_HANDLE};
    VkImageView m_dummyView{VK_NULL_HANDLE};
    // 1x1 RG32UI stand-in for the RT shadow visibility (an integer binding takes an integer view)
    VkBuffer m_dummyBuffer{VK_NULL_HANDLE};
    render::OpenPBRTablesBuffer m_openpbrTables;   // binding 20
    VkDeviceMemory m_dummyBufferMemory{VK_NULL_HANDLE};
    bool m_dummyImageTransitioned{false};
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
                          // Total: 184 bytes

    LightingParams m_params{};
};

} // namespace ohao
