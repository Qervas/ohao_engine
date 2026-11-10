#pragma once

#include "render_pass_base.hpp"
#include "render/shader_bindings/core_gbuffer.hpp"
#include "gpu/vulkan/bindless_texture_manager.hpp"
#include "render/frame/frame_resources.hpp"
#include "render/rt/rt_compute.hpp"
#include <array>

namespace ohao::render { class RenderScene; class GpuPassTimer; }

namespace ohao {

class GpuCulling;

// G-Buffer generation pass
// Outputs: Position, Normal, Albedo, Velocity, Depth
// GPU-driven (sub-project 2b spec, "GPU-driven geometry", "Meshlets and mesh shaders"), in
// five timed parts: cull.instances (last frame's visible draws in the frustum), gbuffer.early
// (draws them, clearing the targets), hiz.build (the depth pyramid), cull.late (every draw
// against the frustum and the pyramid), gbuffer.late (draws the newly visible, loading the
// targets). Each part draws GpuCulling's four lists of its phase: instance draws with
// vkCmdDrawIndexedIndirectCount (core/gbuffer.slang; firstInstance indexes RenderScene's draw
// table), then meshlet draws with task and mesh shaders (cull/meshlets.slang) or, without
// them, the fallback's per-meshlet draws through its pull vertex shader; one-sided materials
// with back faces culled. The late part then draws the alpha-tested (MASK) draws, which no
// list holds, one by one through core/gbuffer.slang's `masked`. Set 0 = bindless textures, set 1 "scene" = {instances, materials,
// material refs, camera UBO, SSS tiles, draw table, vertex arena, meshlets, meshlet vertices,
// meshlet triangles, culling commands, Hi-Z pyramid}, one set per frame slot.
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
    // VK_EXT_mesh_shader is enabled (before initialize): the mesh-shader pipelines are built.
    void setMeshShaderSupported(bool on) { m_meshShaderSupported = on; }
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
    // Emitted radiance (rgb: the emissive texture times the material's emissive strength,
    // linear, RGBA16F; 0 where nothing emits): lighting adds it to the lit image.
    [[nodiscard]] VkImageView getEmissiveView() const { return m_gbuffer[kEmissive].view; }
    // OpenPBR's per-pixel inputs beyond albedo, roughness and metalness (core/deferred_lighting.slang).
    [[nodiscard]] VkImageView getSpecularView() const { return m_gbuffer[kSpecular].view; }
    [[nodiscard]] VkImageView getOpenPBRView() const { return m_gbuffer[kOpenPBR].view; }
    [[nodiscard]] VkImageView getLayersView() const { return m_gbuffer[kLayers].view; }
    [[nodiscard]] const glm::mat4& projection() const { return m_projection; }
    [[nodiscard]] VkBuffer materialBuffer() const;   // RenderScene's material table (null without a scene)

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

    // SSS classification (layout.json, kSSSHeaderWords): the pass resets it before drawing and
    // every visible fragment of a subsurface material flags its 16x16 tile; SSSPass dispatches
    // over the lists (indirect). Host-mapped: the tiles flagged by the last frame whose fence
    // has been waited on.
    // The scene set (set 1 of core/gbuffer.slang: instances, materials, refs, camera UBO, ..., draw
    // table) and its layout, for the transparent pass's blended meshes, which reuse gbuffer.slang. A
    // slot's set is written by this frame's execute().
    [[nodiscard]] VkDescriptorSetLayout sceneSetLayout() const { return m_sceneLayout; }
    [[nodiscard]] VkDescriptorSet sceneSet(uint32_t slot) const {
        return slot < m_sceneSets.size() ? m_sceneSets[slot] : VK_NULL_HANDLE;
    }

    [[nodiscard]] VkBuffer sssTileBuffer() const { return m_sssTiles.buffer; }
    [[nodiscard]] VkExtent2D sssTiles() const;
    [[nodiscard]] uint32_t sssTilesFlagged() const;

private:
    [[nodiscard]] bool createRenderPass();
    [[nodiscard]] bool createFramebuffer();
    [[nodiscard]] bool createPipeline();
    [[nodiscard]] bool createGBuffer();
    [[nodiscard]] bool createSceneDescriptors();
    static constexpr uint32_t kSceneBindings = 12;   // set 1 (gbuffer_pass.cpp's kScene)
    void destroySceneDescriptors();
    void destroyGBuffer();
    // Rewrites the frame slot's scene set and camera UBO. False when the scene has
    // no buffers to bind.
    [[nodiscard]] bool updateSceneSet(uint32_t frameIndex);
    [[nodiscard]] bool createMeshletPipelines(VkGraphicsPipelineCreateInfo base, VkPipelineRasterizationStateCreateInfo raster);
    // One of the two render passes (`late`: loads what the early one drew), drawing its phase's
    // lists when `draw`.
    void drawPass(VkCommandBuffer cmd, uint32_t frameIndex, bool late, bool draw);
    void timerBegin(VkCommandBuffer cmd, const char* name);
    void timerEnd(VkCommandBuffer cmd);

    // G-Buffer render targets: position, normal, albedo, velocity, depth (4, the depth
    // attachment), emissive (5, the fragment shader's output 4), OpenPBR's specular (6, output 5:
    // specular_color, specular_weight / 1.5) and base (7, output 6: base_diffuse_roughness,
    // (specular_ior - 1) / 2.55)
    static constexpr uint32_t GBUFFER_COUNT = 9;
    static constexpr uint32_t kDepth = 4;
    static constexpr uint32_t kEmissive = 5;
    static constexpr uint32_t kSpecular = 6;
    static constexpr uint32_t kOpenPBR = 7;
    static constexpr uint32_t kLayers = 8;   // OpenPBR's layers (material/material_layers.slang packLayers), RG32_UINT
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
    // The meshlet path, [0] one-sided (back faces culled), [1] double-sided: task + mesh
    // shaders (when supported) and the fallback's pull vertex shader.
    std::array<VkPipeline, 2> m_meshPipelines{};
    std::array<VkPipeline, 2> m_pullPipelines{};
    // Alpha-tested materials (RenderScene::maskDraws, kDrawMask): core/gbuffer.slang's `masked`, [0] one-
    // sided (back faces culled), [1] double-sided, drawn one by one after the late phase's lists.
    std::array<VkPipeline, 2> m_maskPipelines{};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
    bool m_meshShaderSupported{false};
    // The stages the scene set and the push block reach: vertex and fragment, task and mesh
    // when supported.
    [[nodiscard]] VkShaderStageFlags geometryStages() const noexcept;

    GpuCulling* m_culling{nullptr};
    render::GpuPassTimer* m_passTimer{nullptr};

    // Descriptors for G-Buffer access

    // Sampler for G-Buffer

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

    // Push constants: the SSS tiles across and down (gbuffer.slang, bytes 0-15; the material
    // slot comes from the draw record), then gpu::shared::MeshletPush (the meshlet shaders,
    // bytes 16-63; their block covers 0-63).
    struct DrawPushConstant {
        uint32_t tilesX, tilesY;
        uint32_t pad0, pad1;
    };
    OHAO_CHECK_PUSH(DrawPushConstant, core_gbuffer)   // core/gbuffer.slang's
    static_assert(sizeof(DrawPushConstant) == 16);

    glm::mat4 m_view{1.0f};
    glm::mat4 m_projection{1.0f};
    glm::mat4 m_prevViewProj{1.0f};
    glm::vec3 m_cameraPos{0.0f};   // this frame's, from the view (the meshlet cone test)

    // Wireframe mode
    bool m_wireframeEnabled{false};

    // Bindless texture manager
    BindlessTextureManager* m_textureManager{nullptr};
};

} // namespace ohao
