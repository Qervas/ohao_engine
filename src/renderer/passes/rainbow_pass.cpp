#include "rainbow_pass.hpp"
#include <iostream>
#include <array>

namespace ohao {

RainbowPass::~RainbowPass() {
    cleanup();
}

bool RainbowPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device         = device;
    m_physicalDevice = physicalDevice;

    if (!createDescriptors()) {
        std::cerr << "RainbowPass: descriptor creation failed" << std::endl;
        return false;
    }
    if (!createPipelineResources()) {
        std::cerr << "RainbowPass: pipeline creation failed" << std::endl;
        return false;
    }
    std::cout << "RainbowPass: OK" << std::endl;
    return true;
}

void RainbowPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);
    safeDestroy(m_pipeline);
    safeDestroy(m_pipelineLayout);
    safeDestroy(m_descriptorPool);
    safeDestroy(m_descriptorLayout);
    if (m_ownsSampler) safeDestroy(m_depthSampler);
    m_descriptorSet = VK_NULL_HANDLE;
}

void RainbowPass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (!m_enabled || m_intensity < 0.001f) return;
    if (m_hdrImage == VK_NULL_HANDLE || m_depthView == VK_NULL_HANDLE) return;

    if (m_descriptorDirty) {
        updateDescriptors();
        m_descriptorDirty = false;
    }
    if (m_descriptorSet == VK_NULL_HANDLE) return;

    // Transition HDR: SHADER_READ_ONLY_OPTIMAL → GENERAL
    VkImageMemoryBarrier toGeneral{};
    toGeneral.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGeneral.oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toGeneral.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.image               = m_hdrImage;
    toGeneral.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toGeneral.srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    toGeneral.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toGeneral);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    RainbowParams params{};
    params.antiSolarPos = m_antiSolarPos;
    params.arcRadius    = 0.42f;
    params.arcWidth     = 0.04f;
    params.intensity    = m_intensity;
    params.padding0     = 0.0f;
    params.padding1     = 0.0f;
    params.padding2     = 0.0f;

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(RainbowParams), &params);

    uint32_t groupX = (m_width  + 7u) / 8u;
    uint32_t groupY = (m_height + 7u) / 8u;
    vkCmdDispatch(cmd, groupX, groupY, 1);

    // Transition HDR: GENERAL → SHADER_READ_ONLY_OPTIMAL
    VkImageMemoryBarrier toReadOnly{};
    toReadOnly.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toReadOnly.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    toReadOnly.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toReadOnly.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toReadOnly.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toReadOnly.image               = m_hdrImage;
    toReadOnly.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toReadOnly.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    toReadOnly.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toReadOnly);
}

void RainbowPass::onResize(uint32_t width, uint32_t height) {
    m_width  = width;
    m_height = height;
}

void RainbowPass::setHDROutput(VkImageView view, VkImage image) {
    if (view != m_hdrView || image != m_hdrImage) {
        m_hdrView  = view;
        m_hdrImage = image;
        m_descriptorDirty = true;
    }
}

void RainbowPass::setDepthView(VkImageView view, VkSampler sampler) {
    if (view != m_depthView) {
        m_depthView = view;
        if (sampler != VK_NULL_HANDLE) {
            if (m_ownsSampler && m_depthSampler != VK_NULL_HANDLE) {
                safeDestroy(m_depthSampler);
                m_ownsSampler = false;
            }
            m_depthSampler = sampler;
        } else if (m_depthSampler == VK_NULL_HANDLE) {
            m_depthSampler = createSampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
            m_ownsSampler  = true;
        }
        m_descriptorDirty = true;
    }
}

bool RainbowPass::createDescriptors() {
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorLayout) != VK_SUCCESS)
        return false;

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes    = poolSizes.data();
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        return false;

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descriptorLayout;
    return vkAllocateDescriptorSets(m_device, &allocInfo, &m_descriptorSet) == VK_SUCCESS;
}

bool RainbowPass::createPipelineResources() {
    return createComputePipeline(
        "compute_rainbow.comp.spv",
        m_descriptorLayout,
        static_cast<uint32_t>(sizeof(RainbowParams)),
        m_pipeline, m_pipelineLayout);
}

void RainbowPass::updateDescriptors() {
    if (m_descriptorSet == VK_NULL_HANDLE || m_hdrView == VK_NULL_HANDLE) return;

    std::array<VkWriteDescriptorSet, 2> writes{};

    VkDescriptorImageInfo storageInfo{};
    storageInfo.imageView   = m_hdrView;
    storageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = m_descriptorSet;
    writes[0].dstBinding      = 0;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo      = &storageInfo;

    VkDescriptorImageInfo samplerInfo{};
    samplerInfo.imageView   = m_depthView != VK_NULL_HANDLE ? m_depthView : m_hdrView;
    samplerInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    samplerInfo.sampler     = m_depthSampler;
    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = m_descriptorSet;
    writes[1].dstBinding      = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo      = &samplerInfo;

    uint32_t count = (m_depthView != VK_NULL_HANDLE && m_depthSampler != VK_NULL_HANDLE) ? 2u : 1u;
    vkUpdateDescriptorSets(m_device, count, writes.data(), 0, nullptr);
}

} // namespace ohao
