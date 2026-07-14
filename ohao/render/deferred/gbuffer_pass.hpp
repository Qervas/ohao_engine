#pragma once

#include "render_pass_base.hpp"
#include "render/culling.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "core/common_types.hpp"
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

    [[nodiscard]] bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    [[nodiscard]] const char* getName() const override { return "GBufferPass"; }

    // Setup for rendering
    void setScene(Scene* scene) { m_scene = scene; }
    void setViewProjection(const glm::mat4& view, const glm::mat4& proj,
                          const glm::mat4& prevViewProj);
    void setGeometryBuffers(VkBuffer vertexBuffer, VkBuffer indexBuffer);
    void setMeshBufferMap(const std::unordered_map<uint64_t, struct MeshBufferInfo>* bufferMap) {
        m_meshBufferMap = bufferMap;
    }

    // Get G-Buffer attachments for use by other passes
    [[nodiscard]] VkImageView getPositionView() const { return m_gbuffer[0].view; }
    [[nodiscard]] VkImageView getNormalView() const { return m_gbuffer[1].view; }
    [[nodiscard]] VkImageView getAlbedoView() const { return m_gbuffer[2].view; }
    [[nodiscard]] VkImageView getVelocityView() const { return m_gbuffer[3].view; }
    [[nodiscard]] VkImageView getDepthView() const { return m_gbuffer[4].view; }

    // VkImage handles for render graph import
    [[nodiscard]] VkImage getPositionImage() const { return m_gbuffer[0].image; }
    [[nodiscard]] VkImage getNormalImage() const { return m_gbuffer[1].image; }
    [[nodiscard]] VkImage getAlbedoImage() const { return m_gbuffer[2].image; }
    [[nodiscard]] VkImage getVelocityImage() const { return m_gbuffer[3].image; }
    [[nodiscard]] VkImage getDepthImage() const { return m_gbuffer[4].image; }

    // Format accessors for render graph import
    [[nodiscard]] VkFormat getPositionFormat() const { return m_gbuffer[0].format; }
    [[nodiscard]] VkFormat getNormalFormat() const { return m_gbuffer[1].format; }
    [[nodiscard]] VkFormat getAlbedoFormat() const { return m_gbuffer[2].format; }
    [[nodiscard]] VkFormat getVelocityFormat() const { return m_gbuffer[3].format; }
    [[nodiscard]] VkFormat getDepthFormat() const { return m_gbuffer[4].format; }

    [[nodiscard]] VkRenderPass getRenderPass() const { return m_renderPass; }
    [[nodiscard]] VkFramebuffer getFramebuffer() const { return m_framebuffer; }

    // Texture manager for bindless textures
    void setTextureManager(BindlessTextureManager* texManager) { m_textureManager = texManager; }

    // Wireframe mode
    void setWireframeEnabled(bool enabled) { m_wireframeEnabled = enabled; }
    [[nodiscard]] bool getWireframeEnabled() const { return m_wireframeEnabled; }

    // Descriptor set for deferred lighting
    [[nodiscard]] VkDescriptorSetLayout getGBufferLayout() const { return m_gbufferLayout; }
    [[nodiscard]] VkDescriptorSet getGBufferDescriptor() const { return m_gbufferDescriptor; }

private:
    [[nodiscard]] bool createRenderPass();
    [[nodiscard]] bool createFramebuffer();
    [[nodiscard]] bool createPipeline();
    [[nodiscard]] bool createGBuffer();
    [[nodiscard]] bool createDescriptors();
    void destroyGBuffer();

    // G-Buffer render targets
    static constexpr uint32_t GBUFFER_COUNT = 5;
    std::array<RenderTarget, GBUFFER_COUNT> m_gbuffer;

    // Render pass and framebuffer
    VkRenderPass m_renderPass{VK_NULL_HANDLE};
    VkFramebuffer m_framebuffer{VK_NULL_HANDLE};

    // Pipeline (static meshes)
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipeline m_wireframePipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Descriptors for G-Buffer access
    VkDescriptorSetLayout m_gbufferLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_gbufferDescriptor{VK_NULL_HANDLE};

    // Sampler for G-Buffer
    VkSampler m_sampler{VK_NULL_HANDLE};

    // Scene reference
    Scene* m_scene{nullptr};

    // Geometry buffers (from VulkanRenderer)
    VkBuffer m_vertexBuffer{VK_NULL_HANDLE};
    VkBuffer m_indexBuffer{VK_NULL_HANDLE};
    const std::unordered_map<uint64_t, MeshBufferInfo>* m_meshBufferMap{nullptr};

    // Dimensions
    uint32_t m_width{0};
    uint32_t m_height{0};

    // Push constant data for G-Buffer rendering
    // Total: 224 bytes (3 mat4 + 2 vec4) — fits within 256-byte NVIDIA limit
    struct GBufferUBO {
        glm::mat4 model;
        glm::mat4 viewProj;
        glm::mat4 prevMVP;
        glm::vec4 materialParams;  // x=metallic, y=roughness, z=roughMetalTexIdx, w=albedoTexIdx
        glm::vec4 albedoColor;     // rgb=albedo, a=normalTexIdx
        glm::vec4 emissiveParams;  // x=emissiveTexIdx, y=emissiveStrength, z=unused, w=unused
    };  // 240 bytes — fits in 256-byte push constant limit

    glm::mat4 m_view;
    glm::mat4 m_projection;
    glm::mat4 m_prevViewProj;

    // Wireframe mode
    bool m_wireframeEnabled{false};

    // Bindless texture manager
    BindlessTextureManager* m_textureManager{nullptr};
};

} // namespace ohao
