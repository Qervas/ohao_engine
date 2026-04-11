#pragma once

#include "render_pass_base.hpp"
#include "render/culling.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "core/common_types.hpp"
#include <array>
#include <unordered_map>

namespace ohao {

class Scene;

static constexpr uint32_t MAX_BONES = 128;

struct BoneMatrixUBO {
    glm::mat4 boneMatrices[MAX_BONES];
    int boneCount{0};
};

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

    // VkImage handles for render graph import
    VkImage getPositionImage() const { return m_gbuffer[0].image; }
    VkImage getNormalImage() const { return m_gbuffer[1].image; }
    VkImage getAlbedoImage() const { return m_gbuffer[2].image; }
    VkImage getVelocityImage() const { return m_gbuffer[3].image; }
    VkImage getDepthImage() const { return m_gbuffer[4].image; }

    // Format accessors for render graph import
    VkFormat getPositionFormat() const { return m_gbuffer[0].format; }
    VkFormat getNormalFormat() const { return m_gbuffer[1].format; }
    VkFormat getAlbedoFormat() const { return m_gbuffer[2].format; }
    VkFormat getVelocityFormat() const { return m_gbuffer[3].format; }
    VkFormat getDepthFormat() const { return m_gbuffer[4].format; }

    VkRenderPass getRenderPass() const { return m_renderPass; }
    VkFramebuffer getFramebuffer() const { return m_framebuffer; }

    // Texture manager for bindless textures
    void setTextureManager(BindlessTextureManager* texManager) { m_textureManager = texManager; }

    // Wireframe mode
    void setWireframeEnabled(bool enabled) { m_wireframeEnabled = enabled; }
    bool getWireframeEnabled() const { return m_wireframeEnabled; }

    // Descriptor set for deferred lighting
    VkDescriptorSetLayout getGBufferLayout() const { return m_gbufferLayout; }
    VkDescriptorSet getGBufferDescriptor() const { return m_gbufferDescriptor; }

    // Upload bone matrices for a specific animated actor
    void uploadBoneMatrices(const std::vector<glm::mat4>& matrices);

private:
    bool createRenderPass();
    bool createFramebuffer();
    bool createPipeline();
    bool createSkinnedPipeline();
    bool createGBuffer();
    bool createDescriptors();
    bool createBoneMatrixResources();
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

    // Skinned pipeline (animated meshes)
    VkPipeline m_skinnedPipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_skinnedPipelineLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_boneDescriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_boneDescriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_boneDescriptorSet{VK_NULL_HANDLE};
    VkBuffer m_boneMatrixBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_boneMatrixMemory{VK_NULL_HANDLE};
    void* m_boneMatrixMapped{nullptr};

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
        glm::mat4 viewProj;       // precomputed projection * view
        glm::mat4 prevMVP;
        glm::vec4 materialParams;  // x=metallic, y=roughness, z=ao, w=albedoTexIdx (uint bits)
        glm::vec4 albedoColor;     // rgb=albedo, a=normalTexIdx (uint bits)
    };

    glm::mat4 m_view;
    glm::mat4 m_projection;
    glm::mat4 m_prevViewProj;

    // Wireframe mode
    bool m_wireframeEnabled{false};

    // Bindless texture manager
    BindlessTextureManager* m_textureManager{nullptr};
};

} // namespace ohao
