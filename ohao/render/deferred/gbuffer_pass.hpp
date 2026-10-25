#pragma once

#include "render_pass_base.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "render/frame/frame_resources.hpp"
#include "render/rt/rt_compute.hpp"
#include <array>

namespace ohao::render { class RenderScene; class GpuPassTimer; }

namespace ohao {

class GpuCulling;

// G-Buffer generation pass
// Outputs: Position, Normal, Albedo, Velocity, Depth
// GPU-driven (sub-project 2b spec, "GPU-driven geometry"), in five timed parts: cull.instances
// (last frame's visible draws in the frustum), gbuffer.early (draws them, clearing the
// targets), hiz.build (the depth pyramid), cull.late (every draw against the frustum and the
// pyramid), gbuffer.late (draws the newly visible, loading the targets). Draws are
// vkCmdDrawIndexedIndirectCount over GpuCulling's lists, one-sided materials with back faces
// culled. firstInstance indexes RenderScene's draw table; set 0 = bindless textures, set 1
// "scene" = {instances, materials, material refs, camera UBO, SSS tiles, draw table}, one set
// per frame slot.
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
    void setRenderScene(const render::RenderScene* scene) { m_renderScene = scene; }
    void setCulling(GpuCulling* culling) { m_culling = culling; }
    // Times the five parts (the render graph does not time this pass).
    void setPassTimer(render::GpuPassTimer* timer) { m_passTimer = timer; }
    void setViewProjection(const glm::mat4& view, const glm::mat4& proj,
                          const glm::mat4& prevViewProj);

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

    // Texture manager for bindless textures
    void setTextureManager(BindlessTextureManager* texManager) { m_textureManager = texManager; }

    // Wireframe mode
    void setWireframeEnabled(bool enabled) { m_wireframeEnabled = enabled; }
    [[nodiscard]] bool getWireframeEnabled() const { return m_wireframeEnabled; }

    // Descriptor set for deferred lighting
    [[nodiscard]] VkDescriptorSetLayout getGBufferLayout() const { return m_gbufferLayout; }
    [[nodiscard]] VkDescriptorSet getGBufferDescriptor() const { return m_gbufferDescriptor; }

    // SSS classification (layout.json, kSSSHeaderWords): the pass resets it before drawing and
    // every visible fragment of a subsurface material flags its 16x16 tile; SSSPass dispatches
    // over the lists (indirect). Host-mapped: the tiles flagged by the last frame whose fence
    // has been waited on.
    [[nodiscard]] VkBuffer sssTileBuffer() const { return m_sssTiles.buffer; }
    [[nodiscard]] VkExtent2D sssTiles() const;
    [[nodiscard]] uint32_t sssTilesFlagged() const;

private:
    [[nodiscard]] bool createRenderPass();
    [[nodiscard]] bool createFramebuffer();
    [[nodiscard]] bool createPipeline();
    [[nodiscard]] bool createGBuffer();
    [[nodiscard]] bool createDescriptors();
    [[nodiscard]] bool createSceneDescriptors();
    void destroySceneDescriptors();
    void destroyGBuffer();
    // Rewrites the frame slot's scene set and camera UBO. False when the scene has
    // no buffers to bind.
    [[nodiscard]] bool updateSceneSet(uint32_t frameIndex);
    // One of the two render passes (`late`: loads what the early one drew), drawing list pair
    // `pair` when `draw`.
    void drawPass(VkCommandBuffer cmd, uint32_t frameIndex, bool late, uint32_t pair, bool draw);
    void timerBegin(VkCommandBuffer cmd, const char* name);
    void timerEnd(VkCommandBuffer cmd);

    // G-Buffer render targets
    static constexpr uint32_t GBUFFER_COUNT = 5;
    std::array<RenderTarget, GBUFFER_COUNT> m_gbuffer;

    // The early render pass clears and leaves the targets for the late one (depth readable by
    // the Hi-Z build); the late one loads them and leaves them for the lighting. They are
    // compatible: one framebuffer, one set of pipelines.
    VkRenderPass m_renderPass{VK_NULL_HANDLE};
    VkRenderPass m_lateRenderPass{VK_NULL_HANDLE};
    VkFramebuffer m_framebuffer{VK_NULL_HANDLE};

    // Pipelines: double-sided (no face culling), one-sided (back faces culled), wireframe
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    VkPipeline m_cullBackPipeline{VK_NULL_HANDLE};
    VkPipeline m_wireframePipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    GpuCulling* m_culling{nullptr};
    render::GpuPassTimer* m_passTimer{nullptr};

    // Descriptors for G-Buffer access
    VkDescriptorSetLayout m_gbufferLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet m_gbufferDescriptor{VK_NULL_HANDLE};

    // Sampler for G-Buffer
    VkSampler m_sampler{VK_NULL_HANDLE};

    // Scene the pass draws (owned by VulkanRenderer)
    const render::RenderScene* m_renderScene{nullptr};

    // Set 1 "scene": layout, pool, one set + camera UBO per frame slot. The UBOs are
    // host-visible and persistently mapped; a slot's UBO and set are rewritten only in
    // execute(cmd, slot), after that slot's fence wait.
    struct CameraUBO {
        glm::mat4 viewProj;
        glm::mat4 prevViewProj;
    };
    static_assert(sizeof(CameraUBO) == 128);
    VkDescriptorSetLayout m_sceneLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_emptyLayout{VK_NULL_HANDLE};   // set 0 stand-in without bindless textures
    VkDescriptorPool m_scenePool{VK_NULL_HANDLE};
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> m_sceneSets{};
    std::array<VkBuffer, MAX_FRAMES_IN_FLIGHT> m_cameraBuffers{};
    std::array<VkDeviceMemory, MAX_FRAMES_IN_FLIGHT> m_cameraMemory{};
    std::array<void*, MAX_FRAMES_IN_FLIGHT> m_cameraMapped{};
    rtc::HostBuffer m_sssTiles{};   // set 1, binding 4
    void resetSSSTiles(VkCommandBuffer cmd);

    // Dimensions
    uint32_t m_width{0};
    uint32_t m_height{0};

    // Push constant: the SSS tiles across and down (matches gbuffer.frag; the material slot
    // comes from the draw record).
    struct DrawPushConstant {
        uint32_t tilesX, tilesY;
        uint32_t pad0, pad1;
    };
    static_assert(sizeof(DrawPushConstant) == 16);

    glm::mat4 m_view{1.0f};
    glm::mat4 m_projection{1.0f};
    glm::mat4 m_prevViewProj{1.0f};

    // Wireframe mode
    bool m_wireframeEnabled{false};

    // Bindless texture manager
    BindlessTextureManager* m_textureManager{nullptr};
};

} // namespace ohao
