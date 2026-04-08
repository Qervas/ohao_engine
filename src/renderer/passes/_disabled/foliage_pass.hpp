#pragma once

#include "render_pass_base.hpp"
#include <glm/glm.hpp>
#include <array>
#include <vector>

namespace ohao {

// ---------------------------------------------------------------------------
// FoliagePass — GPU-instanced cross-billboard foliage with compute culling.
//
// Architecture:
//   1. CPU uploads FoliageInstance array to a host-visible SSBO.
//   2. Every frame a compute pass (foliage_foliage_cull.comp.spv) frustum-
//      culls all instances and writes VkDrawIndexedIndirectCommand entries
//      into a device-local indirect buffer, setting instanceCount=0 for
//      culled blades.
//   3. A graphics pass renders the surviving instances directly from the
//      indirect buffer into the existing GBuffer (LOAD_OP_LOAD on all 4
//      colour MRTs + depth read-only).
//
// Billboard geometry:
//   Cross-quad: two intersecting quads at 90° (8 verts, 12 indices).
//   Single quad: one camera-facing quad       (4 verts,  6 indices).
//   LOD cutover at 50 m from camera.
//
// Integration:
//   After GBufferPass::execute() and before DeferredLightingPass:
//     pass.setGBufferAttachments(...);
//     pass.setMatrices(viewProj, camPos);
//     pass.setFrustumPlanes(planes);
//     pass.setWind(dir, strength, totalTime);
//     pass.execute(cmd, frameIndex);
// ---------------------------------------------------------------------------

class FoliagePass : public RenderPassBase {
public:
    // -------------------------------------------------------------------------
    // Per-instance data (same layout in GLSL SSBO).
    // -------------------------------------------------------------------------
    struct FoliageInstance {
        glm::vec3 position;  // world-space root position
        float     scale;     // uniform billboard scale (metres)
        glm::vec4 color;     // RGBA tint
        uint32_t  lod;       // caller hint (unused at runtime — cull shader overrides)
        uint32_t  pad[3];
    };
    static_assert(sizeof(FoliageInstance) == 48,
                  "FoliageInstance must be 48 bytes to match GLSL std430 layout");

    FoliagePass()  = default;
    ~FoliagePass() override;

    // RenderPassBase interface
    bool        initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void        cleanup() override;
    void        execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void        onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "FoliagePass"; }

    // -------------------------------------------------------------------------
    // Instance management
    // Call uploadInstances() whenever the scene changes; it memcpys to the
    // persistent host-visible mapping.  Max 65536 instances by default.
    // -------------------------------------------------------------------------
    void uploadInstances(const std::vector<FoliageInstance>& instances);
    void clearInstances();

    // -------------------------------------------------------------------------
    // GBuffer attachment wiring (not owned by this pass).
    // Must be called after GBufferPass initializes / resizes.
    // -------------------------------------------------------------------------
    void setGBufferAttachments(VkImageView posView,    VkImageView normalView,
                               VkImageView albedoView, VkImageView velView,
                               VkImageView depthView,
                               VkFormat colorFmt = VK_FORMAT_R16G16B16A16_SFLOAT,
                               VkFormat depthFmt = VK_FORMAT_D32_SFLOAT);

    // -------------------------------------------------------------------------
    // Grass texture (combined-image-sampler, set 0 binding 1 in draw shader).
    // Pass VK_NULL_HANDLE to use a built-in 1×1 white fallback.
    // -------------------------------------------------------------------------
    void setGrassTexture(VkImageView view, VkSampler sampler);

    // -------------------------------------------------------------------------
    // Per-frame camera / render state (call before execute()).
    // -------------------------------------------------------------------------
    void setMatrices(const glm::mat4& viewProj, const glm::vec3& camPos);
    void setFrustumPlanes(const std::array<glm::vec4, 6>& planes);
    void setWind(const glm::vec3& dir, float strength, float time);

    // -------------------------------------------------------------------------
    // Runtime toggles
    // -------------------------------------------------------------------------
    void  setEnabled(bool v)       { m_enabled      = v; }
    bool  isEnabled() const        { return m_enabled; }
    void  setCullDistance(float v) { m_cullDistance  = v; }
    float getCullDistance() const  { return m_cullDistance; }
    void  setTerrainSize(float v)  { m_terrainSize   = v; }

    // Set the terrain splatmap for density-aware cull (binding 2 in compute shader).
    // Pass VK_NULL_HANDLE to disable splatmap gating (all areas treated as grass).
    void setSplatmap(VkImageView view, VkSampler sampler) {
        m_splatmapView    = view;
        m_splatmapSampler = sampler;
        m_cullDescDirty   = true;  // force descriptor update
    }

private:
    // ------------------------------------------------------------------
    // Push-constant blocks — must mirror GLSL layouts exactly.
    // ------------------------------------------------------------------

    // Compute cull shader (foliage_foliage_cull.comp): 128 bytes
    struct CullParams {
        glm::vec4 frustumPlanes[6];  //  96
        glm::vec3 cameraPos;         //  12
        float     cullDistance;      //   4  → 112
        uint32_t  instanceCount;     //   4
        uint32_t  crossIndexCount;   //   4
        uint32_t  singleIndexCount;  //   4
        float     terrainSize;       //   4  → 128  (replaces former pad; feeds splatmap UV)
    };
    static_assert(sizeof(CullParams) == 128, "CullParams must be 128 bytes");

    // Graphics draw shaders (foliage_foliage.vert / .frag): 112 bytes
    struct FoliagePC {
        glm::mat4 viewProj;      //  64
        glm::vec3 cameraPos;     //  12
        float     time;          //   4  →  80
        glm::vec3 windDir;       //  12
        float     windStrength;  //   4  →  96
        float     pad[4];        //  16  → 112
    };
    static_assert(sizeof(FoliagePC) == 112, "FoliagePC must be 112 bytes");

    // ------------------------------------------------------------------
    // Vulkan objects — compute cull pipeline
    // ------------------------------------------------------------------
    VkPipeline            m_cullPipeline{VK_NULL_HANDLE};
    VkPipelineLayout      m_cullPipelineLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_cullDescLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_cullDescPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_cullDescSet{VK_NULL_HANDLE};

    // ------------------------------------------------------------------
    // Vulkan objects — graphics draw pipeline
    // ------------------------------------------------------------------
    VkPipeline            m_drawPipeline{VK_NULL_HANDLE};
    VkPipelineLayout      m_drawPipelineLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_drawDescLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_drawDescPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_drawDescSet{VK_NULL_HANDLE};
    VkRenderPass          m_renderPass{VK_NULL_HANDLE};
    VkFramebuffer         m_framebuffer{VK_NULL_HANDLE};

    // ------------------------------------------------------------------
    // Billboard mesh (static, uploaded once)
    //   Cross-quad : 8 verts, 12 indices  (m_crossIndexCount  = 12)
    //   Single-quad: 4 verts,  6 indices  (m_singleIndexCount =  6)
    //   Vertex stride: vec3(pos) + vec2(uv) + vec3(normal) = 32 bytes
    //   Index stride: uint16
    // ------------------------------------------------------------------
    VkBuffer       m_billboardVB{VK_NULL_HANDLE};
    VkDeviceMemory m_billboardVBMem{VK_NULL_HANDLE};
    VkBuffer       m_billboardIB{VK_NULL_HANDLE};
    VkDeviceMemory m_billboardIBMem{VK_NULL_HANDLE};
    uint32_t       m_crossIndexCount{12};
    uint32_t       m_singleIndexCount{6};

    // ------------------------------------------------------------------
    // Instance SSBO — host-visible + host-coherent (persistent map)
    // ------------------------------------------------------------------
    VkBuffer       m_instanceBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_instanceMemory{VK_NULL_HANDLE};
    void*          m_instanceMapped{nullptr};
    uint32_t       m_maxInstances{0};
    uint32_t       m_instanceCount{0};

    // ------------------------------------------------------------------
    // Indirect draw buffer — device-local (written by compute shader)
    // One VkDrawIndexedIndirectCommand per instance.
    // ------------------------------------------------------------------
    VkBuffer       m_indirectBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_indirectMemory{VK_NULL_HANDLE};

    // ------------------------------------------------------------------
    // Fallback 1×1 white texture (owned) used when no grass texture given.
    // ------------------------------------------------------------------
    VkImage        m_fallbackImage{VK_NULL_HANDLE};
    VkDeviceMemory m_fallbackMemory{VK_NULL_HANDLE};
    VkImageView    m_fallbackView{VK_NULL_HANDLE};
    VkSampler      m_fallbackSampler{VK_NULL_HANDLE};

    // External grass texture (not owned)
    VkImageView m_grassTexView{VK_NULL_HANDLE};
    VkSampler   m_grassSampler{VK_NULL_HANDLE};

    // External splatmap for density-aware culling (not owned)
    VkImageView m_splatmapView{VK_NULL_HANDLE};
    VkSampler   m_splatmapSampler{VK_NULL_HANDLE};

    // ------------------------------------------------------------------
    // GBuffer attachment views (not owned)
    // ------------------------------------------------------------------
    VkImageView m_gbufPos{VK_NULL_HANDLE};
    VkImageView m_gbufNormal{VK_NULL_HANDLE};
    VkImageView m_gbufAlbedo{VK_NULL_HANDLE};
    VkImageView m_gbufVel{VK_NULL_HANDLE};
    VkImageView m_gbufDepth{VK_NULL_HANDLE};
    VkFormat    m_colorFmt{VK_FORMAT_R16G16B16A16_SFLOAT};
    VkFormat    m_depthFmt{VK_FORMAT_D32_SFLOAT};

    // ------------------------------------------------------------------
    // Runtime state
    // ------------------------------------------------------------------
    bool  m_enabled{false};
    bool  m_cullDescDirty{true};
    bool  m_drawDescDirty{true};
    float m_cullDistance{120.0f};
    float m_terrainSize{1024.0f};  // fed into splatmap UV computation

    glm::mat4                m_viewProj{1.0f};
    glm::vec3                m_cameraPos{0.0f};
    std::array<glm::vec4, 6> m_frustumPlanes{};
    glm::vec3                m_windDir{1.0f, 0.0f, 0.0f};
    float                    m_windStrength{0.2f};
    float                    m_time{0.0f};

    uint32_t m_width{1920};
    uint32_t m_height{1080};

    // ------------------------------------------------------------------
    // Private helpers
    // ------------------------------------------------------------------
    bool createCullPipeline();
    bool createDrawPipeline();
    bool createGBufferRenderPass();
    bool createFramebuffer();
    bool createCullDescriptors();
    bool createDrawDescriptors();
    bool createBillboardMesh();
    bool createInstanceBuffer(uint32_t maxInstances);
    bool createIndirectBuffer(uint32_t maxInstances);
    bool createFallbackTexture();

    void destroyFramebuffer();
    void updateCullDescriptors();
    void updateDrawDescriptors();

    // Staging-buffer helper used by createBillboardMesh().
    bool uploadBufferViaStagingBuffer(VkBuffer dst, VkDeviceSize size,
                                     const void* data,
                                     VkBufferUsageFlags dstUsage);

    // Allocate + bind device-local GPU buffer.
    bool createGPUBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags props,
                         VkBuffer& outBuffer, VkDeviceMemory& outMemory);
};

} // namespace ohao
