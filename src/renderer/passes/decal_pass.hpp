#pragma once

#include "render_pass_base.hpp"
#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

namespace ohao {

class BindlessTextureManager;

// Deferred OBB decal pass.
//
// Renders one unit cube per decal using instanced drawing.  The fragment shader
// reconstructs world position from the GBuffer depth buffer, projects into each
// decal's OBB local space, clips outside the box, samples the bindless albedo
// texture, and writes blended colour to the GBuffer albedo attachment
// (LOAD_OP_LOAD so existing lit geometry is preserved).
//
// Usage:
//   decal.setGBufferAlbedo(gbuffer->getAlbedoView(), gbuffer->getAlbedoFormat());
//   decal.setDepthBuffer(gbuffer->getDepthView(), depthSampler);
//   decal.setBindlessManager(textureManager);
//   uint32_t h = decal.addDecal(desc);
//   // each frame, before execute():
//   decal.setMatrices(viewProj, invViewProj, screenSize);
//   decal.execute(cmd, frameIndex);
class DecalPass : public RenderPassBase {
public:
    static constexpr uint32_t MAX_DECALS = 256;

    // CPU-side decal description.  decalMatrix and worldMatrix are inverse
    // pairs — worldMatrix transforms the [-1,1]^3 unit cube to world space;
    // decalMatrix is its inverse, used in the shader to project world positions
    // back into OBB local space.
    struct DecalDesc {
        glm::mat4 decalMatrix;    // world → decal local space  (inverse of worldMatrix)
        glm::mat4 worldMatrix;    // decal local → world space
        glm::vec4 colorTint;      // rgba tint applied on top of texture
        uint32_t  albedoIdx;      // bindless texture index (0xFFFFFFFF = solid colour)
        uint32_t  normalIdx;      // unused — reserved for future normal-map decals
        float     opacity;        // global opacity multiplier [0, 1]
        float     roughnessScale; // reserved — passed to shader for future use
    };

    DecalPass()  = default;
    ~DecalPass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "DecalPass"; }

    // --- Decal lifecycle ---

    // Add a decal and return its handle (handle != 0 on success).
    uint32_t addDecal(const DecalDesc& desc);

    // Remove a previously added decal by handle.
    void removeDecal(uint32_t handle);

    // Remove all decals.
    void clearDecals();

    // --- Resource connections ---
    // Must be called (at least once, and again after any resize) before execute().

    // GBuffer albedo attachment (RGBA16F, LOAD_OP_LOAD).
    // format is used when creating the VkRenderPass; pass the GBuffer albedo format.
    void setGBufferAlbedo(VkImageView albedoView, VkFormat albedoFormat);

    // GBuffer depth buffer + a pre-created NEAREST sampler for it.
    // Layout expected at execute() time: DEPTH_STENCIL_READ_ONLY_OPTIMAL.
    void setDepthBuffer(VkImageView depthView, VkSampler depthSampler);

    // Per-frame camera matrices.
    void setMatrices(const glm::mat4& viewProj,
                     const glm::mat4& invViewProj,
                     const glm::vec2& screenSize);

    // Optional bindless texture manager.  When set, the bindless descriptor set
    // (set 1) is bound so decals can reference textures by index.
    void setBindlessManager(BindlessTextureManager* mgr);

private:
    // Push constants (144 bytes — well within the 256-byte NVIDIA limit)
    struct PushConstants {
        glm::mat4 viewProj;     //  64
        glm::mat4 invViewProj;  //  64
        glm::vec2 screenSize;   //   8
        float     pad[2];       //   8  → 144
    };
    static_assert(sizeof(PushConstants) <= 256,
                  "DecalPass push constants exceed 256-byte Vulkan limit");

    // GPU-side decal data struct (must match decal.vert / decal.frag std430 layout)
    struct DecalGPU {
        glm::mat4 decalMatrix;    // 64
        glm::mat4 worldMatrix;    // 64
        glm::vec4 colorTint;      // 16
        uint32_t  albedoIdx;      //  4
        uint32_t  normalIdx;      //  4
        float     opacity;        //  4
        float     roughnessScale; //  4  → 160 bytes per decal
    };

    // Helper builders
    bool createRenderPass();
    bool createFramebuffer();
    bool createDescriptors();
    bool createPipeline();
    bool createUnitCubeMesh();
    bool createDecalBuffer();

    // Framebuffer helpers (rebuilt on resize / albedo view change)
    void destroyFramebuffer();

    // Upload m_decals to the GPU SSBO (called when m_decalsDirty is set)
    void uploadDecals();

    // Write/refresh descriptor set 0 (SSBO + depth sampler)
    void updateDescriptors();

    // ---- Owned Vulkan resources ----
    VkRenderPass  m_renderPass{VK_NULL_HANDLE};
    VkFramebuffer m_framebuffer{VK_NULL_HANDLE};
    VkFormat      m_albedoFormat{VK_FORMAT_R16G16B16A16_SFLOAT};

    VkPipeline       m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    // Set 0: SSBO (DecalGPU array) + depth sampler
    VkDescriptorSetLayout m_descriptorLayout0{VK_NULL_HANDLE};
    VkDescriptorPool      m_descriptorPool0{VK_NULL_HANDLE};
    VkDescriptorSet       m_descriptorSet0{VK_NULL_HANDLE};

    // Unit cube geometry (8 verts, 36 indices, 12 triangles)
    VkBuffer       m_cubeVB{VK_NULL_HANDLE};
    VkDeviceMemory m_cubeVBMem{VK_NULL_HANDLE};
    VkBuffer       m_cubeIB{VK_NULL_HANDLE};
    VkDeviceMemory m_cubeIBMem{VK_NULL_HANDLE};
    static constexpr uint32_t CUBE_INDEX_COUNT = 36;

    // Persistently-mapped SSBO for DecalGPU data
    VkBuffer       m_decalBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_decalMemory{VK_NULL_HANDLE};
    void*          m_decalMapped{nullptr};

    // ---- External views (not owned) ----
    VkImageView m_albedoView{VK_NULL_HANDLE};
    VkImageView m_depthView{VK_NULL_HANDLE};
    VkSampler   m_depthSampler{VK_NULL_HANDLE};

    // ---- Decal registry ----
    std::vector<DecalGPU>             m_decals;        // GPU-ready packed data
    std::unordered_map<uint32_t, size_t> m_handleToIndex; // handle → m_decals index
    uint32_t m_nextHandle{1};   // 0 is reserved as "invalid"
    bool     m_decalsDirty{false};
    bool     m_descriptorDirty{false};

    // ---- Optional bindless texture manager ----
    BindlessTextureManager* m_bindlessMgr{nullptr};

    // ---- Per-frame camera data ----
    glm::mat4 m_viewProj{1.0f};
    glm::mat4 m_invViewProj{1.0f};
    glm::vec2 m_screenSize{1920.0f, 1080.0f};

    // ---- Render dimensions ----
    uint32_t m_width{1920};
    uint32_t m_height{1080};
};

} // namespace ohao
