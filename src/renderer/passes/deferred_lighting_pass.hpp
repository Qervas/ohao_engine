#pragma once

#include "render_pass_base.hpp"
#include "gbuffer_pass.hpp"

namespace ohao {

// Deferred lighting pass - reads G-Buffer and outputs lit HDR image
class DeferredLightingPass : public RenderPassBase {
public:
    DeferredLightingPass() = default;
    ~DeferredLightingPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "DeferredLightingPass"; }

    // Configuration
    void setGBufferPass(GBufferPass* gbufferPass);
    void setShadowMap(VkImageView shadowMap, VkSampler shadowSampler);
    void setIBLTextures(VkImageView irradiance, VkImageView prefiltered,
                        VkImageView brdfLUT, VkSampler iblSampler);
    void setSSAOTexture(VkImageView ssao, VkSampler ssaoSampler);
    void setSSGITexture(VkImageView ssgi, VkSampler ssgiSampler);

    // Update descriptors when resources change
    void updateDescriptorSets();

    // Get output
    VkImageView getOutputView() const { return m_hdrOutput.view; }
    VkImage getOutputImage() const { return m_hdrOutput.image; }
    VkRenderPass getRenderPass() const { return m_renderPass; }

    // Light management
    void setLightBuffer(VkBuffer lightBuffer) { m_lightBuffer = lightBuffer; }
    void setLightCount(uint32_t count) { m_lightCount = count; }

    // Camera data
    void setCameraData(const glm::vec3& position, const glm::mat4& view, const glm::mat4& invViewProj);

    // Cascade shadow map data (UBO from CSMPass, not owned here)
    void setCascadeBuffer(VkBuffer buf) { m_cascadeBuffer = buf; }

    // Wetness (0=dry, 1=fully wet) — set each frame before execute()
    void setWetness(float w) { m_params.wetness = glm::clamp(w, 0.0f, 1.0f); }

private:
    bool createRenderPass();
    bool createFramebuffer();
    bool createPipeline();
    bool createOutputImage();
    bool createDescriptors();

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

    // Dummy resources for unbound descriptor bindings (prevents Vulkan UB)
    VkImage m_dummyImage{VK_NULL_HANDLE};
    VkDeviceMemory m_dummyMemory{VK_NULL_HANDLE};
    VkImageView m_dummyView{VK_NULL_HANDLE};
    VkBuffer m_dummyBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_dummyBufferMemory{VK_NULL_HANDLE};
    bool m_dummyImageTransitioned{false};
    bool createDummyResources();
    void destroyDummyResources();

    // Dimensions
    uint32_t m_width{0};
    uint32_t m_height{0};

    // Push constants (must stay <= 256 bytes — NVIDIA limit)
    struct LightingParams {
        glm::mat4 invViewProj;  // 64
        glm::mat4 view;         // 64  — for cascade depth selection
        glm::vec3 cameraPos;    // 12
        float padding1;         // 4
        glm::vec2 screenSize;   // 8
        uint32_t lightCount;    // 4
        uint32_t flags;         // 4  — Bit 0: IBL, Bit 1: SSAO, Bit 2: shadows, Bit 3: SSGI
        float wetness;          // 4  — 0=dry, 1=fully wet (rain accumulation)
        float paddingW;         // 4
    };                          // Total: 168 bytes

    LightingParams m_params{};
};

} // namespace ohao
