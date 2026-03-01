#pragma once

#include "render_pass_base.hpp"

namespace ohao {

// Physical sky pass — Preetham analytical daylight model.
// Runs after deferred lighting (LOAD_OP_LOAD on HDR output).
// Fills only sky pixels (GBuffer depth == 1.0) with sky radiance.
class SkyPass : public RenderPassBase {
public:
    SkyPass() = default;
    ~SkyPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "SkyPass"; }

    // Sky parameters
    void setSunDirection(const glm::vec3& dir)  { m_sunDirection = glm::normalize(dir); }
    void setTurbidity(float t)                  { m_turbidity = glm::clamp(t, 1.0f, 10.0f); }
    void setSunIntensity(float i)               { m_sunIntensity = i; }
    void setGroundColor(const glm::vec3& c)     { m_groundColor = c; }
    void setEnabled(bool e)                     { m_enabled = e; }
    bool getEnabled() const                     { return m_enabled; }

    // Camera data for view-direction reconstruction
    void setCameraData(const glm::mat4& invViewProj, const glm::vec3& cameraPos);

    // Input: GBuffer depth buffer (to identify sky pixels)
    void setDepthBuffer(VkImageView depth);

    // Output: HDR lighting buffer (LOAD_OP_LOAD — sky fills empty pixels)
    // Must be called after lighting pass is initialized and after each resize.
    void setHDROutput(VkImageView view, VkImage image);

private:
    bool createRenderPass();
    bool createFramebuffer();
    bool createDescriptors();
    bool createPipeline();
    bool createSampler();
    void destroyFramebuffer();
    void updateDescriptors();

    // Render pass (LOAD_OP_LOAD preserves lit geometry pixels)
    VkRenderPass m_renderPass{VK_NULL_HANDLE};
    VkFramebuffer m_framebuffer{VK_NULL_HANDLE};

    // Pipeline
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Descriptors
    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_descriptorSet{VK_NULL_HANDLE};
    VkSampler m_sampler{VK_NULL_HANDLE};

    // External resources (not owned)
    VkImageView m_depthView{VK_NULL_HANDLE};
    VkImageView m_hdrView{VK_NULL_HANDLE};
    VkImage     m_hdrImage{VK_NULL_HANDLE};

    // Dimensions
    uint32_t m_width{0};
    uint32_t m_height{0};

    // Sky parameters
    glm::vec3 m_sunDirection{0.3f, 0.9f, 0.3f};  // normalized toward sun
    float     m_turbidity{2.5f};
    float     m_sunIntensity{1.0f};
    glm::vec3 m_groundColor{0.08f, 0.07f, 0.06f};
    bool      m_enabled{true};

    // Camera
    glm::mat4 m_invViewProj{1.0f};
    glm::vec3 m_cameraPos{0.0f};

    // Push constants (112 bytes, matches sky.frag layout)
    struct SkyParams {
        glm::mat4 invViewProj;   // 64
        glm::vec3 sunDirection;  // 12
        float     turbidity;     //  4  → 80
        glm::vec3 cameraPos;     // 12
        float     sunIntensity;  //  4  → 96
        glm::vec3 groundColor;   // 12
        float     pad0;          //  4  → 112
    };
};

} // namespace ohao
