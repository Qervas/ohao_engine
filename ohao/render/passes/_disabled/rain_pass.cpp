#include "rain_pass.hpp"
#include <iostream>
#include <array>

namespace ohao {

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

RainPass::~RainPass() {
    cleanup();
}

bool RainPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device         = device;
    m_physicalDevice = physicalDevice;

    if (!createDescriptors()) {
        std::cerr << "RainPass: descriptor creation failed" << std::endl;
        return false;
    }

    if (!createPipelineResources()) {
        std::cerr << "RainPass: pipeline creation failed" << std::endl;
        return false;
    }

    std::cout << "RainPass: OK" << std::endl;
    return true;
}

void RainPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);

    safeDestroy(m_pipeline);
    safeDestroy(m_pipelineLayout);
    safeDestroy(m_descriptorPool);
    safeDestroy(m_descriptorLayout);
    m_descriptorSet = VK_NULL_HANDLE;  // freed with pool
}

// ---------------------------------------------------------------------------
// Execute
// ---------------------------------------------------------------------------

void RainPass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (!m_enabled || m_intensity < 0.001f) return;
    if (m_hdrImage == VK_NULL_HANDLE || m_hdrView == VK_NULL_HANDLE) return;

    // Update descriptor if HDR view changed (resize or initial bind)
    if (m_descriptorDirty) {
        updateDescriptors();
        m_descriptorDirty = false;
    }
    if (m_descriptorSet == VK_NULL_HANDLE) return;

    // 1. Transition HDR: SHADER_READ_ONLY_OPTIMAL → GENERAL
    //    (sky render pass leaves image in SHADER_READ_ONLY; compute needs GENERAL)
    VkImageMemoryBarrier toGeneral{};
    toGeneral.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGeneral.oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toGeneral.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.image               = m_hdrImage;
    toGeneral.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toGeneral.srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                  | VK_ACCESS_SHADER_READ_BIT;
    toGeneral.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT
                                  | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
            | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toGeneral);

    // 2. Dispatch compute
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    float aspect = (m_height > 0)
        ? static_cast<float>(m_width) / static_cast<float>(m_height)
        : 1.777f;  // fallback 16:9

    RainParams params{};
    params.time      = m_time;
    params.intensity = m_intensity;
    params.windX     = m_windX;
    params.aspect    = aspect;

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(RainParams), &params);

    uint32_t groupX = (m_width  + 7u) / 8u;
    uint32_t groupY = (m_height + 7u) / 8u;
    vkCmdDispatch(cmd, groupX, groupY, 1);

    // 3. Transition HDR: GENERAL → SHADER_READ_ONLY_OPTIMAL
    //    (particle render pass and post-processing expect SHADER_READ_ONLY)
    VkImageMemoryBarrier toReadOnly{};
    toReadOnly.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toReadOnly.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    toReadOnly.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toReadOnly.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toReadOnly.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toReadOnly.image               = m_hdrImage;
    toReadOnly.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toReadOnly.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT
                                   | VK_ACCESS_SHADER_WRITE_BIT;
    toReadOnly.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT
                                   | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
            | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toReadOnly);
}

void RainPass::onResize(uint32_t width, uint32_t height) {
    m_width  = width;
    m_height = height;
    // HDR view is reconnected externally via setHDROutput after resize.
}

// ---------------------------------------------------------------------------
// Connection setters
// ---------------------------------------------------------------------------

void RainPass::setHDROutput(VkImageView view, VkImage image) {
    if (view != m_hdrView || image != m_hdrImage) {
        m_hdrView         = view;
        m_hdrImage        = image;
        m_descriptorDirty = true;
    }
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool RainPass::createDescriptors() {
    // Single binding: storage image (HDR framebuffer, read-write in GENERAL)
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings    = &binding;
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorLayout)
            != VK_SUCCESS) return false;

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool)
            != VK_SUCCESS) return false;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descriptorLayout;
    if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_descriptorSet)
            != VK_SUCCESS) return false;

    return true;
}

bool RainPass::createPipelineResources() {
    return createComputePipeline(
        "compute_rain.comp.spv",
        m_descriptorLayout,
        static_cast<uint32_t>(sizeof(RainParams)),
        m_pipeline, m_pipelineLayout);
}

void RainPass::updateDescriptors() {
    if (m_descriptorSet == VK_NULL_HANDLE || m_hdrView == VK_NULL_HANDLE) return;

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView   = m_hdrView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = m_descriptorSet;
    write.dstBinding      = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.descriptorCount = 1;
    write.pImageInfo      = &imageInfo;
    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
}

bool RainPass::reloadShader(const std::string& spvPath) {
    return reloadComputeShader(spvPath, m_descriptorLayout, sizeof(RainParams),
                               m_pipeline, m_pipelineLayout);
}

} // namespace ohao
