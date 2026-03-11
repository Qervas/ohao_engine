#include "underwater_pass.hpp"
#include <array>
#include <iostream>

namespace ohao {

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

UnderwaterPass::~UnderwaterPass() {
    cleanup();
}

bool UnderwaterPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device         = device;
    m_physicalDevice = physicalDevice;

    // Linear clamp sampler for HDR read
    {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_device, &si, nullptr, &m_linearSampler) != VK_SUCCESS) {
            std::cerr << "UnderwaterPass: sampler creation failed" << std::endl;
            return false;
        }
    }

    if (!createDescriptors()) {
        std::cerr << "UnderwaterPass: descriptor creation failed" << std::endl;
        return false;
    }
    if (!createPipeline()) {
        std::cerr << "UnderwaterPass: pipeline creation failed" << std::endl;
        return false;
    }

    std::cout << "UnderwaterPass: OK" << std::endl;
    return true;
}

void UnderwaterPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);

    safeDestroy(m_pipeline);
    safeDestroy(m_pipelineLayout);
    safeDestroy(m_descPool);
    safeDestroy(m_descLayout);
    m_descSet = VK_NULL_HANDLE;
    safeDestroy(m_linearSampler);
}

// ---------------------------------------------------------------------------
// Execute
// ---------------------------------------------------------------------------

void UnderwaterPass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    // Only dispatch when camera is actually underwater
    if (!m_enabled || m_waterDepth <= 0.001f) return;
    if (m_hdrImage == VK_NULL_HANDLE) return;
    if (m_pipeline == VK_NULL_HANDLE) return;

    if (m_descDirty) {
        updateDescriptors();
        m_descDirty = false;
    }
    if (m_descSet == VK_NULL_HANDLE) return;

    // HDR is in SHADER_READ_ONLY_OPTIMAL after water pass.
    // We need GENERAL for the storage write view.
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

    // Dispatch
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &m_descSet, 0, nullptr);

    UnderwaterPC pc{};
    pc.screenSize      = glm::vec2(static_cast<float>(m_width), static_cast<float>(m_height));
    pc.time            = m_time;
    pc.waterDepth      = m_waterDepth;
    pc.fogColor        = m_fogColor;
    pc.fogDensity      = m_fogDensity;
    pc.chromStrength   = m_chromStrength;
    pc.distortFreq     = m_distortFreq;
    pc.distortSpeed    = m_distortSpeed;
    pc.distortStrength = m_distortStrength;

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(UnderwaterPC), &pc);

    uint32_t groupX = (m_width  + 7u) / 8u;
    uint32_t groupY = (m_height + 7u) / 8u;
    vkCmdDispatch(cmd, groupX, groupY, 1);

    // Restore SHADER_READ_ONLY for post-processing
    VkImageMemoryBarrier toReadOnly{};
    toReadOnly.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toReadOnly.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    toReadOnly.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toReadOnly.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toReadOnly.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toReadOnly.image               = m_hdrImage;
    toReadOnly.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toReadOnly.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    toReadOnly.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
            | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toReadOnly);
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------

void UnderwaterPass::onResize(uint32_t width, uint32_t height) {
    m_width  = width;
    m_height = height;
}

// ---------------------------------------------------------------------------
// Resource connections
// ---------------------------------------------------------------------------

void UnderwaterPass::setHDRTarget(VkImageView readView, VkImage image, VkImageView writeView) {
    m_hdrReadView  = readView;
    m_hdrImage     = image;
    m_hdrWriteView = writeView;
    m_descDirty    = true;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool UnderwaterPass::createDescriptors() {
    // 2 bindings:
    //   0: hdrInput  (COMBINED_IMAGE_SAMPLER — read)
    //   1: hdrOutput (STORAGE_IMAGE — write)
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
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

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes    = poolSizes.data();
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descPool) != VK_SUCCESS)
        return false;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descLayout;
    return vkAllocateDescriptorSets(m_device, &allocInfo, &m_descSet) == VK_SUCCESS;
}

bool UnderwaterPass::createPipeline() {
    return createComputePipeline("water_water_underwater.comp.spv",
                                 m_descLayout,
                                 sizeof(UnderwaterPC),
                                 m_pipeline, m_pipelineLayout);
}

void UnderwaterPass::updateDescriptors() {
    if (!m_descSet) return;
    if (!m_hdrReadView || !m_hdrWriteView) return;
    if (!m_linearSampler) return;

    VkDescriptorImageInfo readInfo{};
    readInfo.sampler     = m_linearSampler;
    readInfo.imageView   = m_hdrReadView;
    readInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;  // GENERAL covers both read and write

    VkDescriptorImageInfo writeInfo{};
    writeInfo.sampler     = VK_NULL_HANDLE;
    writeInfo.imageView   = m_hdrWriteView;
    writeInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = m_descSet;
    writes[0].dstBinding      = 0;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo      = &readInfo;

    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = m_descSet;
    writes[1].dstBinding      = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo      = &writeInfo;

    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

bool UnderwaterPass::reloadShader(const std::string& spvPath) {
    return reloadComputeShader(spvPath, m_descLayout, sizeof(UnderwaterPC),
                               m_pipeline, m_pipelineLayout);
}

} // namespace ohao
