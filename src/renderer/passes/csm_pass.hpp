#pragma once

#include "render_pass_base.hpp"
#include "utils/common_types.hpp"
#include <array>
#include <unordered_map>

namespace ohao {

class Scene;

// Cascaded Shadow Map pass for directional light shadows
class CSMPass : public RenderPassBase {
public:
    static constexpr uint32_t CASCADE_COUNT = 4;
    static constexpr uint32_t SHADOW_MAP_SIZE = 2048;

    CSMPass() = default;
    ~CSMPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    const char* getName() const override { return "CSMPass"; }

    // Configuration
    void setScene(Scene* scene) { m_scene = scene; }
    void setLightDirection(const glm::vec3& direction) { m_lightDirection = glm::normalize(direction); }
    void setCameraData(const glm::mat4& view, const glm::mat4& proj,
                       float nearPlane, float farPlane);
    void setGeometryBuffers(VkBuffer vertexBuffer, VkBuffer indexBuffer) {
        m_vertexBuffer = vertexBuffer;
        m_indexBuffer = indexBuffer;
    }
    void setMeshBufferMap(const std::unordered_map<uint64_t, MeshBufferInfo>* bufferMap) {
        m_meshBufferMap = bufferMap;
    }

    // Split scheme: 0.0 = linear, 1.0 = logarithmic
    void setSplitLambda(float lambda) { m_splitLambda = lambda; }

    // Get shadow map for sampling
    VkImageView getShadowMapArrayView() const { return m_shadowMapArrayView; }
    VkSampler getShadowSampler() const { return m_shadowSampler; }

    // Get cascade data for shader
    const CascadeData& getCascadeData() const { return m_cascadeData; }
    VkBuffer getCascadeBuffer() const { return m_cascadeBuffer; }

private:
    bool createShadowMap();
    bool createRenderPass();
    bool createFramebuffers();
    bool createPipeline();
    bool createCascadeBuffer();

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

    // Scene reference
    Scene* m_scene{nullptr};

    // Geometry buffers (from OffscreenRenderer)
    VkBuffer m_vertexBuffer{VK_NULL_HANDLE};
    VkBuffer m_indexBuffer{VK_NULL_HANDLE};
    const std::unordered_map<uint64_t, MeshBufferInfo>* m_meshBufferMap{nullptr};

    // Light and camera data
    glm::vec3 m_lightDirection{0.0f, -1.0f, 0.0f};
    glm::mat4 m_cameraView;
    glm::mat4 m_cameraProj;
    float m_nearPlane{0.1f};
    float m_farPlane{1000.0f};

    // Cascade configuration
    float m_splitLambda{0.95f}; // 0.95 = mostly logarithmic
    std::array<float, CASCADE_COUNT + 1> m_cascadeSplits{};

    // Cascade data for shader
    CascadeData m_cascadeData{};

    // Push constant for shadow rendering
    struct ShadowPushConstant {
        glm::mat4 model;
        uint32_t cascadeIndex;
        float padding[3];
    };
};

} // namespace ohao
