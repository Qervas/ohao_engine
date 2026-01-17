#include "volumetric_pass.hpp"
#include <array>
#include <iostream>

namespace ohao {

VolumetricPass::~VolumetricPass() {
    cleanup();
}

bool VolumetricPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;

    m_width = 1920;
    m_height = 1080;

    if (!createScatteringOutput()) return false;
    if (!createDescriptors()) return false;
    if (!createScatterPipeline()) return false;

    std::cout << "VolumetricPass initialized: " << m_width << "x" << m_height << std::endl;

    return true;
}

void VolumetricPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    // Destroy pipelines
    if (m_scatterPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_scatterPipeline, nullptr);
        m_scatterPipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }

    // Destroy samplers
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }

    // Destroy descriptors
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_descriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_descriptorLayout, nullptr);
        m_descriptorLayout = VK_NULL_HANDLE;
    }

    destroyResources();
}

void VolumetricPass::execute(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (m_depthView == VK_NULL_HANDLE || m_scatterPipeline == VK_NULL_HANDLE) {
        return;
    }

    // Bind pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_scatterPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    // Push constants
    VolumetricParams params{};
    params.invView = m_invView;
    params.invProjection = m_invProjection;
    params.fogColorDensity = glm::vec4(m_fogColor, m_density);
    params.scatterParams = glm::vec4(m_scattering, m_absorption, m_fogHeight, m_fogFalloff);

    uint32_t froxelWidth = (m_width + FROXEL_TILE_SIZE - 1) / FROXEL_TILE_SIZE;
    uint32_t froxelHeight = (m_height + FROXEL_TILE_SIZE - 1) / FROXEL_TILE_SIZE;
    params.volumeParams = glm::vec4(froxelWidth, froxelHeight, FROXEL_DEPTH_SLICES, m_maxDistance);

    // Extract near/far from projection matrix
    params.nearPlane = m_projection[3][2] / (m_projection[2][2] - 1.0f);
    params.farPlane = m_projection[3][2] / (m_projection[2][2] + 1.0f);
    params.sampleCount = m_sampleCount;
    params.frameIndex = frameIndex;

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(VolumetricParams), &params);

    // Dispatch - one thread per pixel (scaled down for performance)
    uint32_t groupsX = (m_width + 7) / 8;
    uint32_t groupsY = (m_height + 7) / 8;
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    // Barrier for output
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.image = m_scatteringOutput;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void VolumetricPass::onResize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;

    vkDeviceWaitIdle(m_device);

    destroyResources();
    createScatteringOutput();
    updateDescriptorSet();
}

void VolumetricPass::setDepthBuffer(VkImageView depth) {
    m_depthView = depth;
}

void VolumetricPass::setShadowMap(VkImageView shadow, VkSampler shadowSampler) {
    m_shadowView = shadow;
    m_shadowSampler = shadowSampler;
}

void VolumetricPass::setLightBuffer(VkBuffer lightBuffer) {
    m_lightBuffer = lightBuffer;
}

void VolumetricPass::setMatrices(const glm::mat4& view, const glm::mat4& proj,
                                  const glm::mat4& invView, const glm::mat4& invProj) {
    m_view = view;
    m_projection = proj;
    m_invView = invView;
    m_invProjection = invProj;
}

void VolumetricPass::updateDescriptorSet() {
    if (m_descriptorSet == VK_NULL_HANDLE) return;

    std::array<VkWriteDescriptorSet, 3> writes{};

    // Depth buffer
    VkDescriptorImageInfo depthInfo{};
    depthInfo.sampler = m_sampler;
    depthInfo.imageView = m_depthView;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = m_descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &depthInfo;

    // Shadow map (binding 1)
    VkDescriptorImageInfo shadowInfo{};
    shadowInfo.sampler = m_shadowSampler ? m_shadowSampler : m_sampler;
    shadowInfo.imageView = m_shadowView ? m_shadowView : m_depthView;  // Fallback
    shadowInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = m_descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &shadowInfo;

    // Scattering output (binding 4)
    VkDescriptorImageInfo scatterInfo{};
    scatterInfo.sampler = VK_NULL_HANDLE;
    scatterInfo.imageView = m_scatteringView;
    scatterInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = m_descriptorSet;
    writes[2].dstBinding = 4;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].descriptorCount = 1;
    writes[2].pImageInfo = &scatterInfo;

    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

bool VolumetricPass::createScatteringOutput() {
    // Create half-resolution scattering output for performance
    uint32_t outWidth = m_width;
    uint32_t outHeight = m_height;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imageInfo.extent = {outWidth, outHeight, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_device, &imageInfo, nullptr, &m_scatteringOutput) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(m_device, m_scatteringOutput, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_scatteringMemory) != VK_SUCCESS) {
        return false;
    }

    vkBindImageMemory(m_device, m_scatteringOutput, m_scatteringMemory, 0);

    // Create image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_scatteringOutput;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_scatteringView) != VK_SUCCESS) {
        return false;
    }

    // Create sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool VolumetricPass::createDescriptors() {
    // Layout bindings
    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};

    // Depth buffer (binding 0)
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Shadow map (binding 1)
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Light buffer (binding 2)
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Froxel volume (binding 3) - unused for now
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // Scatter output (binding 4)
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorLayout) != VK_SUCCESS) {
        return false;
    }

    // Pool
    std::array<VkDescriptorPoolSize, 3> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 2;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 1;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[2].descriptorCount = 2;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = 1;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        return false;
    }

    // Allocate set
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorLayout;

    if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_descriptorSet) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool VolumetricPass::createScatterPipeline() {
    VkShaderModule compShader = loadShaderModule("compute_volumetric_scatter.comp.spv");
    if (compShader == VK_NULL_HANDLE) {
        std::cerr << "Failed to load volumetric scatter shader" << std::endl;
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
    pushConstant.size = sizeof(VolumetricParams);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descriptorLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, compShader, nullptr);
        return false;
    }

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = shaderStage;
    pipelineInfo.layout = m_pipelineLayout;

    VkResult result = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1,
                                               &pipelineInfo, nullptr, &m_scatterPipeline);

    vkDestroyShaderModule(m_device, compShader, nullptr);

    if (result != VK_SUCCESS) {
        std::cerr << "Failed to create volumetric scatter pipeline" << std::endl;
        return false;
    }

    std::cout << "Volumetric scatter pipeline created successfully" << std::endl;
    return true;
}

bool VolumetricPass::createFroxelVolume() {
    // TODO: Implement 3D froxel volume for more advanced volumetrics
    return true;
}

bool VolumetricPass::createInjectPipeline() {
    // TODO: Implement light injection pass
    return true;
}

bool VolumetricPass::createIntegratePipeline() {
    // TODO: Implement ray integration pass
    return true;
}

void VolumetricPass::destroyResources() {
    if (m_scatteringView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_scatteringView, nullptr);
        m_scatteringView = VK_NULL_HANDLE;
    }
    if (m_scatteringOutput != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_scatteringOutput, nullptr);
        m_scatteringOutput = VK_NULL_HANDLE;
    }
    if (m_scatteringMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_scatteringMemory, nullptr);
        m_scatteringMemory = VK_NULL_HANDLE;
    }
}

} // namespace ohao
