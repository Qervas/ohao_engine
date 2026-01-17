#pragma once

#include "render_pass_base.hpp"

namespace ohao {

// Screen-Space Reflections using hierarchical ray marching
class SSRPass : public RenderPassBase {
public:
    SSRPass() = default;
    ~SSRPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "SSRPass"; }

    // G-Buffer inputs
    void setDepthBuffer(VkImageView depth);
    void setNormalBuffer(VkImageView normal);
    void setColorBuffer(VkImageView color);  // Scene color for reflection sampling
    void setRoughnessBuffer(VkImageView roughness);  // Optional: for roughness-based blur

    // Matrices
    void setMatrices(const glm::mat4& view, const glm::mat4& proj,
                     const glm::mat4& invView, const glm::mat4& invProj);

    void updateDescriptorSet();

    // Configuration
    void setMaxDistance(float dist) { m_maxDistance = dist; }
    void setThickness(float thickness) { m_thickness = thickness; }
    void setMaxSteps(uint32_t steps) { m_maxSteps = steps; }
    void setBinarySearchSteps(uint32_t steps) { m_binarySearchSteps = steps; }
    void setRoughnessFade(float fade) { m_roughnessFade = fade; }
    void setEdgeFade(float fade) { m_edgeFade = fade; }

    // Output
    VkImageView getReflectionView() const { return m_reflectionView; }
    VkSampler getSampler() const { return m_sampler; }

private:
    bool createOutputImage();
    bool createHiZBuffer();
    bool createDescriptors();
    bool createPipeline();
    bool createHiZPipeline();
    void destroyOutputImage();
    void destroyHiZBuffer();
    void generateHiZ(VkCommandBuffer cmd);

    // Input views
    VkImageView m_depthView{VK_NULL_HANDLE};
    VkImageView m_normalView{VK_NULL_HANDLE};
    VkImageView m_colorView{VK_NULL_HANDLE};
    VkImageView m_roughnessView{VK_NULL_HANDLE};

    // Reflection output
    VkImage m_reflectionImage{VK_NULL_HANDLE};
    VkDeviceMemory m_reflectionMemory{VK_NULL_HANDLE};
    VkImageView m_reflectionView{VK_NULL_HANDLE};

    // Hi-Z buffer (depth pyramid for hierarchical tracing)
    VkImage m_hizImage{VK_NULL_HANDLE};
    VkDeviceMemory m_hizMemory{VK_NULL_HANDLE};
    VkImageView m_hizView{VK_NULL_HANDLE};
    std::vector<VkImageView> m_hizMipViews;
    uint32_t m_hizMipLevels{0};

    // Ray march pipeline (compute)
    VkPipeline m_ssrPipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_ssrPipelineLayout{VK_NULL_HANDLE};

    // Hi-Z generation pipeline (compute)
    VkPipeline m_hizPipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_hizPipelineLayout{VK_NULL_HANDLE};

    // Descriptors
    VkDescriptorSetLayout m_ssrDescriptorLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_hizDescriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_ssrDescriptorSet{VK_NULL_HANDLE};
    std::vector<VkDescriptorSet> m_hizDescriptorSets;

    // Samplers
    VkSampler m_sampler{VK_NULL_HANDLE};          // Linear sampler for color
    VkSampler m_pointSampler{VK_NULL_HANDLE};     // Point sampler for depth/hi-z

    // Dimensions
    uint32_t m_width{0};
    uint32_t m_height{0};

    // Matrices
    glm::mat4 m_view;
    glm::mat4 m_projection;
    glm::mat4 m_invView;
    glm::mat4 m_invProjection;

    // Ray march parameters
    float m_maxDistance{100.0f};      // Max ray distance in world units
    float m_thickness{0.1f};          // Depth thickness for hit detection
    uint32_t m_maxSteps{64};          // Max ray march steps
    uint32_t m_binarySearchSteps{8};  // Binary search refinement steps
    float m_roughnessFade{0.5f};      // Fade reflections based on roughness
    float m_edgeFade{0.1f};           // Fade at screen edges

    // Push constants for SSR compute shader
    struct SSRParams {
        glm::mat4 view;
        glm::mat4 projection;
        glm::mat4 invView;
        glm::mat4 invProjection;
        glm::vec4 screenSize;     // xy = size, zw = 1/size
        float maxDistance;
        float thickness;
        float roughnessFade;
        float edgeFade;
        uint32_t maxSteps;
        uint32_t binarySearchSteps;
        uint32_t hizMipLevels;
        uint32_t padding;
    };

    // Push constants for Hi-Z generation
    struct HiZParams {
        glm::uvec2 srcSize;
        glm::uvec2 dstSize;
        uint32_t srcMip;
        uint32_t padding[3];
    };
};

} // namespace ohao
