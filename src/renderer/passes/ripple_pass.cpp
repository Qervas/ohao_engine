#include "ripple_pass.hpp"
#include <array>
#include <functional>
#include <iostream>
#include <cstring>

namespace ohao {

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

RipplePass::~RipplePass() {
    cleanup();
}

bool RipplePass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device         = device;
    m_physicalDevice = physicalDevice;

    if (!createImages()) {
        std::cerr << "RipplePass: image creation failed" << std::endl;
        return false;
    }
    if (!createDescriptors()) {
        std::cerr << "RipplePass: descriptor creation failed" << std::endl;
        return false;
    }
    if (!createPipeline()) {
        std::cerr << "RipplePass: pipeline creation failed" << std::endl;
        return false;
    }

    std::cout << "RipplePass: OK" << std::endl;
    return true;
}

void RipplePass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);

    safeDestroy(m_viewA);
    safeDestroy(m_viewB);
    safeDestroy(m_imageA);
    safeDestroy(m_imageB);
    safeFree(m_memA);
    safeFree(m_memB);

    safeDestroy(m_pipeline);
    safeDestroy(m_pipelineLayout);
    safeDestroy(m_descPool);
    safeDestroy(m_descLayout);
    m_descSetAB = VK_NULL_HANDLE;
    m_descSetBA = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Execute
// ---------------------------------------------------------------------------

void RipplePass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (!m_enabled) return;
    if (m_pipeline == VK_NULL_HANDLE) return;

    if (m_descDirty) {
        updateDescriptors();
        m_descDirty = false;
    }

    // Select ping-pong descriptor set
    VkDescriptorSet activeSet = m_pingPong ? m_descSetBA : m_descSetAB;
    if (activeSet == VK_NULL_HANDLE) return;

    // src image (srcHeight) needs GENERAL layout for imageLoad
    // dst image (dstHeight) needs GENERAL layout for imageStore
    // Both images are always kept in GENERAL after initialization.
    // No barriers needed within ping-pong (same layout each frame).

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &activeSet, 0, nullptr);

    // Build push constants
    RipplePC pc{};
    pc.terrainSize   = m_terrainSize;
    pc.damping       = m_damping;
    pc.waveSpeed     = m_waveSpeed;
    pc.dt            = m_dt;
    pc.rippleCount   = static_cast<uint32_t>(
                            glm::min(static_cast<int>(m_pendingSources.size()),
                                     static_cast<int>(MAX_SOURCES)));
    pc.mapSize       = MAP_SIZE;
    pc.pad0          = 0.0f;
    pc.pad1          = 0.0f;

    // Pack sources: each source is (pos.x, pos.y, strength, radius) = 4 floats
    for (uint32_t i = 0; i < pc.rippleCount; i++) {
        const auto& s          = m_pendingSources[i];
        pc.sourceData[i * 4 + 0] = s.pos.x;
        pc.sourceData[i * 4 + 1] = s.pos.y;
        pc.sourceData[i * 4 + 2] = s.strength;
        pc.sourceData[i * 4 + 3] = s.radius;
    }

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(RipplePC), &pc);

    uint32_t groups = (MAP_SIZE + 15u) / 16u;
    vkCmdDispatch(cmd, groups, groups, 1);

    // Memory barrier: ensure write completes before next frame reads
    VkMemoryBarrier barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
            | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);

    // Swap ping-pong and clear pending impulses
    m_pingPong = !m_pingPong;
    m_pendingSources.clear();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

VkImageView RipplePass::getRippleMapView() const {
    // Return whichever image was written last (the 'dst' of the previous frame)
    return m_pingPong ? m_viewA : m_viewB;
}

void RipplePass::addRipple(glm::vec2 worldPosXZ, float strength, float radius) {
    if (m_pendingSources.size() >= MAX_SOURCES) return;
    m_pendingSources.push_back({worldPosXZ, strength, radius});
}

void RipplePass::clearRipples() {
    m_pendingSources.clear();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

static bool createR16Image(VkDevice device, VkPhysicalDevice physDev,
                            uint32_t size,
                            VkImage& outImg, VkDeviceMemory& outMem, VkImageView& outView,
                            std::function<uint32_t(uint32_t, VkMemoryPropertyFlags)> findMem) {
    VkImageCreateInfo imgInfo{};
    imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType     = VK_IMAGE_TYPE_2D;
    imgInfo.format        = VK_FORMAT_R16_SFLOAT;
    imgInfo.extent        = {size, size, 1};
    imgInfo.mipLevels     = 1;
    imgInfo.arrayLayers   = 1;
    imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage         = VK_IMAGE_USAGE_STORAGE_BIT
                          | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &imgInfo, nullptr, &outImg) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, outImg, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = findMem(req.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &alloc, nullptr, &outMem) != VK_SUCCESS)
        return false;
    vkBindImageMemory(device, outImg, outMem, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                           = outImg;
    viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                          = VK_FORMAT_R16_SFLOAT;
    viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = 1;
    return vkCreateImageView(device, &viewInfo, nullptr, &outView) == VK_SUCCESS;
}

bool RipplePass::createImages() {
    auto findMem = [this](uint32_t filter, VkMemoryPropertyFlags props) {
        return findMemoryType(filter, props);
    };

    if (!createR16Image(m_device, m_physicalDevice, MAP_SIZE,
                        m_imageA, m_memA, m_viewA, findMem))
        return false;
    if (!createR16Image(m_device, m_physicalDevice, MAP_SIZE,
                        m_imageB, m_memB, m_viewB, findMem))
        return false;

    // Transition both images from UNDEFINED → GENERAL (persistent storage layout)
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(m_device, 0, 0, &queue);

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo cpInfo{};
    cpInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpInfo.queueFamilyIndex = 0;
    cpInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (vkCreateCommandPool(m_device, &cpInfo, nullptr, &cmdPool) != VK_SUCCESS)
        return false;

    VkCommandBuffer uploadCmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbAlloc{};
    cbAlloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAlloc.commandPool        = cmdPool;
    cbAlloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAlloc.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(m_device, &cbAlloc, &uploadCmd) != VK_SUCCESS) {
        vkDestroyCommandPool(m_device, cmdPool, nullptr);
        return false;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(uploadCmd, &beginInfo);

    std::array<VkImageMemoryBarrier, 2> barriers{};
    for (int i = 0; i < 2; i++) {
        barriers[i].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[i].oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[i].newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].image               = (i == 0) ? m_imageA : m_imageB;
        barriers[i].subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barriers[i].srcAccessMask       = 0;
        barriers[i].dstAccessMask       = VK_ACCESS_SHADER_READ_BIT
                                        | VK_ACCESS_SHADER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(uploadCmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr,
        static_cast<uint32_t>(barriers.size()), barriers.data());

    vkEndCommandBuffer(uploadCmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &uploadCmd;
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkDestroyCommandPool(m_device, cmdPool, nullptr);

    m_imagesInitialized = true;
    return true;
}

bool RipplePass::createDescriptors() {
    // 2 bindings: srcHeight (binding 0) and dstHeight (binding 1), both STORAGE_IMAGE
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descLayout) != VK_SUCCESS)
        return false;

    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSize.descriptorCount = 4;  // 2 sets × 2 bindings

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 2;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descPool) != VK_SUCCESS)
        return false;

    // Allocate 2 descriptor sets (AB and BA)
    std::array<VkDescriptorSetLayout, 2> layouts = {m_descLayout, m_descLayout};
    std::array<VkDescriptorSet, 2> sets{};
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descPool;
    allocInfo.descriptorSetCount = 2;
    allocInfo.pSetLayouts        = layouts.data();
    if (vkAllocateDescriptorSets(m_device, &allocInfo, sets.data()) != VK_SUCCESS)
        return false;

    m_descSetAB = sets[0];
    m_descSetBA = sets[1];

    updateDescriptors();
    return true;
}

bool RipplePass::createPipeline() {
    return createComputePipeline("water_water_ripple.comp.spv",
                                 m_descLayout,
                                 sizeof(RipplePC),
                                 m_pipeline, m_pipelineLayout);
}

void RipplePass::updateDescriptors() {
    if (!m_descSetAB || !m_descSetBA || !m_viewA || !m_viewB) return;

    // AB set: A=src (binding 0), B=dst (binding 1)
    // BA set: B=src (binding 0), A=dst (binding 1)
    std::array<VkDescriptorImageInfo, 4> imgInfos{};
    imgInfos[0] = {VK_NULL_HANDLE, m_viewA, VK_IMAGE_LAYOUT_GENERAL};
    imgInfos[1] = {VK_NULL_HANDLE, m_viewB, VK_IMAGE_LAYOUT_GENERAL};
    imgInfos[2] = {VK_NULL_HANDLE, m_viewB, VK_IMAGE_LAYOUT_GENERAL};
    imgInfos[3] = {VK_NULL_HANDLE, m_viewA, VK_IMAGE_LAYOUT_GENERAL};

    std::array<VkWriteDescriptorSet, 4> writes{};
    for (int i = 0; i < 4; i++) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = (i < 2) ? m_descSetAB : m_descSetBA;
        writes[i].dstBinding      = static_cast<uint32_t>(i % 2);
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo      = &imgInfos[i];
    }
    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
    m_descDirty = false;
}

bool RipplePass::reloadShader(const std::string& spvPath) {
    return reloadComputeShader(spvPath, m_descLayout, sizeof(RipplePC),
                               m_pipeline, m_pipelineLayout);
}

} // namespace ohao
