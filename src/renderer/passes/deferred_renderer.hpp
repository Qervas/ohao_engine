#pragma once

#include "gbuffer_pass.hpp"
#include "deferred_lighting_pass.hpp"
#include "csm_pass.hpp"
#include "post_processing_pipeline.hpp"
#include "utils/common_types.hpp"
#include <memory>
#include <unordered_map>

namespace ohao {

class Scene;

// Full deferred rendering pipeline that orchestrates all passes
// This is the main entry point for AAA-quality rendering
class DeferredRenderer {
public:
    DeferredRenderer() = default;
    ~DeferredRenderer();

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice);
    void cleanup();

    // Main render function - call this once per frame
    void render(VkCommandBuffer cmd, uint32_t frameIndex);

    // Resize handling
    void onResize(uint32_t width, uint32_t height);

    // Scene configuration
    void setScene(Scene* scene);

    // Geometry buffers (from OffscreenRenderer)
    void setGeometryBuffers(VkBuffer vertexBuffer, VkBuffer indexBuffer,
                            const std::unordered_map<uint64_t, MeshBufferInfo>* bufferMap);

    // Camera configuration
    void setCameraData(const glm::mat4& view, const glm::mat4& proj,
                       const glm::vec3& position, float nearPlane, float farPlane);

    // Light configuration
    void setDirectionalLight(const glm::vec3& direction, const glm::vec3& color, float intensity);
    void setLightBuffer(VkBuffer lightBuffer, uint32_t lightCount);

    // IBL configuration (optional)
    void setIBLTextures(VkImageView irradiance, VkImageView prefiltered,
                        VkImageView brdfLUT, VkSampler iblSampler);

    // Post-processing configuration
    PostProcessingPipeline* getPostProcessing() { return m_postProcessing.get(); }

    // Get final output for display/readback
    VkImageView getFinalOutput() const;
    VkImage getFinalOutputImage() const;

    // Get SSAO output for deferred lighting
    VkImageView getSSAOOutput() const;

    // Get jitter offset for TAA
    glm::vec2 getJitterOffset(uint32_t frameIndex) const;

private:
    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};

    // Render passes
    std::unique_ptr<GBufferPass> m_gbufferPass;
    std::unique_ptr<CSMPass> m_csmPass;
    std::unique_ptr<DeferredLightingPass> m_lightingPass;
    std::unique_ptr<PostProcessingPipeline> m_postProcessing;

    // Scene reference
    Scene* m_scene{nullptr};

    // Dimensions
    uint32_t m_width{0};
    uint32_t m_height{0};

    // Camera data
    glm::mat4 m_view;
    glm::mat4 m_proj;
    glm::mat4 m_prevViewProj;
    glm::vec3 m_cameraPos;
    float m_nearPlane{0.1f};
    float m_farPlane{1000.0f};

    // Light data
    glm::vec3 m_lightDirection{0.0f, -1.0f, 0.0f};
    VkBuffer m_lightBuffer{VK_NULL_HANDLE};
    uint32_t m_lightCount{0};
};

} // namespace ohao
