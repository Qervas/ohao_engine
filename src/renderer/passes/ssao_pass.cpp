#include "ssao_pass.hpp"
#include <array>
#include <random>
#include <cmath>

namespace ohao {

SSAOPass::~SSAOPass() {
    cleanup();
}

bool SSAOPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;

    m_width = 1920;
    m_height = 1080;

    if (!createOutputImage()) return false;
    if (!createNoiseTexture()) return false;
    if (!createDescriptors()) return false;
    if (!createPipeline()) return false;

    return true;
}

void SSAOPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    if (m_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_pipeline, nullptr);
    if (m_pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    if (m_descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    if (m_descriptorLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, m_descriptorLayout, nullptr);
    if (m_sampler != VK_NULL_HANDLE) vkDestroySampler(m_device, m_sampler, nullptr);
    if (m_noiseSampler != VK_NULL_HANDLE) vkDestroySampler(m_device, m_noiseSampler, nullptr);

    // Noise texture
    if (m_noiseView != VK_NULL_HANDLE) vkDestroyImageView(m_device, m_noiseView, nullptr);
    if (m_noiseImage != VK_NULL_HANDLE) vkDestroyImage(m_device, m_noiseImage, nullptr);
    if (m_noiseMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_noiseMemory, nullptr);

    // Staging buffer (may still exist if never executed)
    if (m_noiseStagingBuffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, m_noiseStagingBuffer, nullptr);
    if (m_noiseStagingMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_noiseStagingMemory, nullptr);

    destroyOutputImage();

    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_descriptorPool = VK_NULL_HANDLE;
    m_descriptorLayout = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
    m_noiseSampler = VK_NULL_HANDLE;
    m_noiseView = VK_NULL_HANDLE;
    m_noiseImage = VK_NULL_HANDLE;
    m_noiseMemory = VK_NULL_HANDLE;
    m_noiseStagingBuffer = VK_NULL_HANDLE;
    m_noiseStagingMemory = VK_NULL_HANDLE;
    m_noiseUploaded = false;
}

void SSAOPass::uploadNoiseTexture(VkCommandBuffer cmd) {
    if (m_noiseStagingBuffer == VK_NULL_HANDLE || m_noiseImage == VK_NULL_HANDLE) {
        m_noiseUploaded = true;
        return;
    }

    // Transition noise image from UNDEFINED to TRANSFER_DST
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_noiseImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Copy staging buffer to image
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {4, 4, 1};

    vkCmdCopyBufferToImage(cmd, m_noiseStagingBuffer, m_noiseImage,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition noise image to SHADER_READ_ONLY
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    m_noiseUploaded = true;
    // Staging buffer destroyed in cleanup() (can't destroy while command buffer is recording)
}

void SSAOPass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (m_depthView == VK_NULL_HANDLE || m_normalView == VK_NULL_HANDLE) return;
    if (m_pipeline == VK_NULL_HANDLE || m_pipelineLayout == VK_NULL_HANDLE ||
        m_descriptorSet == VK_NULL_HANDLE) return;

    // Upload noise texture on first use
    if (!m_noiseUploaded) {
        uploadNoiseTexture(cmd);
    }

    // Transition AO output to general for compute write
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_aoOutput;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Bind compute pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                            0, 1, &m_descriptorSet, 0, nullptr);

    // Push constants
    SSAOParams params{};
    params.projection = m_projection;
    params.invProjection = m_invProjection;
    params.noiseScale = glm::vec4(
        static_cast<float>(m_width) / 4.0f,  // Noise texture is 4x4
        static_cast<float>(m_height) / 4.0f,
        static_cast<float>(m_width),
        static_cast<float>(m_height)
    );
    params.radius = m_radius;
    params.bias = m_bias;
    params.intensity = m_intensity;
    params.sampleCount = m_sampleCount;
    params.texelSize = glm::vec2(1.0f / m_width, 1.0f / m_height);
    params.falloffStart = m_falloffStart;
    params.falloffEnd = m_falloffEnd;

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);

    // Dispatch compute shader (8x8 workgroups)
    uint32_t groupsX = (m_width + 7) / 8;
    uint32_t groupsY = (m_height + 7) / 8;
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    // Transition AO output to shader read
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void SSAOPass::onResize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;

    vkDeviceWaitIdle(m_device);

    destroyOutputImage();
    createOutputImage();
    createDescriptors();
}

void SSAOPass::setProjectionMatrix(const glm::mat4& proj, const glm::mat4& invProj) {
    m_projection = proj;
    m_invProjection = invProj;
}

void SSAOPass::setDepthBuffer(VkImageView depth) {
    m_depthView = depth;
    updateDescriptorSet();
}

void SSAOPass::setNormalBuffer(VkImageView normal) {
    m_normalView = normal;
    updateDescriptorSet();
}

void SSAOPass::updateDescriptorSet() {
    if (m_descriptorSet == VK_NULL_HANDLE || m_sampler == VK_NULL_HANDLE) return;

    // Use noise view as fallback for unset sampler bindings (valid RGBA32F image)
    VkImageView fallbackView = m_noiseView != VK_NULL_HANDLE ? m_noiseView : m_aoOutputView;
    VkSampler fallbackSampler = m_noiseSampler != VK_NULL_HANDLE ? m_noiseSampler : m_sampler;
    if (fallbackView == VK_NULL_HANDLE) return; // Nothing to bind

    std::array<VkDescriptorImageInfo, 4> imageInfos{};
    std::array<VkWriteDescriptorSet, 4> writes{};

    // Binding 0: Depth buffer (use fallback if not yet set)
    imageInfos[0].sampler = m_sampler;
    imageInfos[0].imageView = m_depthView != VK_NULL_HANDLE ? m_depthView : fallbackView;
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = m_descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &imageInfos[0];

    // Binding 1: Normal buffer (use fallback if not yet set)
    imageInfos[1].sampler = m_sampler;
    imageInfos[1].imageView = m_normalView != VK_NULL_HANDLE ? m_normalView : fallbackView;
    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = m_descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &imageInfos[1];

    // Binding 2: AO output (storage image) — always available after init
    imageInfos[2].sampler = VK_NULL_HANDLE;
    imageInfos[2].imageView = m_aoOutputView;
    imageInfos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = m_descriptorSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].descriptorCount = 1;
    writes[2].pImageInfo = &imageInfos[2];

    // Binding 3: Noise texture — always available after init
    imageInfos[3].sampler = fallbackSampler;
    imageInfos[3].imageView = m_noiseView != VK_NULL_HANDLE ? m_noiseView : fallbackView;
    imageInfos[3].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = m_descriptorSet;
    writes[3].dstBinding = 3;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].descriptorCount = 1;
    writes[3].pImageInfo = &imageInfos[3];

    vkUpdateDescriptorSets(m_device, 4, writes.data(), 0, nullptr);
}

bool SSAOPass::createOutputImage() {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R16_SFLOAT; // Single channel AO
    imageInfo.extent = {m_width, m_height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_device, &imageInfo, nullptr, &m_aoOutput) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(m_device, m_aoOutput, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_aoMemory) != VK_SUCCESS) {
        return false;
    }

    vkBindImageMemory(m_device, m_aoOutput, m_aoMemory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_aoOutput;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_aoOutputView) != VK_SUCCESS) {
        return false;
    }

    // Create sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    if (m_sampler == VK_NULL_HANDLE) {
        if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
            return false;
        }
    }

    return true;
}

void SSAOPass::destroyOutputImage() {
    if (m_aoOutputView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_aoOutputView, nullptr);
        m_aoOutputView = VK_NULL_HANDLE;
    }
    if (m_aoOutput != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_aoOutput, nullptr);
        m_aoOutput = VK_NULL_HANDLE;
    }
    if (m_aoMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_aoMemory, nullptr);
        m_aoMemory = VK_NULL_HANDLE;
    }
}

bool SSAOPass::createNoiseTexture() {
    // Generate 4x4 noise texture with random unit vectors
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    std::array<glm::vec4, 16> noiseData;
    for (auto& v : noiseData) {
        v = glm::vec4(
            dist(gen),
            dist(gen),
            0.0f,  // z = 0 for tangent space rotation
            0.0f
        );
        float len = std::sqrt(v.x * v.x + v.y * v.y);
        if (len > 0.0001f) {
            v.x /= len;
            v.y /= len;
        }
    }

    // Create staging buffer
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    VkDeviceSize bufferSize = sizeof(noiseData);

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    vkCreateBuffer(m_device, &bufferInfo, nullptr, &stagingBuffer);

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(m_device, stagingBuffer, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    vkAllocateMemory(m_device, &allocInfo, nullptr, &stagingMemory);
    vkBindBufferMemory(m_device, stagingBuffer, stagingMemory, 0);

    void* data;
    vkMapMemory(m_device, stagingMemory, 0, bufferSize, 0, &data);
    memcpy(data, noiseData.data(), bufferSize);
    vkUnmapMemory(m_device, stagingMemory);

    // Create noise image
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    imageInfo.extent = {4, 4, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    vkCreateImage(m_device, &imageInfo, nullptr, &m_noiseImage);

    vkGetImageMemoryRequirements(m_device, m_noiseImage, &memReq);
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReq.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    vkAllocateMemory(m_device, &allocInfo, nullptr, &m_noiseMemory);
    vkBindImageMemory(m_device, m_noiseImage, m_noiseMemory, 0);

    // Image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_noiseImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    vkCreateImageView(m_device, &viewInfo, nullptr, &m_noiseView);

    // Repeating sampler for noise
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;

    vkCreateSampler(m_device, &samplerInfo, nullptr, &m_noiseSampler);

    // Keep staging buffer alive for deferred upload in first execute() call
    m_noiseStagingBuffer = stagingBuffer;
    m_noiseStagingMemory = stagingMemory;
    m_noiseUploaded = false;

    return true;
}

bool SSAOPass::createDescriptors() {
    // Bindings:
    // 0: Depth buffer (sampled)
    // 1: Normal buffer (sampled)
    // 2: AO output (storage image)
    // 3: Noise texture (sampled)
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (m_descriptorLayout == VK_NULL_HANDLE) {
        if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorLayout) != VK_SUCCESS) {
            return false;
        }
    }

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 3;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = 1;

    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    }

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorLayout;

    return vkAllocateDescriptorSets(m_device, &allocInfo, &m_descriptorSet) == VK_SUCCESS;
}

bool SSAOPass::createPipeline() {
    VkShaderModule compShader = loadShaderModule("compute_ssao.comp.spv");

    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = compShader;
    stageInfo.pName = "main";

    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(SSAOParams);

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
    pipelineInfo.stage = stageInfo;
    pipelineInfo.layout = m_pipelineLayout;

    VkResult result = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                               nullptr, &m_pipeline);

    vkDestroyShaderModule(m_device, compShader, nullptr);

    return result == VK_SUCCESS;
}

} // namespace ohao
