#pragma once

#include "render_pass_base.hpp"
#include <glm/glm.hpp>
#include <array>
#include <string>

namespace ohao {

// GPU-tessellated terrain pass.
//
// Renders a heightmap-displaced quad-patch grid into the existing GBuffer using
// LOAD_OP_LOAD on all four colour MRTs and depth-test-only (read-only depth).
//
// Pipeline: vert (passthrough XZ) -> tesc (LOD tess levels) ->
//           tese (heightmap displacement + normals) -> frag (layer splatting -> GBuffer)
//
// LOD tessellation levels by camera distance:
//   < 25 m  -> 64    (full detail)
//   < 100 m -> 32
//   < 250 m ->  8
//   >= 250 m ->  2   (coarse silhouette)
//
// Usage:
//   pass.initialize(device, physDevice);
//   pass.setGBufferAttachments(posView, normalView, albedoView, velView, depthView, ...);
//   pass.setHeightmap(view, sampler);
//   pass.setSplatMap(view, sampler);
//   pass.setLayerAlbedo(0, grassAlbedoView);  // repeat for layers 1-3
//   pass.setLayerNormal(0, grassNormalView);
//   pass.setLayerSampler(linearSampler);
//   pass.setEnabled(true);
//   // Each frame:
//   pass.setViewProjection(viewProj, camPos);
//   pass.setTime(totalTime);
//   pass.execute(cmd, frameIndex);

class TerrainPass : public RenderPassBase {
public:
    TerrainPass() = default;
    ~TerrainPass() override;

    bool        initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void        cleanup() override;
    void        execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void        onResize(uint32_t width, uint32_t height) override;
    const char* getName() const override { return "TerrainPass"; }

    // -------------------------------------------------------------------------
    // GBuffer attachment wiring
    // Call whenever GBufferPass recreates its render targets (resize / init).
    // Views are NOT owned by this pass.
    // -------------------------------------------------------------------------
    void setGBufferAttachments(
        VkImageView posView,    VkImageView normalView,
        VkImageView albedoView, VkImageView velView,
        VkImageView depthView,
        VkFormat    colorFmt = VK_FORMAT_R16G16B16A16_SFLOAT,
        VkFormat    depthFmt = VK_FORMAT_D32_SFLOAT);

    // -------------------------------------------------------------------------
    // Per-frame camera state
    // -------------------------------------------------------------------------
    void setViewProjection(const glm::mat4& vp, const glm::vec3& camPos);

    // -------------------------------------------------------------------------
    // Terrain textures (NOT owned by this pass)
    // -------------------------------------------------------------------------
    // Heightmap: single-channel R32F or R8 texture
    void setHeightmap(VkImageView view, VkSampler sampler);
    // Splatmap: RGBA, R=grass G=dirt B=rock A=snow weights
    void setSplatMap(VkImageView view, VkSampler sampler);
    // Per-layer albedo (layers 0-3)
    void setLayerAlbedo(uint32_t layer, VkImageView view);
    // Per-layer normal map (layers 0-3)
    void setLayerNormal(uint32_t layer, VkImageView view);
    // Shared trilinear sampler for all layer albedo/normal textures
    void setLayerSampler(VkSampler sampler);

    // -------------------------------------------------------------------------
    // Terrain parameters
    // -------------------------------------------------------------------------
    void setEnabled(bool v)      { m_enabled    = v; }
    bool isEnabled() const       { return m_enabled; }

    void setHeightScale(float v) { m_heightScale = v; }
    void setTerrainSize(float v) { m_terrainSize = v; }
    void setSnowCover(float v)   { m_snowCover   = glm::clamp(v, 0.0f, 1.0f); }
    void setWetness(float v)     { m_wetness     = glm::clamp(v, 0.0f, 1.0f); }
    void setWaterLevel(float v)  { m_waterLevel  = v; }
    void setFrostCover(float v)  { m_frostCover  = glm::clamp(v, 0.0f, 1.0f); }
    void setTileOffset(float x, float z) { m_tileOffsetX = x; m_tileOffsetZ = z; }
    void setTime(float v)        { m_time        = v; }
    void setErosionEnabled(bool v)   { m_erosionEnabled = v; }
    void setErosionIterations(int n) { m_erosionIterations = glm::clamp(n, 0, 128); }
    void setErosionRate(float r)     { m_erosionParams.erosionRate = glm::clamp(r, 0.0f, 0.5f); }

    // Terrain type (drives material appearance in frag + procedural generation)
    // 0=external, 1=flat, 2=hills, 3=mountains, 4=canyon, 5=desert, 6=arctic
    void setTerrainType(int type) { m_genParams.type = type; m_useProceduralGen = (type != 0); if (m_useProceduralGen) m_genDirty = true; }
    int  getTerrainType() const   { return m_genParams.type; }

    // Procedural generation parameters
    void setGenFrequency(float f)    { m_genParams.frequency    = f; m_genDirty = true; }
    void setGenAmplitude(float a)    { m_genParams.amplitude    = glm::clamp(a, 0.0f, 1.0f); }
    void setGenOctaves(int n)        { m_genParams.octaves      = glm::clamp(n, 1, 12); m_genDirty = true; }
    void setGenPersistence(float p)  { m_genParams.persistence  = glm::clamp(p, 0.01f, 1.0f); }
    void setGenLacunarity(float l)   { m_genParams.lacunarity   = glm::clamp(l, 1.0f, 4.0f); }
    void setGenOffset(glm::vec2 off) { m_genParams.offset       = off; m_genDirty = true; }
    void setGenResolution(uint32_t r){ m_genParams.resolution   = static_cast<int32_t>(r); m_genNeedsRebuild = true; }

    // Macro variation texture (binding 10) — optional, use VK_NULL_HANDLE to disable
    void setMacroVariation(VkImageView view) { m_macroVariationView = view; m_descriptorDirty = true; }

    // Heightmap resolution hint (updates hmapResInv for shaders)
    void setHeightmapResolution(uint32_t res) { m_hmapResInv = 1.0f / static_cast<float>(res); }

    // Dispatch GPU terrain generation into the provided command buffer.
    // Call once at startup or whenever gen params change.
    // After this returns, the generated heightmap is bound as the active heightmap.
    void generateProcedural(VkCommandBuffer cmd);
    bool needsGeneration() const { return m_useProceduralGen && m_genImage == VK_NULL_HANDLE; }

    // Ensure the gen heightmap image is allocated (CPU-side allocation, must call before
    // recording generateProcedural into a command buffer).
    bool ensureGenHeightmap();

    // Accessors for GPU readback / physics sync
    VkImage  getGenImage()       const { return m_genImage; }
    int32_t  getGenResolution()  const { return m_genParams.resolution; }
    bool     hasProceduralHeightmap() const { return m_useProceduralGen && m_genImage != VK_NULL_HANDLE; }

    // Accessors for feeding splatmap to other passes (e.g. FoliagePass cull).
    // Returns the active splatmap view (may be externally set or owned by paint system).
    VkImageView getSplatmapView()     const { return m_splatmapView; }
    // Heightmap sampler is LINEAR + CLAMP_TO_EDGE — suitable for splatmap too.
    VkSampler   getHeightmapSampler() const { return m_heightmapSampler; }

    // -------------------------------------------------------------------------
    // Runtime splatmap painting (CPU-side brush, GPU upload on next execute()).
    // -------------------------------------------------------------------------
    // Paint a circular brush onto the CPU splatmap, then mark dirty for GPU upload.
    // channel: 0=grass, 1=dirt, 2=rock, 3=snow
    // worldPos: world-space XZ center; terrainSize must match current terrain size
    // radius: brush radius in world units; strength: [0,1] paint opacity
    void paintSplat(glm::vec2 worldPos, int channel, float radius,
                    float strength, float terrainSize);

    // Allocate GPU splatmap and upload CPU data. Called from execute() when dirty.
    // Returns the newly created VkImageView (or VK_NULL_HANDLE on failure).
    VkImageView flushSplatmap(VkCommandBuffer cmd);

    // Clear all splat paint (reset to procedural auto-blend / external splatmap).
    void clearSplatPaint();

private:
    // Push constant block shared by all four shader stages.
    // Total: 64 + 12 + 4*12 = 124 bytes
    struct TerrainPushConstants {
        glm::mat4 viewProj;      // 64
        glm::vec3 cameraPos;     // 12  ->  76
        float     heightScale;   //  4  ->  80
        float     terrainSize;   //  4  ->  84
        float     snowCover;     //  4  ->  88
        float     wetness;       //  4  ->  92
        float     time;          //  4  ->  96
        float     hmapResInv;    //  4  -> 100
        int32_t   terrainType;   //  4  -> 104
        float     pad2;          //  4  -> 108
        float     waterLevel;    //  4  -> 112
        float     frostCover;    //  4  -> 116
        float     tileOffsetX;   //  4  -> 120
        float     tileOffsetZ;   //  4  -> 124
    };
    static_assert(sizeof(TerrainPushConstants) <= 128,
                  "TerrainPushConstants must fit in 128-byte push-constant budget");
    static_assert(sizeof(TerrainPushConstants) % 4 == 0,
                  "TerrainPushConstants size must be a multiple of 4 bytes");

    // ---- Vulkan objects (owned) ----------------------------------------------
    VkRenderPass  m_renderPass{VK_NULL_HANDLE};
    VkFramebuffer m_framebuffer{VK_NULL_HANDLE};

    VkPipeline       m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

    VkDescriptorSetLayout m_descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_descriptorSet{VK_NULL_HANDLE};

    // Heightmap / splatmap sampler (LINEAR, CLAMP_TO_EDGE)
    VkSampler m_heightmapSampler{VK_NULL_HANDLE};
    // Layer albedo / normal sampler (stored externally, passed in)
    VkSampler m_layerSampler{VK_NULL_HANDLE};

    // Grid mesh (static, uploaded once)
    VkBuffer       m_vertexBuffer{VK_NULL_HANDLE};
    VkDeviceMemory m_vertexMemory{VK_NULL_HANDLE};
    uint32_t       m_vertexCount{0};   // N*N*4

    // ---- External (non-owned) views ------------------------------------------
    VkImageView m_gbufPos{VK_NULL_HANDLE};
    VkImageView m_gbufNormal{VK_NULL_HANDLE};
    VkImageView m_gbufAlbedo{VK_NULL_HANDLE};
    VkImageView m_gbufVel{VK_NULL_HANDLE};
    VkImageView m_gbufDepth{VK_NULL_HANDLE};
    // Actual formats read from GBufferPass (important for render pass compatibility)
    VkFormat    m_gbufPosFormat{VK_FORMAT_R16G16B16A16_SFLOAT};
    VkFormat    m_gbufNormalFormat{VK_FORMAT_A2R10G10B10_UNORM_PACK32};
    VkFormat    m_gbufAlbedoFormat{VK_FORMAT_R8G8B8A8_SRGB};
    VkFormat    m_gbufVelFormat{VK_FORMAT_R16G16_SFLOAT};
    VkFormat    m_depthFormat{VK_FORMAT_D32_SFLOAT};

    VkImageView m_heightmapView{VK_NULL_HANDLE};
    VkImageView m_splatmapView{VK_NULL_HANDLE};
    std::array<VkImageView, 4> m_layerAlbedoViews{};
    std::array<VkImageView, 4> m_layerNormalViews{};

    // Macro variation texture (external, not owned)
    VkImageView m_macroVariationView{VK_NULL_HANDLE};

    // ---- State ---------------------------------------------------------------
    bool      m_enabled{false};
    bool      m_descriptorDirty{true};
    bool      m_framebufferDirty{false};  // set when GBuffer views change
    float     m_heightScale{100.0f};
    float     m_terrainSize{1024.0f};
    float     m_snowCover{0.0f};
    float     m_wetness{0.0f};
    float     m_time{0.0f};
    float     m_hmapResInv{1.0f / 512.0f};  // updated when heightmap changes
    glm::mat4 m_viewProj{1.0f};
    glm::vec3 m_cameraPos{0.0f};
    float     m_waterLevel{0.0f};
    float     m_frostCover{0.0f};
    float     m_tileOffsetX{0.0f};
    float     m_tileOffsetZ{0.0f};
    uint32_t  m_width{1920};
    uint32_t  m_height{1080};

    // --- Procedural generation pipeline ---
    struct GenPushConstants {
        glm::vec2 offset{0.0f, 0.0f};   //  8
        float     frequency{2.0f};       //  4 -> 12
        float     amplitude{1.0f};       //  4 -> 16
        int32_t   octaves{8};            //  4 -> 20
        int32_t   type{2};              //  4 -> 24  (2=hills default)
        float     persistence{0.5f};     //  4 -> 28
        float     lacunarity{2.0f};      //  4 -> 32
        float     ridgeOffset{1.0f};     //  4 -> 36
        float     domainStrength{2.0f};  //  4 -> 40
        int32_t   resolution{512};       //  4 -> 44
        float     pad{0.0f};             //  4 -> 48
    };  // 48 bytes
    static_assert(sizeof(GenPushConstants) <= 128,
                  "GenPushConstants exceeds safe push-constant budget");

    VkPipeline            m_genPipeline{VK_NULL_HANDLE};
    VkPipelineLayout      m_genPipelineLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_genDescSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_genDescPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_genDescSet{VK_NULL_HANDLE};

    // Owned generated heightmap (R32F, resolution x resolution)
    VkImage               m_genImage{VK_NULL_HANDLE};
    VkDeviceMemory        m_genMemory{VK_NULL_HANDLE};
    VkImageView           m_genView{VK_NULL_HANDLE};
    VkSampler             m_genSampler{VK_NULL_HANDLE};

    GenPushConstants      m_genParams;
    bool                  m_useProceduralGen{false};
    bool                  m_genNeedsRebuild{false};  // image res changed
    bool                  m_genDirty{false};          // param changed, re-dispatch needed

    // --- Hydraulic erosion pipeline ---
    struct ErosionPushConstants {
        float   erosionRate{0.12f};
        float   talus{0.003f};
        float   depositRate{0.5f};
        int32_t resolution{512};
    };  // 16 bytes

    VkPipeline            m_erosionPipeline{VK_NULL_HANDLE};
    VkPipelineLayout      m_erosionPipelineLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_erosionDescSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_erosionDescPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_erosionDescSetA{VK_NULL_HANDLE};
    VkDescriptorSet       m_erosionDescSetB{VK_NULL_HANDLE};
    VkImage               m_erosionImage{VK_NULL_HANDLE};
    VkDeviceMemory        m_erosionMemory{VK_NULL_HANDLE};
    VkImageView           m_erosionView{VK_NULL_HANDLE};
    ErosionPushConstants  m_erosionParams;
    int                   m_erosionIterations{32};
    bool                  m_erosionEnabled{true};

    // ---- Runtime splatmap painting ------------------------------------------
    // CPU-side RGBA8 splatmap (R=grass, G=dirt, B=rock, A=snow).
    // Painted via paintSplat(), uploaded to GPU on execute() when dirty.
    std::vector<uint8_t>  m_cpuSplatmap;         // RGBA8, m_splatResolution × m_splatResolution
    uint32_t              m_splatResolution{512};
    bool                  m_splatDirty{false};

    // Owned GPU splatmap created by the paint system (not the external setSplatMap view).
    VkImage               m_ownedSplatImage{VK_NULL_HANDLE};
    VkDeviceMemory        m_ownedSplatMemory{VK_NULL_HANDLE};
    VkImageView           m_ownedSplatView{VK_NULL_HANDLE};
    // Staging buffer for partial uploads
    VkBuffer              m_splatStagingBuf{VK_NULL_HANDLE};
    VkDeviceMemory        m_splatStagingMem{VK_NULL_HANDLE};

    // ---- Private helpers ----------------------------------------------------
    bool createRenderPass();
    bool createFramebuffer();
    bool createDescriptors();
    bool createPipeline();
    bool createGridMesh();

    void destroyFramebuffer();
    void updateDescriptors();

    // Procedural gen helpers
    bool createGenPipeline();
    bool createGenHeightmap();   // creates m_genImage at m_genParams.resolution
    void destroyGenResources();  // destroys image/view/sampler/memory only
    void updateGenDescriptors();

    // Hydraulic erosion helpers
    bool createErosionPipeline();
    void applyErosion(VkCommandBuffer cmd);
};

} // namespace ohao
