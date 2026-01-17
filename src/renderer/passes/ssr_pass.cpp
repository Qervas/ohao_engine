#include "ssr_pass.hpp"
#include <stdexcept>
#include <array>
#include <cmath>
#include <iostream>

namespace ohao {

SSRPass::~SSRPass() {
    cleanup();
}

bool SSRPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;

    m_width = 1920;
    m_height = 1080;

    if (!createOutputImage()) return false;
    if (!createHiZBuffer()) return false;
    if (!createDescriptors()) return false;
    if (!createHiZPipeline()) return false;
    if (!createPipeline()) return false;

    std::cout << "SSRPass initialized: " << m_width << "x" << m_height
              << " with " << m_hizMipLevels << " Hi-Z mip levels" << std::endl;

    return true;
}

void SSRPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    // Destroy pipelines
    if (m_ssrPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_ssrPipeline, nullptr);
        m_ssrPipeline = VK_NULL_HANDLE;
    }
    if (m_ssrPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_ssrPipelineLayout, nullptr);
        m_ssrPipelineLayout = VK_NULL_HANDLE;
    }
    if (m_hizPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_hizPipeline, nullptr);
        m_hizPipeline = VK_NULL_HANDLE;
    }
    if (m_hizPipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_hizPipelineLayout, nullptr);
        m_hizPipelineLayout = VK_NULL_HANDLE;
    }

    // Destroy samplers
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    if (m_pointSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, m_pointSampler, nullptr);
        m_pointSampler = VK_NULL_HANDLE;
    }

    // Destroy descriptor resources
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_ssrDescriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_ssrDescriptorLayout, nullptr);
        m_ssrDescriptorLayout = VK_NULL_HANDLE;
    }
    if (m_hizDescriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_hizDescriptorLayout, nullptr);
        m_hizDescriptorLayout = VK_NULL_HANDLE;
    }

    destroyHiZBuffer();
    destroyOutputImage();
}

void SSRPass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (m_depthView == VK_NULL_HANDLE ||
        m_normalView == VK_NULL_HANDLE ||
        m_colorView == VK_NULL_HANDLE) {
        return;
    }

    // First: Generate Hi-Z buffer
    generateHiZ(cmd);

    // Memory barrier after Hi-Z generation
    VkMemoryBarrier memBarrier{};
    memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &memBarrier, 0, nullptr, 0, nullptr);

    // Second: SSR ray marching
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ssrPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_ssrPipelineLayout, 0, 1, &m_ssrDescriptorSet, 0, nullptr);

    // Push constants
    SSRParams params{};
    params.view = m_view;
    params.projection = m_projection;
    params.invView = m_invView;
    params.invProjection = m_invProjection;
    params.screenSize = glm::vec4(m_width, m_height, 1.0f / m_width, 1.0f / m_height);
    params.maxDistance = m_maxDistance;
    params.thickness = m_thickness;
    params.roughnessFade = m_roughnessFade;
    params.edgeFade = m_edgeFade;
    params.maxSteps = m_maxSteps;
    params.binarySearchSteps = m_binarySearchSteps;
    params.hizMipLevels = m_hizMipLevels;
    params.padding = 0;

    vkCmdPushConstants(cmd, m_ssrPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(SSRParams), &params);

    // Dispatch
    uint32_t groupsX = (m_width + 7) / 8;
    uint32_t groupsY = (m_height + 7) / 8;
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    // Barrier for output to be readable
    VkImageMemoryBarrier outputBarrier{};
    outputBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    outputBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    outputBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    outputBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    outputBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    outputBarrier.image = m_reflectionImage;
    outputBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    outputBarrier.subresourceRange.baseMipLevel = 0;
    outputBarrier.subresourceRange.levelCount = 1;
    outputBarrier.subresourceRange.baseArrayLayer = 0;
    outputBarrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &outputBarrier);
}

void SSRPass::generateHiZ(VkCommandBuffer cmd) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_hizPipeline);

    uint32_t srcWidth = m_width;
    uint32_t srcHeight = m_height;

    for (uint32_t mip = 0; mip < m_hizMipLevels; ++mip) {
        uint32_t dstWidth = std::max(1u, srcWidth / 2);
        uint32_t dstHeight = std::max(1u, srcHeight / 2);

        // Bind descriptor set for this mip level
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_hizPipelineLayout, 0, 1, &m_hizDescriptorSets[mip], 0, nullptr);

        // Push constants
        HiZParams params{};
        params.srcSize = glm::uvec2(srcWidth, srcHeight);
        params.dstSize = glm::uvec2(dstWidth, dstHeight);
        params.srcMip = mip;

        vkCmdPushConstants(cmd, m_hizPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(HiZParams), &params);

        // Dispatch
        uint32_t groupsX = (dstWidth + 7) / 8;
        uint32_t groupsY = (dstHeight + 7) / 8;
        vkCmdDispatch(cmd, groupsX, groupsY, 1);

        // Barrier between mip levels
        if (mip < m_hizMipLevels - 1) {
            VkMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 1, &barrier, 0, nullptr, 0, nullptr);
        }

        srcWidth = dstWidth;
        srcHeight = dstHeight;
    }
}

void SSRPass::onResize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;

    vkDeviceWaitIdle(m_device);

    destroyHiZBuffer();
    destroyOutputImage();
    createOutputImage();
    createHiZBuffer();
    updateDescriptorSet();
}

void SSRPass::setDepthBuffer(VkImageView depth) {
    m_depthView = depth;
}

void SSRPass::setNormalBuffer(VkImageView normal) {
    m_normalView = normal;
}

void SSRPass::setColorBuffer(VkImageView color) {
    m_colorView = color;
}

void SSRPass::setRoughnessBuffer(VkImageView roughness) {
    m_roughnessView = roughness;
}

void SSRPass::setMatrices(const glm::mat4& view, const glm::mat4& proj,
                          const glm::mat4& invView, const glm::mat4& invProj) {
    m_view = view;
    m_projection = proj;
    m_invView = invView;
    m_invProjection = invProj;
}

void SSRPass::updateDescriptorSet() {
    if (m_ssrDescriptorSet == VK_NULL_HANDLE) return;

    std::array<VkDescriptorImageInfo, 5> imageInfos{};

    // Depth buffer
    imageInfos[0].sampler = m_pointSampler;
    imageInfos[0].imageView = m_depthView;
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Normal buffer
    imageInfos[1].sampler = m_pointSampler;
    imageInfos[1].imageView = m_normalView;
    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Color buffer
    imageInfos[2].sampler = m_sampler;
    imageInfos[2].imageView = m_colorView;
    imageInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Hi-Z buffer
    imageInfos[3].sampler = m_pointSampler;
    imageInfos[3].imageView = m_hizView;
    imageInfos[3].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    // Reflection output
    imageInfos[4].sampler = VK_NULL_HANDLE;
    imageInfos[4].imageView = m_reflectionView;
    imageInfos[4].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 5> writes{};
    for (uint32_t i = 0; i < 4; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_ssrDescriptorSet;
        writes[i].dstBinding = i;
        writes[i].dstArrayElement = 0;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo = &imageInfos[i];
    }

    // Output is storage image
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = m_ssrDescriptorSet;
    writes[4].dstBinding = 4;
    writes[4].dstArrayElement = 0;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[4].descriptorCount = 1;
    writes[4].pImageInfo = &imageInfos[4];

    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

bool SSRPass::createOutputImage() {
    // Create reflection output image (RGBA16F for HDR reflections + confidence)
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imageInfo.extent = {m_width, m_height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_device, &imageInfo, nullptr, &m_reflectionImage) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(m_device, m_reflectionImage, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_reflectionMemory) != VK_SUCCESS) {
        return false;
    }

    vkBindImageMemory(m_device, m_reflectionImage, m_reflectionMemory, 0);

    // Create image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_reflectionImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_reflectionView) != VK_SUCCESS) {
        return false;
    }

    // Create samplers
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 16.0f;

    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
        return false;
    }

    // Point sampler for depth/normals
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_pointSampler) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool SSRPass::createHiZBuffer() {
    // Calculate mip levels
    m_hizMipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(m_width, m_height)))) + 1;

    // Create Hi-Z image with mip chain
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R32_SFLOAT;
    imageInfo.extent = {m_width, m_height, 1};
    imageInfo.mipLevels = m_hizMipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_device, &imageInfo, nullptr, &m_hizImage) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(m_device, m_hizImage, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_hizMemory) != VK_SUCCESS) {
        return false;
    }

    vkBindImageMemory(m_device, m_hizImage, m_hizMemory, 0);

    // Create full image view (all mips)
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_hizImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R32_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = m_hizMipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_hizView) != VK_SUCCESS) {
        return false;
    }

    // Create per-mip views for Hi-Z generation
    m_hizMipViews.resize(m_hizMipLevels);
    for (uint32_t mip = 0; mip < m_hizMipLevels; ++mip) {
        viewInfo.subresourceRange.baseMipLevel = mip;
        viewInfo.subresourceRange.levelCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_hizMipViews[mip]) != VK_SUCCESS) {
            return false;
        }
    }

    return true;
}

bool SSRPass::createDescriptors() {
    // SSR descriptor layout
    std::array<VkDescriptorSetLayoutBinding, 5> ssrBindings{};

    // Depth buffer (sampler)
    ssrBindings[0].binding = 0;
    ssrBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ssrBindings[0].descriptorCount = 1;
    ssrBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Normal buffer (sampler)
    ssrBindings[1].binding = 1;
    ssrBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ssrBindings[1].descriptorCount = 1;
    ssrBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Color buffer (sampler)
    ssrBindings[2].binding = 2;
    ssrBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ssrBindings[2].descriptorCount = 1;
    ssrBindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Hi-Z buffer (sampler)
    ssrBindings[3].binding = 3;
    ssrBindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ssrBindings[3].descriptorCount = 1;
    ssrBindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Reflection output (storage image)
    ssrBindings[4].binding = 4;
    ssrBindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    ssrBindings[4].descriptorCount = 1;
    ssrBindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(ssrBindings.size());
    layoutInfo.pBindings = ssrBindings.data();

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_ssrDescriptorLayout) != VK_SUCCESS) {
        return false;
    }

    // Hi-Z descriptor layout
    std::array<VkDescriptorSetLayoutBinding, 2> hizBindings{};

    // Source depth/mip (sampler)
    hizBindings[0].binding = 0;
    hizBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    hizBindings[0].descriptorCount = 1;
    hizBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Destination mip (storage image)
    hizBindings[1].binding = 1;
    hizBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    hizBindings[1].descriptorCount = 1;
    hizBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    layoutInfo.bindingCount = static_cast<uint32_t>(hizBindings.size());
    layoutInfo.pBindings = hizBindings.data();

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_hizDescriptorLayout) != VK_SUCCESS) {
        return false;
    }

    // Descriptor pool
    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 5 + m_hizMipLevels;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 1 + m_hizMipLevels;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = 1 + m_hizMipLevels;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        return false;
    }

    // Allocate SSR descriptor set
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_ssrDescriptorLayout;

    if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_ssrDescriptorSet) != VK_SUCCESS) {
        return false;
    }

    // Allocate Hi-Z descriptor sets (one per mip level)
    m_hizDescriptorSets.resize(m_hizMipLevels);
    for (uint32_t mip = 0; mip < m_hizMipLevels; ++mip) {
        allocInfo.pSetLayouts = &m_hizDescriptorLayout;
        if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_hizDescriptorSets[mip]) != VK_SUCCESS) {
            return false;
        }
    }

    return true;
}

bool SSRPass::createPipeline() {
    VkShaderModule compShader = loadShaderModule("compute_ssr.comp.spv");
    if (compShader == VK_NULL_HANDLE) {
        std::cerr << "Failed to load SSR compute shader" << std::endl;
        return false;
    }

    VkPipelineShaderStageCreateInfo shaderStage{};
    shaderStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStage.module = compShader;
    shaderStage.pName = "main";

    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(SSRParams);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_ssrDescriptorLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_ssrPipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, compShader, nullptr);
        return false;
    }

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = shaderStage;
    pipelineInfo.layout = m_ssrPipelineLayout;

    VkResult result = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1,
                                               &pipelineInfo, nullptr, &m_ssrPipeline);

    vkDestroyShaderModule(m_device, compShader, nullptr);

    if (result != VK_SUCCESS) {
        std::cerr << "Failed to create SSR pipeline" << std::endl;
        return false;
    }

    std::cout << "SSR pipeline created successfully" << std::endl;
    return true;
}

bool SSRPass::createHiZPipeline() {
    VkShaderModule compShader = loadShaderModule("compute_hiz_generate.comp.spv");
    if (compShader == VK_NULL_HANDLE) {
        std::cerr << "Failed to load Hi-Z compute shader" << std::endl;
        return false;
    }

    VkPipelineShaderStageCreateInfo shaderStage{};
    shaderStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStage.module = compShader;
    shaderStage.pName = "main";

    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(HiZParams);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_hizDescriptorLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_hizPipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, compShader, nullptr);
        return false;
    }

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = shaderStage;
    pipelineInfo.layout = m_hizPipelineLayout;

    VkResult result = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1,
                                               &pipelineInfo, nullptr, &m_hizPipeline);

    vkDestroyShaderModule(m_device, compShader, nullptr);

    if (result != VK_SUCCESS) {
        std::cerr << "Failed to create Hi-Z pipeline" << std::endl;
        return false;
    }

    std::cout << "Hi-Z pipeline created successfully" << std::endl;
    return true;
}

void SSRPass::destroyOutputImage() {
    if (m_reflectionView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_reflectionView, nullptr);
        m_reflectionView = VK_NULL_HANDLE;
    }
    if (m_reflectionImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_reflectionImage, nullptr);
        m_reflectionImage = VK_NULL_HANDLE;
    }
    if (m_reflectionMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_reflectionMemory, nullptr);
        m_reflectionMemory = VK_NULL_HANDLE;
    }
}

void SSRPass::destroyHiZBuffer() {
    for (auto& view : m_hizMipViews) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, view, nullptr);
        }
    }
    m_hizMipViews.clear();

    if (m_hizView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_hizView, nullptr);
        m_hizView = VK_NULL_HANDLE;
    }
    if (m_hizImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_hizImage, nullptr);
        m_hizImage = VK_NULL_HANDLE;
    }
    if (m_hizMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_hizMemory, nullptr);
        m_hizMemory = VK_NULL_HANDLE;
    }
}

} // namespace ohao
