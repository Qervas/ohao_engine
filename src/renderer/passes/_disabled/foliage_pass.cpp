#include "foliage_pass.hpp"

#include <iostream>
#include <array>
#include <cstring>

namespace ohao {

// ============================================================================
//  Constants
// ============================================================================

static constexpr uint32_t kDefaultMaxInstances = 65536;

// Vertex: vec3 pos (12) + vec2 uv (8) + vec3 normal (12) = 32 bytes per vertex.
// Declared separately from FoliageInstance so it can live in local-scope
// without conflicting with the struct in the header.
struct BillboardVertex {
    float pos[3];
    float uv[2];
    float normal[3];
};
static_assert(sizeof(BillboardVertex) == 32, "BillboardVertex stride must be 32 bytes");

// ============================================================================
//  Lifecycle
// ============================================================================

FoliagePass::~FoliagePass() {
    cleanup();
}

bool FoliagePass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device         = device;
    m_physicalDevice = physicalDevice;

    if (!createBillboardMesh()) {
        std::cerr << "FoliagePass: billboard mesh creation failed\n";
        return false;
    }
    if (!createInstanceBuffer(kDefaultMaxInstances)) {
        std::cerr << "FoliagePass: instance buffer creation failed\n";
        return false;
    }
    if (!createIndirectBuffer(kDefaultMaxInstances)) {
        std::cerr << "FoliagePass: indirect buffer creation failed\n";
        return false;
    }
    if (!createFallbackTexture()) {
        std::cerr << "FoliagePass: fallback texture creation failed\n";
        return false;
    }
    if (!createCullDescriptors()) {
        std::cerr << "FoliagePass: cull descriptor creation failed\n";
        return false;
    }
    if (!createCullPipeline()) {
        std::cerr << "FoliagePass: cull pipeline creation failed\n";
        return false;
    }
    // Draw pipeline and render pass are deferred until GBuffer attachments arrive
    // (setGBufferAttachments triggers createGBufferRenderPass + createDrawPipeline).

    std::cout << "FoliagePass: OK (deferred draw pipeline until GBuffer wired)\n";
    return true;
}

void FoliagePass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);

    // Compute cull resources
    safeDestroy(m_cullPipeline);
    safeDestroy(m_cullPipelineLayout);
    safeDestroy(m_cullDescPool);
    safeDestroy(m_cullDescLayout);
    m_cullDescSet = VK_NULL_HANDLE;

    // Graphics draw resources
    safeDestroy(m_drawPipeline);
    safeDestroy(m_drawPipelineLayout);
    safeDestroy(m_drawDescPool);
    safeDestroy(m_drawDescLayout);
    m_drawDescSet = VK_NULL_HANDLE;
    destroyFramebuffer();
    safeDestroy(m_renderPass);

    // Billboard mesh
    safeDestroy(m_billboardIB);
    safeFree(m_billboardIBMem);
    safeDestroy(m_billboardVB);
    safeFree(m_billboardVBMem);

    // Instance SSBO
    if (m_instanceMapped && m_instanceMemory != VK_NULL_HANDLE) {
        vkUnmapMemory(m_device, m_instanceMemory);
        m_instanceMapped = nullptr;
    }
    safeDestroy(m_instanceBuffer);
    safeFree(m_instanceMemory);

    // Indirect buffer
    safeDestroy(m_indirectBuffer);
    safeFree(m_indirectMemory);

    // Fallback texture
    safeDestroy(m_fallbackSampler);
    safeDestroy(m_fallbackView);
    safeDestroy(m_fallbackImage);
    safeFree(m_fallbackMemory);

    m_device = VK_NULL_HANDLE;
}

// ============================================================================
//  execute() — the hot path
// ============================================================================

void FoliagePass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (!m_enabled || m_instanceCount == 0) return;
    if (m_renderPass == VK_NULL_HANDLE || m_framebuffer == VK_NULL_HANDLE) return;
    if (m_drawPipeline == VK_NULL_HANDLE) return;

    // ------------------------------------------------------------------
    // Step 0: lazy descriptor updates
    // ------------------------------------------------------------------
    if (m_cullDescDirty) {
        updateCullDescriptors();
        m_cullDescDirty = false;
    }
    if (m_drawDescDirty) {
        updateDrawDescriptors();
        m_drawDescDirty = false;
    }
    if (m_cullDescSet == VK_NULL_HANDLE || m_drawDescSet == VK_NULL_HANDLE) return;

    // ------------------------------------------------------------------
    // Step 1: Compute frustum cull → fill indirect draw buffer.
    // ------------------------------------------------------------------
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_cullPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_cullPipelineLayout, 0, 1, &m_cullDescSet, 0, nullptr);

    CullParams cp{};
    for (int i = 0; i < 6; ++i) cp.frustumPlanes[i] = m_frustumPlanes[i];
    cp.cameraPos        = m_cameraPos;
    cp.cullDistance     = m_cullDistance;
    cp.instanceCount    = m_instanceCount;
    cp.crossIndexCount  = m_crossIndexCount;
    cp.singleIndexCount = m_singleIndexCount;
    cp.terrainSize      = m_terrainSize;

    vkCmdPushConstants(cmd, m_cullPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(CullParams), &cp);

    uint32_t groupX = (m_instanceCount + 255u) / 256u;
    vkCmdDispatch(cmd, groupX, 1, 1);

    // ------------------------------------------------------------------
    // Step 2: Barrier — compute SSBO write → indirect read + vertex SSBO read.
    // ------------------------------------------------------------------
    std::array<VkBufferMemoryBarrier, 2> barriers{};

    // Indirect buffer: compute write → draw-indirect read
    barriers[0].sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barriers[0].srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[0].dstAccessMask       = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].buffer              = m_indirectBuffer;
    barriers[0].offset              = 0;
    barriers[0].size                = VK_WHOLE_SIZE;

    // Instance SSBO: compute read → vertex shader read (already readable, but
    // explicit barrier is required by the spec when queue families differ or
    // between pipeline stages on the same queue).
    barriers[1].sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barriers[1].srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    barriers[1].dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].buffer              = m_instanceBuffer;
    barriers[1].offset              = 0;
    barriers[1].size                = VK_WHOLE_SIZE;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
        0,
        0, nullptr,
        static_cast<uint32_t>(barriers.size()), barriers.data(),
        0, nullptr);

    // ------------------------------------------------------------------
    // Step 3: Graphics pass — draw foliage into GBuffer (LOAD_OP_LOAD).
    // No clear values needed because all attachments use LOAD.
    // ------------------------------------------------------------------
    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass        = m_renderPass;
    rpBegin.framebuffer       = m_framebuffer;
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = {m_width, m_height};
    rpBegin.clearValueCount   = 0;   // LOAD_OP_LOAD — no clears
    rpBegin.pClearValues      = nullptr;

    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.x        = 0.0f;
    viewport.y        = 0.0f;
    viewport.width    = static_cast<float>(m_width);
    viewport.height   = static_cast<float>(m_height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{{0, 0}, {m_width, m_height}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_drawPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_drawPipelineLayout, 0, 1, &m_drawDescSet, 0, nullptr);

    VkBuffer     vb      = m_billboardVB;
    VkDeviceSize vbOff   = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vbOff);
    vkCmdBindIndexBuffer(cmd, m_billboardIB, 0, VK_INDEX_TYPE_UINT16);

    FoliagePC pc{};
    pc.viewProj     = m_viewProj;
    pc.cameraPos    = m_cameraPos;
    pc.time         = m_time;
    pc.windDir      = m_windDir;
    pc.windStrength = m_windStrength;

    vkCmdPushConstants(cmd, m_drawPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(FoliagePC), &pc);

    // One indirect command per instance — the compute shader zeroed instanceCount
    // for culled blades so the GPU skips them.
    vkCmdDrawIndexedIndirect(cmd, m_indirectBuffer, 0,
                             m_instanceCount,
                             static_cast<uint32_t>(sizeof(VkDrawIndexedIndirectCommand)));

    vkCmdEndRenderPass(cmd);
}

// ============================================================================
//  Resize
// ============================================================================

void FoliagePass::onResize(uint32_t width, uint32_t height) {
    m_width  = width;
    m_height = height;
    // GBuffer views will be re-wired externally; framebuffer rebuild happens
    // in setGBufferAttachments().
}

// ============================================================================
//  Public setters
// ============================================================================

void FoliagePass::uploadInstances(const std::vector<FoliageInstance>& instances) {
    if (instances.empty()) { clearInstances(); return; }

    if (m_instanceMapped == nullptr || m_maxInstances == 0) return;

    uint32_t count = static_cast<uint32_t>(
        std::min<size_t>(instances.size(), m_maxInstances));
    memcpy(m_instanceMapped, instances.data(), count * sizeof(FoliageInstance));
    m_instanceCount  = count;
    m_cullDescDirty  = true;  // descriptor unchanged but touch for safety
    m_drawDescDirty  = true;
}

void FoliagePass::clearInstances() {
    m_instanceCount = 0;
}

void FoliagePass::setGBufferAttachments(VkImageView posView,    VkImageView normalView,
                                         VkImageView albedoView, VkImageView velView,
                                         VkImageView depthView,
                                         VkFormat colorFmt,     VkFormat depthFmt) {
    m_gbufPos    = posView;
    m_gbufNormal = normalView;
    m_gbufAlbedo = albedoView;
    m_gbufVel    = velView;
    m_gbufDepth  = depthView;
    m_colorFmt   = colorFmt;
    m_depthFmt   = depthFmt;

    // Rebuild render pass and framebuffer whenever attachments change.
    destroyFramebuffer();
    safeDestroy(m_renderPass);

    if (!createGBufferRenderPass()) {
        std::cerr << "FoliagePass: GBuffer render pass creation failed\n";
        return;
    }
    if (!createFramebuffer()) {
        std::cerr << "FoliagePass: framebuffer creation failed\n";
        return;
    }

    // Build draw pipeline now that we have a valid render pass.
    if (m_drawPipeline == VK_NULL_HANDLE) {
        if (!createDrawDescriptors()) {
            std::cerr << "FoliagePass: draw descriptor creation failed\n";
            return;
        }
        if (!createDrawPipeline()) {
            std::cerr << "FoliagePass: draw pipeline creation failed\n";
            return;
        }
    }

    m_drawDescDirty = true;
}

void FoliagePass::setGrassTexture(VkImageView view, VkSampler sampler) {
    m_grassTexView  = view;
    m_grassSampler  = sampler;
    m_drawDescDirty = true;
}

void FoliagePass::setMatrices(const glm::mat4& viewProj, const glm::vec3& camPos) {
    m_viewProj  = viewProj;
    m_cameraPos = camPos;
}

void FoliagePass::setFrustumPlanes(const std::array<glm::vec4, 6>& planes) {
    m_frustumPlanes = planes;
}

void FoliagePass::setWind(const glm::vec3& dir, float strength, float time) {
    m_windDir      = dir;
    m_windStrength = strength;
    m_time         = time;
}

// ============================================================================
//  createBillboardMesh()
//
//  Cross-quad layout (world local space, billboard expanded in vertex shader):
//
//  Quad A (facing +Z, lying in XY plane):
//    v0 = (-0.5,  0,  0)    v1 = (0.5,  0,  0)
//    v2 = (-0.5,  1,  0)    v3 = (0.5,  1,  0)
//    triangles: (0,1,2), (1,3,2)
//
//  Quad B (facing +X, lying in ZY plane):
//    v4 = (0,  0, -0.5)     v5 = (0,  0,  0.5)
//    v6 = (0,  1, -0.5)     v7 = (0,  1,  0.5)
//    triangles: (4,5,6), (5,7,6)
//
//  The vertex shader maps pos.x → right-axis and pos.y → up-axis, so the
//  z component of quad B becomes the right contribution and the existing
//  vertex shader formula naturally handles both quads.
//
//  Single-quad is just verts 0-3 with 6 indices.
// ============================================================================

bool FoliagePass::createBillboardMesh() {
    // 8 verts: quad A (0-3), quad B (4-7)
    // Normals: quad A faces +Z (0,0,1), quad B faces +X (1,0,0).
    BillboardVertex verts[8] = {
        // Quad A
        {{-0.5f, 0.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}},  // v0 root-left
        {{ 0.5f, 0.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}},  // v1 root-right
        {{-0.5f, 1.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},  // v2 tip-left
        {{ 0.5f, 1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},  // v3 tip-right
        // Quad B (90° rotated in XZ — x→z, z→-x so right-axis still works)
        {{ 0.0f, 0.0f, -0.5f}, {0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},  // v4
        {{ 0.0f, 0.0f,  0.5f}, {1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},  // v5
        {{ 0.0f, 1.0f, -0.5f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},  // v6
        {{ 0.0f, 1.0f,  0.5f}, {1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},  // v7
    };

    // Indices: 12 for cross-quad (stored first), 6 for single quad (subset of first 6)
    // Index list: [A0,A1,..., B0,B1,...] — single quad uses only first 6.
    uint16_t indices[12] = {
        0, 1, 2,   1, 3, 2,   // Quad A
        4, 5, 6,   5, 7, 6,   // Quad B
    };

    m_crossIndexCount  = 12;
    m_singleIndexCount = 6;

    VkDeviceSize vbSize = sizeof(verts);
    VkDeviceSize ibSize = sizeof(indices);

    // Upload vertex buffer (device-local via staging)
    if (!createGPUBuffer(vbSize,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            m_billboardVB, m_billboardVBMem)) return false;

    if (!uploadBufferViaStagingBuffer(m_billboardVB, vbSize, verts,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return false;

    // Upload index buffer (device-local via staging)
    if (!createGPUBuffer(ibSize,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            m_billboardIB, m_billboardIBMem)) return false;

    if (!uploadBufferViaStagingBuffer(m_billboardIB, ibSize, indices,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) return false;

    return true;
}

// ============================================================================
//  createInstanceBuffer()
// ============================================================================

bool FoliagePass::createInstanceBuffer(uint32_t maxInstances) {
    m_maxInstances = maxInstances;
    VkDeviceSize size = static_cast<VkDeviceSize>(maxInstances) * sizeof(FoliageInstance);

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size        = size;
    bufInfo.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufInfo, nullptr, &m_instanceBuffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, m_instanceBuffer, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_instanceMemory) != VK_SUCCESS)
        return false;

    vkBindBufferMemory(m_device, m_instanceBuffer, m_instanceMemory, 0);
    vkMapMemory(m_device, m_instanceMemory, 0, size, 0, &m_instanceMapped);

    return true;
}

// ============================================================================
//  createIndirectBuffer() — device-local, written by compute shader
// ============================================================================

bool FoliagePass::createIndirectBuffer(uint32_t maxInstances) {
    VkDeviceSize size = static_cast<VkDeviceSize>(maxInstances)
                      * sizeof(VkDrawIndexedIndirectCommand);

    return createGPUBuffer(size,
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        m_indirectBuffer, m_indirectMemory);
}

// ============================================================================
//  createFallbackTexture() — 1×1 opaque white texture for when no grass
//  texture has been assigned.
// ============================================================================

bool FoliagePass::createFallbackTexture() {
    VkImageCreateInfo imgInfo{};
    imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType     = VK_IMAGE_TYPE_2D;
    imgInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
    imgInfo.extent        = {1, 1, 1};
    imgInfo.mipLevels     = 1;
    imgInfo.arrayLayers   = 1;
    imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_device, &imgInfo, nullptr, &m_fallbackImage) != VK_SUCCESS)
        return false;

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(m_device, m_fallbackImage, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_fallbackMemory) != VK_SUCCESS)
        return false;

    vkBindImageMemory(m_device, m_fallbackImage, m_fallbackMemory, 0);

    // Upload 1×1 white RGBA via staging buffer
    uint32_t white = 0xFFFFFFFFu;
    {
        VkBuffer       stagBuf;
        VkDeviceMemory stagMem;
        if (!createGPUBuffer(4,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                stagBuf, stagMem)) return false;

        void* mapped;
        vkMapMemory(m_device, stagMem, 0, 4, 0, &mapped);
        memcpy(mapped, &white, 4);
        vkUnmapMemory(m_device, stagMem);

        // We need a one-time command buffer to copy and transition the image.
        // Because FoliagePass does not own a command pool, we create a temporary one.
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = 0;  // graphics queue assumed to be family 0

        VkCommandPool tmpPool;
        if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &tmpPool) != VK_SUCCESS) {
            vkDestroyBuffer(m_device, stagBuf, nullptr);
            vkFreeMemory(m_device, stagMem, nullptr);
            return false;
        }

        VkCommandBufferAllocateInfo cbAlloc{};
        cbAlloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbAlloc.commandPool        = tmpPool;
        cbAlloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAlloc.commandBufferCount = 1;

        VkCommandBuffer cb;
        vkAllocateCommandBuffers(m_device, &cbAlloc, &cb);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &beginInfo);

        // Transition UNDEFINED → TRANSFER_DST
        VkImageMemoryBarrier barrier{};
        barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = m_fallbackImage;
        barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask       = 0;
        barrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Copy
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent      = {1, 1, 1};
        vkCmdCopyBufferToImage(cb, stagBuf, m_fallbackImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        // Transition TRANSFER_DST → SHADER_READ_ONLY
        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        vkEndCommandBuffer(cb);

        // Get the graphics queue (queue family 0, queue 0)
        VkQueue graphicsQueue;
        vkGetDeviceQueue(m_device, 0, 0, &graphicsQueue);

        VkSubmitInfo submit{};
        submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers    = &cb;
        vkQueueSubmit(graphicsQueue, 1, &submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(graphicsQueue);

        vkDestroyCommandPool(m_device, tmpPool, nullptr);
        vkDestroyBuffer(m_device, stagBuf, nullptr);
        vkFreeMemory(m_device, stagMem, nullptr);
    }

    // Image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                           = m_fallbackImage;
    viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                          = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = 1;
    if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_fallbackView) != VK_SUCCESS)
        return false;

    // Sampler
    VkSamplerCreateInfo sampInfo{};
    sampInfo.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampInfo.magFilter        = VK_FILTER_LINEAR;
    sampInfo.minFilter        = VK_FILTER_LINEAR;
    sampInfo.addressModeU     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.addressModeV     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.addressModeW     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampInfo.anisotropyEnable = VK_FALSE;
    sampInfo.maxAnisotropy    = 1.0f;
    sampInfo.borderColor      = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    if (vkCreateSampler(m_device, &sampInfo, nullptr, &m_fallbackSampler) != VK_SUCCESS)
        return false;

    return true;
}

// ============================================================================
//  createCullDescriptors() — two SSBOs: instance (in) + indirect (out)
// ============================================================================

bool FoliagePass::createCullDescriptors() {
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};

    // Binding 0: instance SSBO (readonly in compute)
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 1: indirect draw buffer SSBO (write in compute)
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 2: splatmap (combined image sampler) for density-aware cull
    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_cullDescLayout)
            != VK_SUCCESS) return false;

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         2};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes    = poolSizes.data();
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_cullDescPool)
            != VK_SUCCESS) return false;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_cullDescPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_cullDescLayout;
    if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_cullDescSet)
            != VK_SUCCESS) return false;

    // Write immediately (buffers are already allocated at this point)
    updateCullDescriptors();
    m_cullDescDirty = false;
    return true;
}

// ============================================================================
//  createDrawDescriptors() — instance SSBO (binding 0) + grass CIS (binding 1)
// ============================================================================

bool FoliagePass::createDrawDescriptors() {
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};

    // Binding 0: instance SSBO (read in vertex shader)
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;

    // Binding 1: grass texture (combined image sampler in fragment shader)
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_drawDescLayout)
            != VK_SUCCESS) return false;

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,          1};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,  1};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes    = poolSizes.data();
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_drawDescPool)
            != VK_SUCCESS) return false;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_drawDescPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_drawDescLayout;
    if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_drawDescSet)
            != VK_SUCCESS) return false;

    updateDrawDescriptors();
    m_drawDescDirty = false;
    return true;
}

// ============================================================================
//  createCullPipeline()
// ============================================================================

bool FoliagePass::createCullPipeline() {
    return createComputePipeline(
        "foliage_foliage_cull.comp.spv",
        m_cullDescLayout,
        static_cast<uint32_t>(sizeof(CullParams)),
        m_cullPipeline, m_cullPipelineLayout);
}

// ============================================================================
//  createGBufferRenderPass() — LOAD_OP_LOAD on all 4 colour + depth read-only.
//  Same attachment layout as TerrainPass (which appends to the GBuffer too).
// ============================================================================

bool FoliagePass::createGBufferRenderPass() {
    // 4 colour MRTs + 1 depth — formats must match GBufferPass exactly.
    std::array<VkAttachmentDescription, 5> attachments{};

    // 0: Position + Metallic (R16G16B16A16_SFLOAT)
    attachments[0].format         = m_colorFmt;  // R16G16B16A16_SFLOAT
    attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;   // preserve GBuffer contents
    attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    attachments[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // 1: Normal + Roughness (A2R10G10B10_UNORM_PACK32)
    attachments[1].format         = VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    attachments[1].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // 2: Albedo + AO (R8G8B8A8_SRGB)
    attachments[2].format         = VK_FORMAT_R8G8B8A8_SRGB;
    attachments[2].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[2].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[2].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[2].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[2].initialLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    attachments[2].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // 3: Velocity (R16G16_SFLOAT)
    attachments[3].format         = VK_FORMAT_R16G16_SFLOAT;
    attachments[3].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[3].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[3].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[3].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[3].initialLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    attachments[3].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // 4: Depth (read-only — foliage reads existing depth for discard, writes new)
    attachments[4].format         = m_depthFmt;
    attachments[4].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[4].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[4].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[4].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[4].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[4].initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    attachments[4].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    std::array<VkAttachmentReference, 4> colorRefs{};
    for (uint32_t i = 0; i < 4; ++i) {
        colorRefs[i].attachment = i;
        colorRefs[i].layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    VkAttachmentReference depthRef{};
    depthRef.attachment = 4;
    depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = static_cast<uint32_t>(colorRefs.size());
    subpass.pColorAttachments       = colorRefs.data();
    subpass.pDepthStencilAttachment = &depthRef;

    // Dependencies: external → foliage (reads GBuffer) and foliage → external
    std::array<VkSubpassDependency, 2> deps{};

    deps[0].srcSubpass      = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass      = 0;
    deps[0].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                            | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                            | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                            | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                            | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                            | VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                            | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                            | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                            | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    deps[1].srcSubpass      = 0;
    deps[1].dstSubpass      = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                            | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].dstStageMask    = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                            | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask   = VK_ACCESS_SHADER_READ_BIT;
    deps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    rpInfo.pAttachments    = attachments.data();
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;
    rpInfo.dependencyCount = static_cast<uint32_t>(deps.size());
    rpInfo.pDependencies   = deps.data();

    return vkCreateRenderPass(m_device, &rpInfo, nullptr, &m_renderPass) == VK_SUCCESS;
}

// ============================================================================
//  createFramebuffer()
// ============================================================================

bool FoliagePass::createFramebuffer() {
    if (m_gbufPos    == VK_NULL_HANDLE || m_gbufNormal == VK_NULL_HANDLE ||
        m_gbufAlbedo == VK_NULL_HANDLE || m_gbufVel    == VK_NULL_HANDLE ||
        m_gbufDepth  == VK_NULL_HANDLE) return false;

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

void FoliagePass::destroyFramebuffer() {
    safeDestroy(m_framebuffer);
}

// ============================================================================
//  createDrawPipeline()
// ============================================================================

bool FoliagePass::createDrawPipeline() {
    VkShaderModule vertShader = loadShaderModule("foliage_foliage.vert.spv");
    VkShaderModule fragShader = loadShaderModule("foliage_foliage.frag.spv");

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertShader;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragShader;
    stages[1].pName  = "main";

    // --- Vertex input: one binding, 32 bytes per vertex (BillboardVertex) ---
    VkVertexInputBindingDescription bindDesc{};
    bindDesc.binding   = 0;
    bindDesc.stride    = sizeof(BillboardVertex);  // 32 bytes
    bindDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 3> attrDescs{};
    // location 0: pos  (vec3, offset 0)
    attrDescs[0].location = 0;
    attrDescs[0].binding  = 0;
    attrDescs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
    attrDescs[0].offset   = offsetof(BillboardVertex, pos);
    // location 1: uv   (vec2, offset 12)
    attrDescs[1].location = 1;
    attrDescs[1].binding  = 0;
    attrDescs[1].format   = VK_FORMAT_R32G32_SFLOAT;
    attrDescs[1].offset   = offsetof(BillboardVertex, uv);
    // location 2: normal (vec3, offset 20)
    attrDescs[2].location = 2;
    attrDescs[2].binding  = 0;
    attrDescs[2].format   = VK_FORMAT_R32G32B32_SFLOAT;
    attrDescs[2].offset   = offsetof(BillboardVertex, normal);

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &bindDesc;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrDescs.size());
    vertexInput.pVertexAttributeDescriptions    = attrDescs.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode    = VK_CULL_MODE_NONE;   // both sides visible for billboards
    rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth   = 1.0f;

    // Alpha-to-coverage for smoother foliage silhouette anti-aliasing.
    // The fragment shader performs a hard discard(alpha < 0.5) but
    // alpha-to-coverage softens the edges when MSAA is in use.
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples  = VK_SAMPLE_COUNT_1_BIT;
    multisampling.alphaToCoverageEnable = VK_TRUE;   // foliage edge AA
    multisampling.alphaToOneEnable      = VK_FALSE;
    multisampling.sampleShadingEnable   = VK_FALSE;

    // Depth test ON, depth write ON (foliage writes depth for correct occlusion).
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS;

    // No blending — GBuffer pass is fully opaque (alpha cutout via discard)
    std::array<VkPipelineColorBlendAttachmentState, 4> blendAttachments{};
    for (auto& a : blendAttachments) {
        a.blendEnable    = VK_FALSE;
        a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                         | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
    colorBlend.pAttachments    = blendAttachments.data();

    std::array<VkDynamicState, 2> dynStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynState{};
    dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.dynamicStateCount = static_cast<uint32_t>(dynStates.size());
    dynState.pDynamicStates    = dynStates.data();

    // Push constants: vertex + fragment stages share the same block
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset     = 0;
    pc.size       = sizeof(FoliagePC);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &m_drawDescLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pc;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_drawPipelineLayout)
            != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, vertShader, nullptr);
        vkDestroyShaderModule(m_device, fragShader, nullptr);
        return false;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount          = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages             = stages.data();
    pipelineInfo.pVertexInputState   = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState   = &multisampling;
    pipelineInfo.pDepthStencilState  = &depthStencil;
    pipelineInfo.pColorBlendState    = &colorBlend;
    pipelineInfo.pDynamicState       = &dynState;
    pipelineInfo.layout              = m_drawPipelineLayout;
    pipelineInfo.renderPass          = m_renderPass;
    pipelineInfo.subpass             = 0;

    VkResult res = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1,
                                             &pipelineInfo, nullptr, &m_drawPipeline);
    vkDestroyShaderModule(m_device, vertShader, nullptr);
    vkDestroyShaderModule(m_device, fragShader, nullptr);

    return res == VK_SUCCESS;
}

// ============================================================================
//  Descriptor update helpers
// ============================================================================

void FoliagePass::updateCullDescriptors() {
    if (m_cullDescSet    == VK_NULL_HANDLE) return;
    if (m_instanceBuffer == VK_NULL_HANDLE) return;
    if (m_indirectBuffer == VK_NULL_HANDLE) return;

    std::array<VkDescriptorBufferInfo, 2> bufInfos{};
    bufInfos[0].buffer = m_instanceBuffer;
    bufInfos[0].offset = 0;
    bufInfos[0].range  = VK_WHOLE_SIZE;
    bufInfos[1].buffer = m_indirectBuffer;
    bufInfos[1].offset = 0;
    bufInfos[1].range  = VK_WHOLE_SIZE;

    // Binding 2: splatmap — fall back to fallback texture (1×1 white) when no splatmap.
    // Shader handles splatSum == 0 gracefully (treats as all-grass, allows everywhere).
    VkImageView  splatView    = (m_splatmapView    != VK_NULL_HANDLE) ? m_splatmapView    : m_fallbackView;
    VkSampler    splatSampler = (m_splatmapSampler != VK_NULL_HANDLE) ? m_splatmapSampler : m_fallbackSampler;
    VkDescriptorImageInfo splatInfo{};
    splatInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    splatInfo.imageView   = splatView;
    splatInfo.sampler     = splatSampler;

    std::array<VkWriteDescriptorSet, 3> writes{};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = m_cullDescSet;
        writes[i].dstBinding      = i;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].descriptorCount = 1;
        writes[i].pBufferInfo     = &bufInfos[i];
    }
    writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet          = m_cullDescSet;
    writes[2].dstBinding      = 2;
    writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].descriptorCount = 1;
    writes[2].pImageInfo      = &splatInfo;

    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

void FoliagePass::updateDrawDescriptors() {
    if (m_drawDescSet    == VK_NULL_HANDLE) return;
    if (m_instanceBuffer == VK_NULL_HANDLE) return;

    // Choose the active grass texture or fall back to the white 1×1
    VkImageView  texView    = (m_grassTexView    != VK_NULL_HANDLE) ? m_grassTexView    : m_fallbackView;
    VkSampler    texSampler = (m_grassSampler    != VK_NULL_HANDLE) ? m_grassSampler    : m_fallbackSampler;

    VkDescriptorBufferInfo instBuf{};
    instBuf.buffer = m_instanceBuffer;
    instBuf.offset = 0;
    instBuf.range  = VK_WHOLE_SIZE;

    VkDescriptorImageInfo texInfo{};
    texInfo.sampler     = texSampler;
    texInfo.imageView   = texView;
    texInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = m_drawDescSet;
    writes[0].dstBinding      = 0;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo     = &instBuf;

    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = m_drawDescSet;
    writes[1].dstBinding      = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo      = &texInfo;

    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

// ============================================================================
//  Low-level GPU buffer helpers
// ============================================================================

bool FoliagePass::createGPUBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                   VkMemoryPropertyFlags props,
                                   VkBuffer& outBuffer, VkDeviceMemory& outMemory) {
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size        = size;
    bufInfo.usage       = usage;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufInfo, nullptr, &outBuffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(m_device, outBuffer, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize  = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, props);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &outMemory) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, outBuffer, nullptr);
        outBuffer = VK_NULL_HANDLE;
        return false;
    }

    vkBindBufferMemory(m_device, outBuffer, outMemory, 0);
    return true;
}

bool FoliagePass::uploadBufferViaStagingBuffer(VkBuffer dst, VkDeviceSize size,
                                                const void* data,
                                                VkBufferUsageFlags /*dstUsage*/) {
    // 1. Create a host-visible staging buffer.
    VkBuffer       stagBuf;
    VkDeviceMemory stagMem;
    if (!createGPUBuffer(size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stagBuf, stagMem)) return false;

    void* mapped;
    vkMapMemory(m_device, stagMem, 0, size, 0, &mapped);
    memcpy(mapped, data, static_cast<size_t>(size));
    vkUnmapMemory(m_device, stagMem);

    // 2. One-shot command buffer on queue family 0 (graphics).
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = 0;

    VkCommandPool tmpPool;
    if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &tmpPool) != VK_SUCCESS) {
        vkDestroyBuffer(m_device, stagBuf, nullptr);
        vkFreeMemory(m_device, stagMem, nullptr);
        return false;
    }

    VkCommandBufferAllocateInfo cbAlloc{};
    cbAlloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAlloc.commandPool        = tmpPool;
    cbAlloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAlloc.commandBufferCount = 1;

    VkCommandBuffer cb;
    vkAllocateCommandBuffers(m_device, &cbAlloc, &cb);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &beginInfo);

    VkBufferCopy copyRegion{0, 0, size};
    vkCmdCopyBuffer(cb, stagBuf, dst, 1, &copyRegion);
    vkEndCommandBuffer(cb);

    VkQueue graphicsQueue;
    vkGetDeviceQueue(m_device, 0, 0, &graphicsQueue);

    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cb;
    vkQueueSubmit(graphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue);

    vkDestroyCommandPool(m_device, tmpPool, nullptr);
    vkDestroyBuffer(m_device, stagBuf, nullptr);
    vkFreeMemory(m_device, stagMem, nullptr);

    return true;
}

} // namespace ohao
