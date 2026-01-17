#pragma once

#include "render_pass_base.hpp"
#include "utils/common_types.hpp"
#include <array>
#include <unordered_map>

namespace ohao {

class Scene;

// G-Buffer generation pass
// Outputs: Position, Normal, Albedo, Velocity, Depth
class GBufferPass : public RenderPassBase {
public:
    GBufferPass() = default;
    ~GBufferPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "GBufferPass"; }

    // Setup for rendering
    void setScene(Scene* scene) { m_scene = scene; }
    void setViewProjection(const glm::mat4& view, const glm::mat4& proj,
                          const glm::mat4& prevViewProj);
    void setGeometryBuffers(VkBuffer vertexBuffer, VkBuffer indexBuffer);
    void setMeshBufferMap(const std::unordered_map<uint64_t, struct MeshBufferInfo>* bufferMap) {
        m_meshBufferMap = bufferMap;
    }

    // Get G-Buffer attachments for use by other passes
    VkImageView getPositionView() const { return m_gbuffer[0].view; }
    VkImageView getNormalView() const { return m_gbuffer[1].view; }
    VkImageView getAlbedoView() const { return m_gbuffer[2].view; }
    VkImageView getVelocityView() const { return m_gbuffer[3].view; }
    VkImageView getDepthView() const { return m_gbuffer[4].view; }

    VkRenderPass getRenderPass() const { return m_renderPass; }
    VkFramebuffer getFramebuffer() const { return m_framebuffer; }

    // Descriptor set for deferred lighting
    VkDescriptorSetLayout getGBufferLayout() const { return m_gbufferLayout; }
    VkDescriptorSet getGBufferDescriptor() const { return m_gbufferDescriptor; }

private:
    bool createRenderPass();
    bool createFramebuffer();
    bool createPipeline();
    bool createGBuffer();
    bool createDescriptors();
    void destroyGBuffer();

    // G-Buffer render targets
    static constexpr uint32_t GBUFFER_COUNT = 5;
    std::array<RenderTarget, GBUFFER_COUNT> m_gbuffer;

    // Render pass and framebuffer
    VkRenderPass m_renderPass{VK_NULL_HANDLE};
    VkFramebuffer m_framebuffer{VK_NULL_HANDLE};

    // Pipeline
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Descriptors for G-Buffer access
    VkDescriptorSetLayout m_gbufferLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_gbufferDescriptor{VK_NULL_HANDLE};

    // Sampler for G-Buffer
    VkSampler m_sampler{VK_NULL_HANDLE};

    // Scene reference
    Scene* m_scene{nullptr};

    // Geometry buffers (from OffscreenRenderer)
    VkBuffer m_vertexBuffer{VK_NULL_HANDLE};
    VkBuffer m_indexBuffer{VK_NULL_HANDLE};
    const std::unordered_map<uint64_t, MeshBufferInfo>* m_meshBufferMap{nullptr};

    // Dimensions
    uint32_t m_width{0};
    uint32_t m_height{0};

    // Push constant data for G-Buffer rendering
    struct GBufferUBO {
        glm::mat4 model;
        glm::mat4 view;
        glm::mat4 projection;
        glm::mat4 prevMVP;
        glm::vec4 materialParams;  // x=metallic, y=roughness, z=ao, w=unused
        glm::vec4 albedoColor;     // rgb=albedo, a=unused
    };

    glm::mat4 m_view;
    glm::mat4 m_projection;
    glm::mat4 m_prevViewProj;
};

} // namespace ohao
