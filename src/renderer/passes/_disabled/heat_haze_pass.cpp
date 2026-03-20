#include "heat_haze_pass.hpp"
#include <iostream>
#include <array>

namespace ohao {

HeatHazePass::~HeatHazePass() {
    cleanup();
}

bool HeatHazePass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device         = device;
    m_physicalDevice = physicalDevice;
    m_width          = 1280;
    m_height         = 720;

    // Create owned output buffer
    m_output = createRenderTarget(
        VK_FORMAT_R16G16B16A16_SFLOAT, m_width, m_height,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (m_output.image == VK_NULL_HANDLE) {
        std::cerr << "HeatHazePass: output buffer creation failed" << std::endl;
        return false;
    }

    // Sampler for reading input
    m_sampler = createSampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    if (m_sampler == VK_NULL_HANDLE) {
        std::cerr << "HeatHazePass: sampler creation failed" << std::endl;
        return false;
    }

    if (!createDescriptors()) {
        std::cerr << "HeatHazePass: descriptor creation failed" << std::endl;
        return false;
    }
    if (!createPipelineResources()) {
        std::cerr << "HeatHazePass: pipeline creation failed" << std::endl;
        return false;
    }
    std::cout << "HeatHazePass: OK" << std::endl;
    return true;
}

void HeatHazePass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);
    safeDestroy(m_pipeline);
    safeDestroy(m_pipelineLayout);
    safeDestroy(m_descriptorPool);
    safeDestroy(m_descriptorLayout);
    safeDestroy(m_sampler);
    m_output.destroy(m_device);
    m_descriptorSet = VK_NULL_HANDLE;
}

void HeatHazePass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (!m_enabled || m_intensity < 0.001f) return;
    if (m_inputView == VK_NULL_HANDLE || m_inputImage == VK_NULL_HANDLE) return;
    if (m_output.image == VK_NULL_HANDLE) return;

    if (m_descriptorDirty) {
        updateDescriptors();
        m_descriptorDirty = false;
    }
    if (m_descriptorSet == VK_NULL_HANDLE) return;

    // Transition input: SHADER_READ_ONLY_OPTIMAL (already there from previous pass)
    // Transition output: UNDEFINED/any → GENERAL for write
    VkImageMemoryBarrier outputToGeneral{};
    outputToGeneral.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    outputToGeneral.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    outputToGeneral.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    outputToGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outputToGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outputToGeneral.image               = m_output.image;
    outputToGeneral.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    outputToGeneral.srcAccessMask       = 0;
    outputToGeneral.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &outputToGeneral);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    HeatHazeParams params{};
    params.time      = m_time;
    params.intensity = m_intensity;
    params.frequency = m_frequency;
    params.speed     = 1.0f;
    params.width     = m_width;
    params.height    = m_height;
    params.padding0  = 0.0f;
    params.padding1  = 0.0f;

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(HeatHazeParams), &params);

    uint32_t groupX = (m_width  + 7u) / 8u;
    uint32_t groupY = (m_height + 7u) / 8u;
    vkCmdDispatch(cmd, groupX, groupY, 1);

    // Transition output: GENERAL → SHADER_READ_ONLY_OPTIMAL for tonemapping
    VkImageMemoryBarrier outputToRead{};
    outputToRead.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    outputToRead.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    outputToRead.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    outputToRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outputToRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outputToRead.image               = m_output.image;
    outputToRead.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    outputToRead.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    outputToRead.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &outputToRead);
}

void HeatHazePass::onResize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;
    m_width  = width;
    m_height = height;

    vkDeviceWaitIdle(m_device);
    m_output.destroy(m_device);
    m_output = createRenderTarget(
        VK_FORMAT_R16G16B16A16_SFLOAT, width, height,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    m_descriptorDirty = true;
}

void HeatHazePass::setInputImage(VkImageView view, VkImage image) {
    if (view != m_inputView || image != m_inputImage) {
        m_inputView  = view;
        m_inputImage = image;
        m_descriptorDirty = true;
    }
}

bool HeatHazePass::createDescriptors() {
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorLayout) != VK_SUCCESS)
        return false;

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          1};

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

bool HeatHazePass::createPipelineResources() {
    return createComputePipeline(
        "compute_heat_haze.comp.spv",
        m_descriptorLayout,
        static_cast<uint32_t>(sizeof(HeatHazeParams)),
        m_pipeline, m_pipelineLayout);
}

void HeatHazePass::updateDescriptors() {
    if (m_descriptorSet == VK_NULL_HANDLE) return;
    if (m_inputView == VK_NULL_HANDLE || m_output.view == VK_NULL_HANDLE) return;

    std::array<VkWriteDescriptorSet, 2> writes{};

    VkDescriptorImageInfo samplerInfo{};
    samplerInfo.sampler     = m_sampler;
    samplerInfo.imageView   = m_inputView;
    samplerInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = m_descriptorSet;
    writes[0].dstBinding      = 0;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo      = &samplerInfo;

    VkDescriptorImageInfo storageInfo{};
    storageInfo.imageView   = m_output.view;
    storageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = m_descriptorSet;
    writes[1].dstBinding      = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo      = &storageInfo;

    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

bool HeatHazePass::reloadShader(const std::string& spvPath) {
    return reloadComputeShader(spvPath, m_descriptorLayout, sizeof(HeatHazeParams),
                               m_pipeline, m_pipelineLayout);
}

} // namespace ohao
