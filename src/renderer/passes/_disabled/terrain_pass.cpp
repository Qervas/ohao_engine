#include "terrain_pass.hpp"
#include <array>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <iostream>
#include <glm/gtc/type_ptr.hpp>

namespace ohao {

// ---------------------------------------------------------------------------
// Grid geometry constants
// ---------------------------------------------------------------------------
// N = number of patches per axis.  N*N quad patches -> N*N*4 vertices.
// 32*32 = 1024 patches; each tessellated to up to 64 inner subdivisions.
static constexpr uint32_t TERRAIN_GRID_N = 32;

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
TerrainPass::~TerrainPass() {
    cleanup();
}

// ---------------------------------------------------------------------------
// initialize
// ---------------------------------------------------------------------------
bool TerrainPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device         = device;
    m_physicalDevice = physicalDevice;

    if (!createDescriptors()) {
        std::cerr << "[TerrainPass] createDescriptors failed\n";
        return false;
    }
    if (!createGridMesh()) {
        std::cerr << "[TerrainPass] createGridMesh failed\n";
        return false;
    }
    if (!createGenPipeline()) {
        std::cerr << "[TerrainPass] createGenPipeline failed (non-fatal, no procedural gen)\n";
        // not fatal — external heightmaps still work
    }
    createErosionPipeline();

    // createRenderPass, createFramebuffer, and createPipeline are deferred
    // until setGBufferAttachments() provides the actual GBuffer views and formats.
    // They are called lazily inside execute() when m_gbufPos is valid.
    return true;
}

// ---------------------------------------------------------------------------
// cleanup
// ---------------------------------------------------------------------------
void TerrainPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);

    safeDestroy(m_pipeline);
    safeDestroy(m_pipelineLayout);
    destroyFramebuffer();
    safeDestroy(m_renderPass);
    safeDestroy(m_descriptorPool);
    safeDestroy(m_descriptorLayout);
    safeDestroy(m_heightmapSampler);

    safeDestroy(m_vertexBuffer);
    safeFree(m_vertexMemory);

    // Cleanup erosion pipeline
    if (m_erosionPipeline        != VK_NULL_HANDLE) { vkDestroyPipeline(m_device, m_erosionPipeline, nullptr); m_erosionPipeline = VK_NULL_HANDLE; }
    if (m_erosionPipelineLayout  != VK_NULL_HANDLE) { vkDestroyPipelineLayout(m_device, m_erosionPipelineLayout, nullptr); m_erosionPipelineLayout = VK_NULL_HANDLE; }
    if (m_erosionDescSetLayout   != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(m_device, m_erosionDescSetLayout, nullptr); m_erosionDescSetLayout = VK_NULL_HANDLE; }
    if (m_erosionDescPool        != VK_NULL_HANDLE) { vkDestroyDescriptorPool(m_device, m_erosionDescPool, nullptr); m_erosionDescPool = VK_NULL_HANDLE; }
    if (m_erosionView            != VK_NULL_HANDLE) { vkDestroyImageView(m_device, m_erosionView, nullptr); m_erosionView = VK_NULL_HANDLE; }
    if (m_erosionImage           != VK_NULL_HANDLE) { vkDestroyImage(m_device, m_erosionImage, nullptr); m_erosionImage = VK_NULL_HANDLE; }
    if (m_erosionMemory          != VK_NULL_HANDLE) { vkFreeMemory(m_device, m_erosionMemory, nullptr); m_erosionMemory = VK_NULL_HANDLE; }

    // Destroy owned splatmap paint resources
    if (m_ownedSplatView   != VK_NULL_HANDLE) { vkDestroyImageView(m_device, m_ownedSplatView, nullptr);   m_ownedSplatView   = VK_NULL_HANDLE; }
    if (m_ownedSplatImage  != VK_NULL_HANDLE) { vkDestroyImage(m_device, m_ownedSplatImage, nullptr);      m_ownedSplatImage  = VK_NULL_HANDLE; }
    if (m_ownedSplatMemory != VK_NULL_HANDLE) { vkFreeMemory(m_device, m_ownedSplatMemory, nullptr);       m_ownedSplatMemory = VK_NULL_HANDLE; }
    if (m_splatStagingBuf  != VK_NULL_HANDLE) { vkDestroyBuffer(m_device, m_splatStagingBuf, nullptr);     m_splatStagingBuf  = VK_NULL_HANDLE; }
    if (m_splatStagingMem  != VK_NULL_HANDLE) { vkFreeMemory(m_device, m_splatStagingMem, nullptr);        m_splatStagingMem  = VK_NULL_HANDLE; }

    // Destroy gen pipeline resources
    safeDestroy(m_genPipeline);
    safeDestroy(m_genPipelineLayout);
    safeDestroy(m_genDescPool);
    safeDestroy(m_genDescSetLayout);
    destroyGenResources();
}

// ---------------------------------------------------------------------------
// onResize
// ---------------------------------------------------------------------------
void TerrainPass::onResize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;
    m_width  = width;
    m_height = height;
    // The DeferredRenderer will call setGBufferAttachments again with new views,
    // which triggers framebuffer and render pass recreation.
    destroyFramebuffer();
    safeDestroy(m_renderPass);
    safeDestroy(m_pipeline);
    safeDestroy(m_pipelineLayout);
}

// ---------------------------------------------------------------------------
// setGBufferAttachments
// ---------------------------------------------------------------------------
void TerrainPass::setGBufferAttachments(
    VkImageView posView,    VkImageView normalView,
    VkImageView albedoView, VkImageView velView,
    VkImageView depthView,
    VkFormat    colorFmt,
    VkFormat    depthFmt)
{
    m_gbufPos    = posView;
    m_gbufNormal = normalView;
    m_gbufAlbedo = albedoView;
    m_gbufVel    = velView;
    m_gbufDepth  = depthView;

    // Store the true formats from GBufferPass.
    // Color attachments use different formats per slot:
    //   slot 0 (pos)    : VK_FORMAT_R16G16B16A16_SFLOAT   (passed as colorFmt)
    //   slot 1 (normal) : VK_FORMAT_A2R10G10B10_UNORM_PACK32
    //   slot 2 (albedo) : VK_FORMAT_R8G8B8A8_SRGB
    //   slot 3 (vel)    : VK_FORMAT_R16G16_SFLOAT
    // The caller passes the position buffer format as colorFmt; the rest are hardcoded
    // to match what GBufferPass::createGBuffer() allocates.
    m_gbufPosFormat    = colorFmt;
    m_gbufNormalFormat = VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    m_gbufAlbedoFormat = VK_FORMAT_R8G8B8A8_SRGB;
    m_gbufVelFormat    = VK_FORMAT_R16G16_SFLOAT;
    m_depthFormat      = depthFmt;

    // Rebuild render pass (format-dependent) and framebuffer.
    destroyFramebuffer();
    safeDestroy(m_renderPass);
    safeDestroy(m_pipeline);
    safeDestroy(m_pipelineLayout);

    if (!createRenderPass()) {
        std::cerr << "[TerrainPass] createRenderPass failed\n";
        return;
    }
    if (!createFramebuffer()) {
        std::cerr << "[TerrainPass] createFramebuffer failed\n";
        return;
    }
    if (!createPipeline()) {
        std::cerr << "[TerrainPass] createPipeline failed\n";
    }
}

// ---------------------------------------------------------------------------
// setViewProjection
// ---------------------------------------------------------------------------
void TerrainPass::setViewProjection(const glm::mat4& vp, const glm::vec3& camPos) {
    m_viewProj  = vp;
    m_cameraPos = camPos;
}

// ---------------------------------------------------------------------------
// Texture setters
// ---------------------------------------------------------------------------
void TerrainPass::setHeightmap(VkImageView view, VkSampler sampler) {
    m_heightmapView    = view;
    // If caller supplies a sampler use it, otherwise we use our own linear sampler.
    if (sampler != VK_NULL_HANDLE) m_heightmapSampler = sampler;
    m_descriptorDirty = true;
}

void TerrainPass::setSplatMap(VkImageView view, VkSampler sampler) {
    m_splatmapView    = view;
    (void)sampler;  // reuse same heightmap sampler for splatmap
    m_descriptorDirty = true;
}

void TerrainPass::setLayerAlbedo(uint32_t layer, VkImageView view) {
    if (layer < 4) {
        m_layerAlbedoViews[layer] = view;
        m_descriptorDirty = true;
    }
}

void TerrainPass::setLayerNormal(uint32_t layer, VkImageView view) {
    if (layer < 4) {
        m_layerNormalViews[layer] = view;
        m_descriptorDirty = true;
    }
}

void TerrainPass::setLayerSampler(VkSampler sampler) {
    m_layerSampler    = sampler;
    m_descriptorDirty = true;
}

// ---------------------------------------------------------------------------
// execute
// ---------------------------------------------------------------------------
void TerrainPass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (!m_enabled)                           return;
    if (m_renderPass  == VK_NULL_HANDLE)      return;
    if (m_framebuffer == VK_NULL_HANDLE)      return;
    if (m_pipeline    == VK_NULL_HANDLE)      return;
    if (m_vertexBuffer == VK_NULL_HANDLE)     return;

    // ── Upload painted splatmap if dirty ────────────────────────────────────
    if (m_splatDirty && !m_cpuSplatmap.empty()) {
        flushSplatmap(cmd);
    }

    // ── Auto-regenerate procedural heightmap when params changed ─────────────
    // If type, frequency, octaves, or offset changed since last frame,
    // re-dispatch the compute shader before rendering.
    if (m_useProceduralGen && (m_genDirty || m_genNeedsRebuild)) {
        if (ensureGenHeightmap()) {
            generateProcedural(cmd);
            // Destroy erosion buffer on resize (forces re-alloc at new resolution)
            if (m_genNeedsRebuild && m_erosionImage != VK_NULL_HANDLE) {
                vkDestroyImageView(m_device, m_erosionView, nullptr);   m_erosionView = VK_NULL_HANDLE;
                vkDestroyImage(m_device, m_erosionImage, nullptr);       m_erosionImage = VK_NULL_HANDLE;
                vkFreeMemory(m_device, m_erosionMemory, nullptr);        m_erosionMemory = VK_NULL_HANDLE;
            }
            applyErosion(cmd);
            m_genDirty        = false;
            m_genNeedsRebuild = false;
        }
    }

    if (m_descriptorDirty) {
        updateDescriptors();
        m_descriptorDirty = false;
    }

    // --- Begin render pass ---------------------------------------------------
    // No clear values needed; we're using LOAD_OP_LOAD throughout.
    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass        = m_renderPass;
    rpInfo.framebuffer       = m_framebuffer;
    rpInfo.renderArea.offset = {0, 0};
    rpInfo.renderArea.extent = {m_width, m_height};
    rpInfo.clearValueCount   = 0;
    rpInfo.pClearValues      = nullptr;

    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Dynamic viewport / scissor
    VkViewport vp{};
    vp.x        = 0.0f;
    vp.y        = 0.0f;
    vp.width    = static_cast<float>(m_width);
    vp.height   = static_cast<float>(m_height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {m_width, m_height};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Bind pipeline and descriptors
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    if (m_descriptorSet != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_pipelineLayout, 0, 1, &m_descriptorSet,
                                0, nullptr);
    }

    // Push constants (all tessellation + fragment stages)
    TerrainPushConstants pc{};
    pc.viewProj    = m_viewProj;
    pc.cameraPos   = m_cameraPos;
    pc.heightScale = m_heightScale;
    pc.terrainSize = m_terrainSize;
    pc.snowCover   = m_snowCover;
    pc.wetness     = m_wetness;
    pc.time        = m_time;
    pc.hmapResInv  = m_hmapResInv;
    pc.terrainType = m_genParams.type;
    pc.pad2        = 0.0f;
    pc.waterLevel  = m_waterLevel;
    pc.frostCover  = m_frostCover;
    pc.tileOffsetX = m_tileOffsetX;
    pc.tileOffsetZ = m_tileOffsetZ;

    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT |
                       VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                       VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT |
                       VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(TerrainPushConstants), &pc);

    // Bind vertex buffer (each vertex is a vec2 XZ position)
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertexBuffer, &offset);

    // Draw: m_vertexCount vertices, 4 per patch -> m_vertexCount/4 patches
    vkCmdDraw(cmd, m_vertexCount, 1, 0, 0);

    vkCmdEndRenderPass(cmd);
}

// ---------------------------------------------------------------------------
// createRenderPass
// ---------------------------------------------------------------------------
bool TerrainPass::createRenderPass() {
    // 5 attachments: [pos, normal, albedo, vel, depth]
    // All colour attachments: LOAD_OP_LOAD, SHADER_READ_ONLY -> COLOR_ATT -> SHADER_READ_ONLY
    // Depth attachment:       LOAD_OP_LOAD, read-only depth test, layout stays DEPTH_STENCIL_READ_ONLY

    std::array<VkAttachmentDescription, 5> atts{};

    // Attachment 0 — Position + Metallic (R16G16B16A16_SFLOAT)
    atts[0].format         = m_gbufPosFormat;
    atts[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    atts[0].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    atts[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    atts[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[0].initialLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    atts[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Attachment 1 — Normal + Roughness (A2R10G10B10_UNORM_PACK32)
    atts[1]        = atts[0];
    atts[1].format = m_gbufNormalFormat;

    // Attachment 2 — Albedo + AO (R8G8B8A8_SRGB)
    atts[2]        = atts[0];
    atts[2].format = m_gbufAlbedoFormat;

    // Attachment 3 — Velocity (R16G16_SFLOAT)
    atts[3]        = atts[0];
    atts[3].format = m_gbufVelFormat;

    // Attachment 4 — Depth (D32_SFLOAT) — read-only depth test
    atts[4].format         = m_depthFormat;
    atts[4].samples        = VK_SAMPLE_COUNT_1_BIT;
    atts[4].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    atts[4].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;   // terrain does not write depth
    atts[4].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[4].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[4].initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    atts[4].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    // Colour attachment references — subpass uses COLOR_ATTACHMENT_OPTIMAL
    std::array<VkAttachmentReference, 4> colorRefs{};
    for (uint32_t i = 0; i < 4; ++i) {
        colorRefs[i].attachment = i;
        colorRefs[i].layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    // Depth reference — read-only (test without write)
    VkAttachmentReference depthRef{};
    depthRef.attachment = 4;
    depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = static_cast<uint32_t>(colorRefs.size());
    subpass.pColorAttachments       = colorRefs.data();
    subpass.pDepthStencilAttachment = &depthRef;

    // Subpass dependencies ensure correct image layout transitions
    std::array<VkSubpassDependency, 2> deps{};

    // External -> subpass 0: wait for previous shader reads before writing colour
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    // Subpass 0 -> external: make colour writes visible to downstream shader reads
    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = static_cast<uint32_t>(atts.size());
    rpInfo.pAttachments    = atts.data();
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;
    rpInfo.dependencyCount = static_cast<uint32_t>(deps.size());
    rpInfo.pDependencies   = deps.data();

    return vkCreateRenderPass(m_device, &rpInfo, nullptr, &m_renderPass) == VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// createFramebuffer
// ---------------------------------------------------------------------------
bool TerrainPass::createFramebuffer() {
    if (m_gbufPos   == VK_NULL_HANDLE ||
        m_gbufNormal == VK_NULL_HANDLE ||
        m_gbufAlbedo == VK_NULL_HANDLE ||
        m_gbufVel    == VK_NULL_HANDLE ||
        m_gbufDepth  == VK_NULL_HANDLE ||
        m_renderPass == VK_NULL_HANDLE) {
        return false;
    }

    std::array<VkImageView, 5> views = {
        m_gbufPos, m_gbufNormal, m_gbufAlbedo, m_gbufVel, m_gbufDepth
    };

    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass      = m_renderPass;
    fbInfo.attachmentCount = static_cast<uint32_t>(views.size());
    fbInfo.pAttachments    = views.data();
    fbInfo.width           = m_width;
    fbInfo.height          = m_height;
    fbInfo.layers          = 1;

    return vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_framebuffer) == VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// destroyFramebuffer
// ---------------------------------------------------------------------------
void TerrainPass::destroyFramebuffer() {
    safeDestroy(m_framebuffer);
}

// ---------------------------------------------------------------------------
// createDescriptors
// ---------------------------------------------------------------------------
bool TerrainPass::createDescriptors() {
    // 11 bindings (all COMBINED_IMAGE_SAMPLER):
    //   0 : heightmap                        (tese + frag)
    //   1 : splatmap                         (tese + frag)
    //   2-5: layer albedo 0..3               (tese + frag)
    //   6-9: layer normal 0..3               (tese + frag)
    //   10 : macro variation texture         (frag only)
    std::array<VkDescriptorSetLayoutBinding, 11> bindings{};
    for (uint32_t i = 0; i < 10; ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT |
                                      VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    // Binding 10: macro variation texture (fragment shader only)
    bindings[10].binding         = 10;
    bindings[10].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[10].descriptorCount = 1;
    bindings[10].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr,
                                    &m_descriptorLayout) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 11;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr,
                               &m_descriptorPool) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descriptorLayout;
    return vkAllocateDescriptorSets(m_device, &allocInfo, &m_descriptorSet) == VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// updateDescriptors
// ---------------------------------------------------------------------------
void TerrainPass::updateDescriptors() {
    if (m_descriptorSet == VK_NULL_HANDLE) return;

    // We need at least a heightmap view to write binding 0.
    // For all other unset views we use the heightmap view as a placeholder so
    // the descriptor is never left uninitialised (NVIDIA driver requires all
    // declared bindings to be written before first use).
    VkImageView fallback = m_heightmapView;
    if (fallback == VK_NULL_HANDLE) return;  // can't do anything without heightmap

    VkSampler hmSampler = m_heightmapSampler;
    if (hmSampler == VK_NULL_HANDLE) return;

    VkSampler layerSampler = (m_layerSampler != VK_NULL_HANDLE)
                              ? m_layerSampler : hmSampler;

    // Build image info array for all 11 bindings
    std::array<VkDescriptorImageInfo, 11> imgInfos{};

    // binding 0: heightmap
    imgInfos[0].sampler     = hmSampler;
    imgInfos[0].imageView   = m_heightmapView;
    imgInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // binding 1: splatmap
    imgInfos[1].sampler     = hmSampler;
    imgInfos[1].imageView   = (m_splatmapView != VK_NULL_HANDLE) ? m_splatmapView : fallback;
    imgInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // bindings 2-5: layer albedos
    for (uint32_t i = 0; i < 4; ++i) {
        imgInfos[2 + i].sampler     = layerSampler;
        imgInfos[2 + i].imageView   = (m_layerAlbedoViews[i] != VK_NULL_HANDLE)
                                       ? m_layerAlbedoViews[i] : fallback;
        imgInfos[2 + i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    // bindings 6-9: layer normals
    for (uint32_t i = 0; i < 4; ++i) {
        imgInfos[6 + i].sampler     = layerSampler;
        imgInfos[6 + i].imageView   = (m_layerNormalViews[i] != VK_NULL_HANDLE)
                                       ? m_layerNormalViews[i] : fallback;
        imgInfos[6 + i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    // binding 10: macro variation (use heightmap view as fallback if null)
    VkSampler macroSampler = (m_layerSampler != VK_NULL_HANDLE) ? m_layerSampler : hmSampler;
    imgInfos[10].sampler     = macroSampler;
    imgInfos[10].imageView   = (m_macroVariationView != VK_NULL_HANDLE) ? m_macroVariationView : fallback;
    imgInfos[10].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Write all 11 bindings in one call
    std::array<VkWriteDescriptorSet, 11> writes{};
    for (uint32_t i = 0; i < 11; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = m_descriptorSet;
        writes[i].dstBinding      = i;
        writes[i].dstArrayElement = 0;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo      = &imgInfos[i];
    }

    vkUpdateDescriptorSets(m_device,
                           static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

// ---------------------------------------------------------------------------
// createPipeline
// ---------------------------------------------------------------------------
bool TerrainPass::createPipeline() {
    if (m_renderPass == VK_NULL_HANDLE || m_descriptorLayout == VK_NULL_HANDLE)
        return false;

    // Load all four shader stages, with graceful failure reporting
    VkShaderModule vertShader{VK_NULL_HANDLE};
    VkShaderModule tescShader{VK_NULL_HANDLE};
    VkShaderModule teseShader{VK_NULL_HANDLE};
    VkShaderModule fragShader{VK_NULL_HANDLE};

    auto safeLoad = [&](const std::string& name, VkShaderModule& out) -> bool {
        try {
            out = loadShaderModule(name);
            return out != VK_NULL_HANDLE;
        } catch (const std::exception& e) {
            std::cerr << "[TerrainPass] Failed to load shader '" << name
                      << "': " << e.what() << "\n";
            return false;
        }
    };

    bool ok = safeLoad("terrain_terrain.vert.spv", vertShader) &&
              safeLoad("terrain_terrain.tesc.spv", tescShader) &&
              safeLoad("terrain_terrain.tese.spv", teseShader) &&
              safeLoad("terrain_terrain.frag.spv", fragShader);

    auto destroyModules = [&]() {
        if (vertShader) vkDestroyShaderModule(m_device, vertShader, nullptr);
        if (tescShader) vkDestroyShaderModule(m_device, tescShader, nullptr);
        if (teseShader) vkDestroyShaderModule(m_device, teseShader, nullptr);
        if (fragShader) vkDestroyShaderModule(m_device, fragShader, nullptr);
    };

    if (!ok) {
        destroyModules();
        return false;
    }

    // --- Shader stages -------------------------------------------------------
    std::array<VkPipelineShaderStageCreateInfo, 4> stages{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertShader;
    stages[0].pName  = "main";

    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    stages[1].module = tescShader;
    stages[1].pName  = "main";

    stages[2].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[2].stage  = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    stages[2].module = teseShader;
    stages[2].pName  = "main";

    stages[3].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[3].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[3].module = fragShader;
    stages[3].pName  = "main";

    // --- Vertex input: single binding, vec2 XZ --------------------------------
    VkVertexInputBindingDescription vertBinding{};
    vertBinding.binding   = 0;
    vertBinding.stride    = sizeof(float) * 2;  // vec2
    vertBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vertAttrib{};
    vertAttrib.location = 0;
    vertAttrib.binding  = 0;
    vertAttrib.format   = VK_FORMAT_R32G32_SFLOAT;
    vertAttrib.offset   = 0;

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &vertBinding;
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions    = &vertAttrib;

    // --- Input assembly: patch list for tessellation -------------------------
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;

    // --- Tessellation state: 4 control points per quad patch -----------------
    VkPipelineTessellationStateCreateInfo tessState{};
    tessState.sType              = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO;
    tessState.patchControlPoints = 4;

    // --- Viewport state (dynamic) --------------------------------------------
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    // --- Rasterizer: no backface culling (terrain visible from below as well) --
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth   = 1.0f;
    rasterizer.cullMode    = VK_CULL_MODE_NONE;
    rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    // --- Multisampling --------------------------------------------------------
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // --- Depth-stencil: test enabled (read-only — terrain receives occlusion) --
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;  // depth attachment is read-only
    depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS;
    depthStencil.stencilTestEnable = VK_FALSE;

    // --- Colour blend: 4 attachments, no blending (opaque GBuffer writes) -----
    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.blendEnable    = VK_FALSE;
    blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    std::array<VkPipelineColorBlendAttachmentState, 4> blendAtts;
    blendAtts.fill(blendAtt);

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = static_cast<uint32_t>(blendAtts.size());
    colorBlending.pAttachments    = blendAtts.data();

    // --- Dynamic state --------------------------------------------------------
    std::array<VkDynamicState, 2> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates    = dynamicStates.data();

    // --- Pipeline layout (1 descriptor set + push constants) -----------------
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                           VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(TerrainPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &m_descriptorLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        destroyModules();
        return false;
    }

    // --- Graphics pipeline ---------------------------------------------------
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount          = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages             = stages.data();
    pipelineInfo.pVertexInputState   = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pTessellationState  = &tessState;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState   = &multisampling;
    pipelineInfo.pDepthStencilState  = &depthStencil;
    pipelineInfo.pColorBlendState    = &colorBlending;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = m_pipelineLayout;
    pipelineInfo.renderPass          = m_renderPass;
    pipelineInfo.subpass             = 0;

    VkResult result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1,
                                                &pipelineInfo, nullptr, &m_pipeline);
    destroyModules();
    return result == VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// createGridMesh
// ---------------------------------------------------------------------------
bool TerrainPass::createGridMesh() {
    // Generate N x N quad patches.  Each patch has 4 corner control points
    // arranged in counter-clockwise order (BL, BR, TR, TL) to match the
    // tese CCW domain and the winding convention used in terrain.tesc.
    const uint32_t N      = TERRAIN_GRID_N;
    const float    step   = 1.0f / static_cast<float>(N);
    const float    origin = -0.5f;  // [-0.5, 0.5] space

    // Reserve 4 vertices per quad patch
    std::vector<float> vertices;
    vertices.reserve(static_cast<size_t>(N) * N * 4 * 2);  // 2 floats per vertex

    for (uint32_t row = 0; row < N; ++row) {
        for (uint32_t col = 0; col < N; ++col) {
            float x0 = origin + col       * step;
            float x1 = origin + (col + 1) * step;
            float z0 = origin + row       * step;
            float z1 = origin + (row + 1) * step;

            // BL
            vertices.push_back(x0); vertices.push_back(z0);
            // BR
            vertices.push_back(x1); vertices.push_back(z0);
            // TR
            vertices.push_back(x1); vertices.push_back(z1);
            // TL
            vertices.push_back(x0); vertices.push_back(z1);
        }
    }

    m_vertexCount = static_cast<uint32_t>(vertices.size() / 2);  // N*N*4
    VkDeviceSize bufferSize = vertices.size() * sizeof(float);

    // Create host-visible, host-coherent buffer (terrain grid is small enough)
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size        = bufferSize;
    bufInfo.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_device, &bufInfo, nullptr, &m_vertexBuffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, m_vertexBuffer, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(
        memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_vertexMemory) != VK_SUCCESS) {
        safeDestroy(m_vertexBuffer);
        return false;
    }
    vkBindBufferMemory(m_device, m_vertexBuffer, m_vertexMemory, 0);

    // Upload vertex data
    void* data;
    vkMapMemory(m_device, m_vertexMemory, 0, bufferSize, 0, &data);
    memcpy(data, vertices.data(), static_cast<size_t>(bufferSize));
    vkUnmapMemory(m_device, m_vertexMemory);

    return true;
}

// ---------------------------------------------------------------------------
// createGenPipeline
// ---------------------------------------------------------------------------
bool TerrainPass::createGenPipeline() {
    // Descriptor set layout: one storage image (binding 0)
    VkDescriptorSetLayoutBinding genBinding{};
    genBinding.binding         = 0;
    genBinding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    genBinding.descriptorCount = 1;
    genBinding.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dslCI{};
    dslCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslCI.bindingCount = 1;
    dslCI.pBindings    = &genBinding;
    if (vkCreateDescriptorSetLayout(m_device, &dslCI, nullptr, &m_genDescSetLayout) != VK_SUCCESS)
        return false;

    // Descriptor pool
    VkDescriptorPoolSize ps{};
    ps.type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    ps.descriptorCount = 1;

    VkDescriptorPoolCreateInfo dpCI{};
    dpCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpCI.maxSets       = 1;
    dpCI.poolSizeCount = 1;
    dpCI.pPoolSizes    = &ps;
    if (vkCreateDescriptorPool(m_device, &dpCI, nullptr, &m_genDescPool) != VK_SUCCESS)
        return false;

    VkDescriptorSetAllocateInfo dsAI{};
    dsAI.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAI.descriptorPool     = m_genDescPool;
    dsAI.descriptorSetCount = 1;
    dsAI.pSetLayouts        = &m_genDescSetLayout;
    if (vkAllocateDescriptorSets(m_device, &dsAI, &m_genDescSet) != VK_SUCCESS)
        return false;

    // Push constant range: GenPushConstants (48 bytes)
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(GenPushConstants);

    VkPipelineLayoutCreateInfo plCI{};
    plCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plCI.setLayoutCount         = 1;
    plCI.pSetLayouts            = &m_genDescSetLayout;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges    = &pcRange;
    if (vkCreatePipelineLayout(m_device, &plCI, nullptr, &m_genPipelineLayout) != VK_SUCCESS)
        return false;

    // Load compute shader (uses base path prefix like other passes)
    VkShaderModule shaderModule = loadShaderModule("terrain_terrain_gen.comp.spv");
    if (shaderModule == VK_NULL_HANDLE) {
        std::cerr << "[TerrainPass] terrain_terrain_gen.comp.spv not found\n";
        return false;
    }

    VkPipelineShaderStageCreateInfo stageCI{};
    stageCI.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageCI.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    stageCI.module = shaderModule;
    stageCI.pName  = "main";

    VkComputePipelineCreateInfo cpCI{};
    cpCI.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpCI.stage  = stageCI;
    cpCI.layout = m_genPipelineLayout;

    VkResult res = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpCI, nullptr, &m_genPipeline);
    vkDestroyShaderModule(m_device, shaderModule, nullptr);
    return res == VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// createGenHeightmap
// ---------------------------------------------------------------------------
bool TerrainPass::createGenHeightmap() {
    destroyGenResources();  // clean up previous if any

    uint32_t res = static_cast<uint32_t>(m_genParams.resolution);

    VkImageCreateInfo imgCI{};
    imgCI.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCI.imageType     = VK_IMAGE_TYPE_2D;
    imgCI.format        = VK_FORMAT_R32_SFLOAT;
    imgCI.extent        = {res, res, 1};
    imgCI.mipLevels     = 1;
    imgCI.arrayLayers   = 1;
    imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_device, &imgCI, nullptr, &m_genImage) != VK_SUCCESS) return false;

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(m_device, m_genImage, &memReq);
    uint32_t memIdx = findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = memReq.size;
    allocInfo.memoryTypeIndex = memIdx;
    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_genMemory) != VK_SUCCESS) return false;
    vkBindImageMemory(m_device, m_genImage, m_genMemory, 0);

    VkImageViewCreateInfo ivCI{};
    ivCI.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivCI.image            = m_genImage;
    ivCI.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    ivCI.format           = VK_FORMAT_R32_SFLOAT;
    ivCI.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(m_device, &ivCI, nullptr, &m_genView) != VK_SUCCESS) return false;

    VkSamplerCreateInfo sampCI{};
    sampCI.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampCI.magFilter    = VK_FILTER_LINEAR;
    sampCI.minFilter    = VK_FILTER_LINEAR;
    sampCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(m_device, &sampCI, nullptr, &m_genSampler) != VK_SUCCESS) return false;

    m_hmapResInv      = 1.0f / static_cast<float>(res);
    m_genNeedsRebuild = false;
    return true;
}

// ---------------------------------------------------------------------------
// ensureGenHeightmap  (public, call before generateProcedural)
// ---------------------------------------------------------------------------
bool TerrainPass::ensureGenHeightmap() {
    if (m_genImage != VK_NULL_HANDLE && !m_genNeedsRebuild) return true;
    return createGenHeightmap();
}

// ---------------------------------------------------------------------------
// destroyGenResources  (image/view/sampler/memory only — not pipeline)
// ---------------------------------------------------------------------------
void TerrainPass::destroyGenResources() {
    if (m_device == VK_NULL_HANDLE) return;
    safeDestroy(m_genSampler);
    safeDestroy(m_genView);
    safeDestroy(m_genImage);
    safeFree(m_genMemory);
}

// ---------------------------------------------------------------------------
// updateGenDescriptors
// ---------------------------------------------------------------------------
void TerrainPass::updateGenDescriptors() {
    if (m_genDescSet == VK_NULL_HANDLE || m_genView == VK_NULL_HANDLE) return;

    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageView   = m_genView;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = m_genDescSet;
    write.dstBinding      = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.descriptorCount = 1;
    write.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
}

// ---------------------------------------------------------------------------
// generateProcedural
// ---------------------------------------------------------------------------
void TerrainPass::generateProcedural(VkCommandBuffer cmd) {
    if (m_genPipeline == VK_NULL_HANDLE) return;
    if (m_genImage == VK_NULL_HANDLE || m_genNeedsRebuild) {
        // Caller must have called ensureGenHeightmap() before recording commands.
        std::cerr << "[TerrainPass] generateProcedural: gen image not ready — "
                     "call ensureGenHeightmap() before recording\n";
        return;
    }

    // 1. Transition: UNDEFINED -> GENERAL (for imageStore)
    VkImageMemoryBarrier toGeneral{};
    toGeneral.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGeneral.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    toGeneral.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.image               = m_genImage;
    toGeneral.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toGeneral.srcAccessMask       = 0;
    toGeneral.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toGeneral);

    // 2. Update descriptor: bind gen image as GENERAL storage image
    updateGenDescriptors();

    // 3. Bind compute pipeline + descriptor set
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_genPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        m_genPipelineLayout, 0, 1, &m_genDescSet, 0, nullptr);

    // 4. Push generation parameters
    vkCmdPushConstants(cmd, m_genPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
        0, sizeof(GenPushConstants), &m_genParams);

    // 5. Dispatch: local_size = 16x16, so ceil(res/16) groups per axis
    uint32_t res    = static_cast<uint32_t>(m_genParams.resolution);
    uint32_t groups = (res + 15) / 16;
    vkCmdDispatch(cmd, groups, groups, 1);

    // 6. Transition: GENERAL -> SHADER_READ_ONLY_OPTIMAL (for terrain render)
    VkImageMemoryBarrier toRead{};
    toRead.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRead.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    toRead.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.image               = m_genImage;
    toRead.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toRead.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    toRead.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toRead);

    // 7. Wire the generated image + sampler as active heightmap for the render descriptors
    m_heightmapView    = m_genView;
    m_heightmapSampler = m_genSampler;
    m_descriptorDirty  = true;
}

// ---------------------------------------------------------------------------
// createErosionPipeline
// ---------------------------------------------------------------------------
bool TerrainPass::createErosionPipeline() {
    if (!m_device || !m_physicalDevice) return false;

    // ── Descriptor set layout: binding 0 = src (readonly), binding 1 = dst (writeonly) ──
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 2;
    layoutCI.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(m_device, &layoutCI, nullptr, &m_erosionDescSetLayout) != VK_SUCCESS) return false;

    // ── Descriptor pool (2 sets × 2 storage images) ──
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4};
    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets       = 2;
    poolCI.poolSizeCount = 1;
    poolCI.pPoolSizes    = &poolSize;
    if (vkCreateDescriptorPool(m_device, &poolCI, nullptr, &m_erosionDescPool) != VK_SUCCESS) return false;

    // Allocate 2 descriptor sets (A: gen→erosionBuf, B: erosionBuf→gen)
    VkDescriptorSetLayout layouts[2] = {m_erosionDescSetLayout, m_erosionDescSetLayout};
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_erosionDescPool;
    allocInfo.descriptorSetCount = 2;
    allocInfo.pSetLayouts        = layouts;
    VkDescriptorSet sets[2];
    if (vkAllocateDescriptorSets(m_device, &allocInfo, sets) != VK_SUCCESS) return false;
    m_erosionDescSetA = sets[0];
    m_erosionDescSetB = sets[1];

    // ── Push constant range ──
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(ErosionPushConstants);

    // ── Pipeline layout ──
    VkPipelineLayoutCreateInfo pipeLayoutCI{};
    pipeLayoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeLayoutCI.setLayoutCount         = 1;
    pipeLayoutCI.pSetLayouts            = &m_erosionDescSetLayout;
    pipeLayoutCI.pushConstantRangeCount = 1;
    pipeLayoutCI.pPushConstantRanges    = &pcRange;
    if (vkCreatePipelineLayout(m_device, &pipeLayoutCI, nullptr, &m_erosionPipelineLayout) != VK_SUCCESS) return false;

    // ── Load compute shader ──
    VkShaderModule shaderMod = loadShaderModule("terrain_terrain_erosion.comp.spv");
    if (shaderMod == VK_NULL_HANDLE) {
        // Erosion not critical — just disable it silently
        m_erosionEnabled = false;
        return true;
    }

    VkComputePipelineCreateInfo pipeCI{};
    pipeCI.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeCI.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeCI.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeCI.stage.module = shaderMod;
    pipeCI.stage.pName  = "main";
    pipeCI.layout       = m_erosionPipelineLayout;
    vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &m_erosionPipeline);
    vkDestroyShaderModule(m_device, shaderMod, nullptr);

    return true;
}

// ---------------------------------------------------------------------------
// applyErosion
// ---------------------------------------------------------------------------
void TerrainPass::applyErosion(VkCommandBuffer cmd) {
    if (!m_erosionEnabled || m_erosionPipeline == VK_NULL_HANDLE) return;
    if (m_genImage == VK_NULL_HANDLE) return;

    uint32_t res = static_cast<uint32_t>(m_genParams.resolution);

    // ── Allocate ping-pong buffer if needed (same format/size as genImage) ──
    if (m_erosionImage == VK_NULL_HANDLE) {
        VkImageCreateInfo imgCI{};
        imgCI.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgCI.imageType     = VK_IMAGE_TYPE_2D;
        imgCI.format        = VK_FORMAT_R32_SFLOAT;
        imgCI.extent        = {res, res, 1};
        imgCI.mipLevels     = 1;
        imgCI.arrayLayers   = 1;
        imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;
        imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imgCI.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCreateImage(m_device, &imgCI, nullptr, &m_erosionImage);

        VkMemoryRequirements memReq;
        vkGetImageMemoryRequirements(m_device, m_erosionImage, &memReq);
        VkMemoryAllocateInfo allocI{};
        allocI.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocI.allocationSize  = memReq.size;
        // Find device-local memory type
        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProps);
        for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
            if ((memReq.memoryTypeBits & (1u << i)) &&
                (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                allocI.memoryTypeIndex = i;
                break;
            }
        }
        vkAllocateMemory(m_device, &allocI, nullptr, &m_erosionMemory);
        vkBindImageMemory(m_device, m_erosionImage, m_erosionMemory, 0);

        // Create image view
        VkImageViewCreateInfo viewCI{};
        viewCI.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewCI.image    = m_erosionImage;
        viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewCI.format   = VK_FORMAT_R32_SFLOAT;
        viewCI.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_device, &viewCI, nullptr, &m_erosionView);

        // Transition erosion buffer to GENERAL
        VkImageMemoryBarrier barrier{};
        barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        barrier.image               = m_erosionImage;
        barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask       = 0;
        barrier.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Update descriptor sets (A: genImage→erosionImage, B: erosionImage→genImage)
        auto writeStorage = [&](VkDescriptorSet ds, uint32_t binding, VkImageView view) {
            VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet  wr{};
            wr.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr.dstSet          = ds;
            wr.dstBinding      = binding;
            wr.descriptorCount = 1;
            wr.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            wr.pImageInfo      = &ii;
            vkUpdateDescriptorSets(m_device, 1, &wr, 0, nullptr);
        };
        writeStorage(m_erosionDescSetA, 0, m_genView);       // A src = gen
        writeStorage(m_erosionDescSetA, 1, m_erosionView);   // A dst = erosion
        writeStorage(m_erosionDescSetB, 0, m_erosionView);   // B src = erosion
        writeStorage(m_erosionDescSetB, 1, m_genView);       // B dst = gen
    }

    // ── Transition genImage to GENERAL for storage write ─────────────────────
    {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        b.image               = m_genImage;
        b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }

    ErosionPushConstants pc = m_erosionParams;
    pc.resolution = static_cast<int32_t>(res);
    uint32_t groups = (res + 15) / 16;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_erosionPipeline);

    for (int i = 0; i < m_erosionIterations; ++i) {
        VkDescriptorSet ds = (i % 2 == 0) ? m_erosionDescSetA : m_erosionDescSetB;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_erosionPipelineLayout, 0, 1, &ds, 0, nullptr);
        vkCmdPushConstants(cmd, m_erosionPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(ErosionPushConstants), &pc);
        vkCmdDispatch(cmd, groups, groups, 1);

        // Barrier between iterations
        VkMemoryBarrier mb{};
        mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 1, &mb, 0, nullptr, 0, nullptr);
    }

    // If odd number of iterations, result is in m_erosionImage (set A wrote to erosionBuf last).
    // If even, result is in m_genImage (set B wrote to gen last). ✓
    // We always want result in m_genImage. If erosionIterations is odd, copy back:
    if (m_erosionIterations % 2 != 0) {
        // Copy erosionImage → genImage
        VkImageMemoryBarrier barriers[2] = {};
        barriers[0].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[0].oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
        barriers[0].newLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].image            = m_erosionImage;
        barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barriers[0].srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[0].dstAccessMask    = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[1].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[1].oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
        barriers[1].newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].image            = m_genImage;
        barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barriers[1].srcAccessMask    = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barriers[1].dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 2, barriers);

        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent         = {res, res, 1};
        vkCmdCopyImage(cmd, m_erosionImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            m_genImage,  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            1, &region);

        // Transition genImage back to GENERAL
        VkImageMemoryBarrier back{};
        back.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        back.oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        back.newLayout        = VK_IMAGE_LAYOUT_GENERAL;
        back.image            = m_genImage;
        back.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        back.srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
        back.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &back);
    }

    // ── Transition genImage back to SHADER_READ_ONLY for terrain rendering ───
    {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.image               = m_genImage;
        b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &b);
    }
}

// ---------------------------------------------------------------------------
// paintSplat — CPU-side brush into RGBA8 splatmap
// ---------------------------------------------------------------------------
void TerrainPass::paintSplat(glm::vec2 worldPos, int channel, float radius,
                              float strength, float terrainSize) {
    if (channel < 0 || channel > 3) return;
    uint32_t res = m_splatResolution;

    // Lazily allocate CPU splatmap (zero = use procedural weights)
    if (m_cpuSplatmap.empty()) {
        m_cpuSplatmap.assign(static_cast<size_t>(res) * res * 4, 0u);
    }

    // World → UV → pixel
    glm::vec2 uv     = worldPos / terrainSize + 0.5f;
    glm::vec2 center = uv * static_cast<float>(res);
    float     radPx  = radius / terrainSize * static_cast<float>(res);
    float     radPx2 = radPx * radPx;

    int x0 = std::max(0, static_cast<int>(center.x - radPx));
    int x1 = std::min(static_cast<int>(res) - 1, static_cast<int>(center.x + radPx));
    int y0 = std::max(0, static_cast<int>(center.y - radPx));
    int y1 = std::min(static_cast<int>(res) - 1, static_cast<int>(center.y + radPx));

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            float dx = static_cast<float>(x) - center.x;
            float dy = static_cast<float>(y) - center.y;
            float dist2 = dx*dx + dy*dy;
            if (dist2 > radPx2) continue;
            float t   = 1.0f - std::sqrt(dist2) / radPx;  // linear falloff
            float str = strength * t * t;                   // smooth falloff

            size_t base = (static_cast<size_t>(y) * res + x) * 4;
            // Set target channel, redistribute others
            float oldTarget = m_cpuSplatmap[base + channel] / 255.0f;
            float newTarget = std::min(1.0f, oldTarget + str);
            float delta     = newTarget - oldTarget;
            m_cpuSplatmap[base + channel] = static_cast<uint8_t>(newTarget * 255.0f);

            // Reduce other channels proportionally to keep sum ≈ 1
            float otherSum = 0.0f;
            for (int c = 0; c < 4; c++) if (c != channel) otherSum += m_cpuSplatmap[base + c];
            if (otherSum > 0.001f * 255.0f) {
                float scale = std::max(0.0f, 1.0f - delta);
                for (int c = 0; c < 4; c++) {
                    if (c != channel)
                        m_cpuSplatmap[base + c] = static_cast<uint8_t>(
                            m_cpuSplatmap[base + c] * scale);
                }
            }
        }
    }
    m_splatDirty = true;
}

// ---------------------------------------------------------------------------
// flushSplatmap — allocate/update GPU splatmap from CPU data
// ---------------------------------------------------------------------------
VkImageView TerrainPass::flushSplatmap(VkCommandBuffer cmd) {
    if (m_cpuSplatmap.empty() || !m_splatDirty) return m_ownedSplatView;

    uint32_t res  = m_splatResolution;
    VkDeviceSize sz = static_cast<VkDeviceSize>(res) * res * 4;

    // ── Allocate/reuse GPU image ──────────────────────────────────────────────
    if (m_ownedSplatImage == VK_NULL_HANDLE) {
        VkImageCreateInfo imgCI{};
        imgCI.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgCI.imageType     = VK_IMAGE_TYPE_2D;
        imgCI.format        = VK_FORMAT_R8G8B8A8_UNORM;
        imgCI.extent        = {res, res, 1};
        imgCI.mipLevels     = 1;
        imgCI.arrayLayers   = 1;
        imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;
        imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imgCI.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCreateImage(m_device, &imgCI, nullptr, &m_ownedSplatImage);

        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(m_device, m_ownedSplatImage, &req);
        uint32_t memIdx = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        VkMemoryAllocateInfo ai{};
        ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = memIdx;
        vkAllocateMemory(m_device, &ai, nullptr, &m_ownedSplatMemory);
        vkBindImageMemory(m_device, m_ownedSplatImage, m_ownedSplatMemory, 0);

        VkImageViewCreateInfo vci{};
        vci.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image            = m_ownedSplatImage;
        vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        vci.format           = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_device, &vci, nullptr, &m_ownedSplatView);

        m_descriptorDirty = true;  // force re-bind new view
    }

    // ── Allocate/reuse staging buffer ────────────────────────────────────────
    if (m_splatStagingBuf == VK_NULL_HANDLE) {
        VkBufferCreateInfo bci{};
        bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size        = sz;
        bci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(m_device, &bci, nullptr, &m_splatStagingBuf);

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(m_device, m_splatStagingBuf, &req);
        uint32_t memIdx = findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        VkMemoryAllocateInfo ai{};
        ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = memIdx;
        vkAllocateMemory(m_device, &ai, nullptr, &m_splatStagingMem);
        vkBindBufferMemory(m_device, m_splatStagingBuf, m_splatStagingMem, 0);
    }

    // Map and copy CPU data
    void* ptr;
    vkMapMemory(m_device, m_splatStagingMem, 0, sz, 0, &ptr);
    std::memcpy(ptr, m_cpuSplatmap.data(), static_cast<size_t>(sz));
    vkUnmapMemory(m_device, m_splatStagingMem);

    // Transition to TRANSFER_DST, copy, transition to SHADER_READ_ONLY
    VkImageMemoryBarrier b{};
    b.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.image            = m_ownedSplatImage;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask    = 0;
    b.dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {res, res, 1};
    vkCmdCopyBufferToImage(cmd, m_splatStagingBuf, m_ownedSplatImage,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);

    m_splatDirty = false;
    // Make the new owned view the active splatmap view for getSplatmapView()
    m_splatmapView = m_ownedSplatView;
    return m_ownedSplatView;
}

// ---------------------------------------------------------------------------
// clearSplatPaint — destroy owned GPU splatmap; revert to external/procedural
// ---------------------------------------------------------------------------
void TerrainPass::clearSplatPaint() {
    m_cpuSplatmap.clear();
    m_splatDirty = false;
    if (m_ownedSplatView   != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_ownedSplatView, nullptr);
        m_ownedSplatView = VK_NULL_HANDLE;
    }
    if (m_ownedSplatImage  != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_ownedSplatImage, nullptr);
        m_ownedSplatImage = VK_NULL_HANDLE;
    }
    if (m_ownedSplatMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_ownedSplatMemory, nullptr);
        m_ownedSplatMemory = VK_NULL_HANDLE;
    }
    // Reset to the externally-set splatmap view (may be VK_NULL_HANDLE if none set)
    // The caller of setSplatMap() still owns that view; we just stop overriding it.
    // Setting m_splatmapView to VK_NULL_HANDLE will cause the terrain to revert to
    // its procedural weight blending in the fragment shader.
    m_splatmapView    = VK_NULL_HANDLE;
    m_descriptorDirty = true;
}

} // namespace ohao
