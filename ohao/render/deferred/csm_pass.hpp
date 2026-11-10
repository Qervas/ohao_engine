#pragma once

#include "render_pass_base.hpp"
#include "render/shader_bindings/shadow_shadow_csm.hpp"
#include "render/frame/frame_resources.hpp"
#include <array>

namespace ohao::render { class RenderScene; }

namespace ohao {

class GpuCulling;

// Cascaded Shadow Map pass for directional light shadows: the fallback without RT shadows.
// GPU-culled (sub-project 2b spec, "GPU-driven geometry"): each cascade culls the draw table to
// its frustum's sides (depth is clamped) and draws indirectly. Set 0 = the instance table and
// the draw table (one set per frame slot), push constant = the cascade's view-projection.
class CSMPass : public RenderPassBase {
public:
    static constexpr uint32_t CASCADE_COUNT = 4;        // at most (the array's layers, the lists)
    static constexpr uint32_t SHADOW_MAP_SIZE = 2048;   // the default size of one cascade

    CSMPass() = default;
    ~CSMPass() override;

    [[nodiscard]] bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    [[nodiscard]] const char* getName() const override { return "CSMPass"; }

    // Configuration
    void setRenderScene(const render::RenderScene* scene) { m_renderScene = scene; }
    void setCulling(GpuCulling* culling) { m_culling = culling; }
    void setLightDirection(const glm::vec3& direction) { m_lightDirection = glm::normalize(direction); }
    void setCameraData(const glm::mat4& view, const glm::mat4& proj,
                       float nearPlane, float farPlane);

    // Split scheme: 0.0 = linear, 1.0 = logarithmic
    void setSplitLambda(float lambda) { m_splitLambda = lambda; }

    // render.settings csm: the cascades drawn (1-CASCADE_COUNT) and each one's size (512-4096).
    // A new size idles the device and rebuilds the shadow map: its views change, so the caller
    // rebinds them (returns true then). Between frames.
    bool configure(uint32_t cascades, uint32_t size);
    [[nodiscard]] uint32_t cascades() const noexcept { return m_cascades; }
    [[nodiscard]] uint32_t size() const noexcept { return m_size; }

    // Get shadow map for sampling
    [[nodiscard]] VkImageView getShadowMapArrayView() const { return m_shadowMapArrayView; }
    [[nodiscard]] VkSampler getShadowSampler() const { return m_shadowSampler; }

    // VkImage handle for render graph import
    [[nodiscard]] VkImage getShadowMapImage() const { return m_shadowMap; }

    // Get cascade data for shader
    [[nodiscard]] const CascadeData& getCascadeData() const { return m_cascadeData; }
    [[nodiscard]] VkBuffer getCascadeBuffer() const { return m_cascadeBuffer; }

private:
    [[nodiscard]] bool createShadowMap();
    [[nodiscard]] bool createRenderPass();
    [[nodiscard]] bool createFramebuffers();
    [[nodiscard]] bool createPipeline();
    [[nodiscard]] bool createCascadeBuffer();
    [[nodiscard]] bool createSceneDescriptors();

    void calculateCascadeSplits();
    void updateCascadeMatrices();
    glm::mat4 calculateLightViewProj(uint32_t cascade);

    // Shadow map array (one layer per cascade)
    VkImage m_shadowMap{VK_NULL_HANDLE};
    VkDeviceMemory m_shadowMapMemory{VK_NULL_HANDLE};
    VkImageView m_shadowMapArrayView{VK_NULL_HANDLE};
    std::array<VkImageView, CASCADE_COUNT> m_cascadeViews{};
    std::array<VkFramebuffer, CASCADE_COUNT> m_framebuffers{};

    // Render pass and pipeline
    VkRenderPass m_renderPass{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Shadow sampler
    VkSampler m_shadowSampler{VK_NULL_HANDLE};

    // Cascade data buffer
    VkBuffer m_cascadeBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_cascadeBufferMemory{VK_NULL_HANDLE};
    void* m_cascadeBufferMapped{nullptr};

    // Scene the pass draws (owned by VulkanRenderer)
    const render::RenderScene* m_renderScene{nullptr};
    GpuCulling* m_culling{nullptr};

    // Set 0: the instance table and the draw table, one set per frame slot, rewritten in
    // execute(cmd, slot)
    VkDescriptorSetLayout m_sceneLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_scenePool{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_sceneSets{};

    // Light and camera data
    glm::vec3 m_lightDirection{0.0f, -1.0f, 0.0f};
    glm::mat4 m_cameraView;
    glm::mat4 m_cameraProj;
    float m_nearPlane{0.1f};
    float m_farPlane{1000.0f};

    // Cascade configuration
    float m_splitLambda{0.95f}; // 0.95 = mostly logarithmic
    uint32_t m_cascades{CASCADE_COUNT};
    uint32_t m_size{SHADOW_MAP_SIZE};
    void destroyShadowMap();   // the image, its views, the framebuffers and the sampler
    std::array<float, CASCADE_COUNT + 1> m_cascadeSplits{};

    // Cascade data for shader
    CascadeData m_cascadeData{};

    // Push constant for one cascade (matches shadow/shadow_csm.slang).
    struct ShadowPushConstant {
        glm::mat4 viewProj;   // the cascade being rendered
    };
    OHAO_CHECK_PUSH(ShadowPushConstant, shadow_shadow_csm)   // shadow/shadow_csm.slang's
    static_assert(sizeof(ShadowPushConstant) == 64);
};

} // namespace ohao
