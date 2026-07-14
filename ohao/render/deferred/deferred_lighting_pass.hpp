#pragma once

#include "render_pass_base.hpp"
#include "gbuffer_pass.hpp"

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
    void setShadowMap(VkImageView shadowMap, VkSampler shadowSampler);
    void setIBLTextures(VkImageView irradiance, VkImageView prefiltered,
                        VkImageView brdfLUT, VkSampler iblSampler);
    void setSSAOTexture(VkImageView ssao, VkSampler ssaoSampler);
    void setSSGITexture(VkImageView ssgi, VkSampler ssgiSampler);
    void setCloudShadow(VkImageView view, VkSampler sampler,
                        const glm::vec2& center, const glm::vec2& extent);
    void setRTShadowMask(VkImageView view, VkSampler sampler);
    void setEnvMap(VkImageView view, VkSampler sampler);

    // Update descriptors when resources change
    void updateDescriptorSets();

    // Get output
    [[nodiscard]] VkImageView getOutputView() const { return m_hdrOutput.view; }
    [[nodiscard]] VkImage getOutputImage() const { return m_hdrOutput.image; }
    [[nodiscard]] VkRenderPass getRenderPass() const { return m_renderPass; }

    // Light management
    void setLightBuffer(VkBuffer lightBuffer) { m_lightBuffer = lightBuffer; }
    void setLightCount(uint32_t count) { m_lightCount = count; }

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
    [[nodiscard]] bool createOutputImage();
    [[nodiscard]] bool createDescriptors();

    // G-Buffer reference
    GBufferPass* m_gbufferPass{nullptr};

    // HDR output
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
    VkDescriptorSet m_descriptorSet{VK_NULL_HANDLE};
    VkSampler m_gbufferSampler{VK_NULL_HANDLE};

    // External resources
    VkBuffer m_lightBuffer{VK_NULL_HANDLE};
    uint32_t m_lightCount{0};
    VkImageView m_shadowMapView{VK_NULL_HANDLE};
    VkSampler m_shadowSampler{VK_NULL_HANDLE};
    VkImageView m_irradianceView{VK_NULL_HANDLE};
    VkImageView m_prefilteredView{VK_NULL_HANDLE};
    VkImageView m_brdfLUTView{VK_NULL_HANDLE};
    VkSampler m_iblSampler{VK_NULL_HANDLE};
    VkImageView m_ssaoView{VK_NULL_HANDLE};
    VkSampler m_ssaoSampler{VK_NULL_HANDLE};
    VkImageView m_ssgiView{VK_NULL_HANDLE};
    VkSampler m_ssgiSampler{VK_NULL_HANDLE};
    VkBuffer m_cascadeBuffer{VK_NULL_HANDLE};
    VkImageView m_cloudShadowView{VK_NULL_HANDLE};
    VkSampler m_cloudShadowSampler{VK_NULL_HANDLE};
    VkImageView m_rtShadowView{VK_NULL_HANDLE};
    VkSampler m_rtShadowSampler{VK_NULL_HANDLE};
    VkImageView m_envMapView{VK_NULL_HANDLE};
    VkSampler m_envMapSampler{VK_NULL_HANDLE};

    // Dummy resources for unbound descriptor bindings (prevents Vulkan UB)
    VkImage m_dummyImage{VK_NULL_HANDLE};
    VkDeviceMemory m_dummyMemory{VK_NULL_HANDLE};
    VkImageView m_dummyView{VK_NULL_HANDLE};
    VkBuffer m_dummyBuffer{VK_NULL_HANDLE};
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
        float padding1{0.0f};
        glm::vec2 screenSize{0.0f};
        uint32_t lightCount{0};
        uint32_t flags{0};
        float wetness{0.0f};      // Default to dry
        float paddingW{0.0f};
        float snowCover{0.0f};    // Default to clear
        float paddingS{0.0f};
        float frostCover{0.0f};   // Default to no frost
        float paddingF{0.0f};
        glm::vec2 cloudShadowCenter{0.0f};
        glm::vec2 cloudShadowExtent{100.0f};
    };
                          // Total: 200 bytes

    LightingParams m_params{};
};

} // namespace ohao
