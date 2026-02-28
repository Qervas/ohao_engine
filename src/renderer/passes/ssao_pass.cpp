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

    safeDestroy(m_pipeline);
    safeDestroy(m_pipelineLayout);
    safeDestroy(m_descriptorPool);
    safeDestroy(m_descriptorLayout);
    safeDestroy(m_sampler);
    safeDestroy(m_noiseSampler);

    safeDestroy(m_noiseView);
    safeDestroy(m_noiseImage);
    safeFree(m_noiseMemory);

    safeDestroy(m_noiseStagingBuffer);
    safeFree(m_noiseStagingMemory);

    destroyOutputImage();

    m_descriptorSet = VK_NULL_HANDLE;
    m_noiseUploaded = false;
}

void SSAOPass::uploadNoiseTexture(VkCommandBuffer cmd) {
    if (m_noiseStagingBuffer == VK_NULL_HANDLE || m_noiseImage == VK_NULL_HANDLE) {
        m_noiseUploaded = true;
        return;
    }

    // Transition noise image from UNDEFINED to TRANSFER_DST
    transitionImage(cmd, m_noiseImage,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, VK_ACCESS_TRANSFER_WRITE_BIT);

    // Copy staging buffer to image
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {4, 4, 1};

    vkCmdCopyBufferToImage(cmd, m_noiseStagingBuffer, m_noiseImage,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition noise image to SHADER_READ_ONLY
    transitionImage(cmd, m_noiseImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    m_noiseUploaded = true;
}

void SSAOPass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (m_depthView == VK_NULL_HANDLE || m_normalView == VK_NULL_HANDLE) return;
    if (m_pipeline == VK_NULL_HANDLE || m_pipelineLayout == VK_NULL_HANDLE ||
        m_descriptorSet == VK_NULL_HANDLE) return;

    if (!m_noiseUploaded) {
        uploadNoiseTexture(cmd);
    }

    // Transition AO output to general for compute write
    transitionImage(cmd, m_aoOutput,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, VK_ACCESS_SHADER_WRITE_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                            0, 1, &m_descriptorSet, 0, nullptr);

    SSAOParams params{};
    params.projection = m_projection;
    params.invProjection = m_invProjection;
    params.noiseScale = glm::vec4(
        static_cast<float>(m_width) / 4.0f,
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

    uint32_t groupsX = (m_width + 7) / 8;
    uint32_t groupsY = (m_height + 7) / 8;
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    // Transition AO output to shader read
    transitionImage(cmd, m_aoOutput,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
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

    VkImageView fallbackView = m_noiseView != VK_NULL_HANDLE ? m_noiseView : m_aoOutputView;
    VkSampler fallbackSampler = m_noiseSampler != VK_NULL_HANDLE ? m_noiseSampler : m_sampler;
    if (fallbackView == VK_NULL_HANDLE) return;

    std::array<VkDescriptorImageInfo, 4> imageInfos{};
    std::array<VkWriteDescriptorSet, 4> writes{};

    // Binding 0: Depth buffer
    imageInfos[0].sampler = m_sampler;
    imageInfos[0].imageView = m_depthView != VK_NULL_HANDLE ? m_depthView : fallbackView;
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = m_descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &imageInfos[0];

    // Binding 1: Normal buffer
    imageInfos[1].sampler = m_sampler;
    imageInfos[1].imageView = m_normalView != VK_NULL_HANDLE ? m_normalView : fallbackView;
    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = m_descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &imageInfos[1];

    // Binding 2: AO output (storage image)
    imageInfos[2].sampler = VK_NULL_HANDLE;
    imageInfos[2].imageView = m_aoOutputView;
    imageInfos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = m_descriptorSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].descriptorCount = 1;
    writes[2].pImageInfo = &imageInfos[2];

    // Binding 3: Noise texture
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
    // Create AO render target using helper
    RenderTarget rt = createRenderTarget(VK_FORMAT_R16_SFLOAT, m_width, m_height,
                                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (rt.image == VK_NULL_HANDLE) return false;

    m_aoOutput = rt.image;
    m_aoMemory = rt.memory;
    m_aoOutputView = rt.view;

    // Create sampler (only once)
    if (m_sampler == VK_NULL_HANDLE) {
        m_sampler = createSampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        if (m_sampler == VK_NULL_HANDLE) return false;
    }

    return true;
}

void SSAOPass::destroyOutputImage() {
    safeDestroy(m_aoOutputView);
    safeDestroy(m_aoOutput);
    safeFree(m_aoMemory);
}

bool SSAOPass::createNoiseTexture() {
    // Generate 4x4 noise texture with random unit vectors
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    std::array<glm::vec4, 16> noiseData;
    for (auto& v : noiseData) {
        v = glm::vec4(dist(gen), dist(gen), 0.0f, 0.0f);
        float len = std::sqrt(v.x * v.x + v.y * v.y);
        if (len > 0.0001f) { v.x /= len; v.y /= len; }
    }

    // Create staging buffer
    VkDeviceSize bufferSize = sizeof(noiseData);

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VkBuffer stagingBuffer;
    vkCreateBuffer(m_device, &bufferInfo, nullptr, &stagingBuffer);

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(m_device, stagingBuffer, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkDeviceMemory stagingMemory;
    vkAllocateMemory(m_device, &allocInfo, nullptr, &stagingMemory);
    vkBindBufferMemory(m_device, stagingBuffer, stagingMemory, 0);

    void* data;
    vkMapMemory(m_device, stagingMemory, 0, bufferSize, 0, &data);
    memcpy(data, noiseData.data(), bufferSize);
    vkUnmapMemory(m_device, stagingMemory);

    // Create noise image + view using helper
    RenderTarget noiseRT = createRenderTarget(VK_FORMAT_R32G32B32A32_SFLOAT, 4, 4,
                                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (noiseRT.image == VK_NULL_HANDLE) return false;

    m_noiseImage = noiseRT.image;
    m_noiseMemory = noiseRT.memory;
    m_noiseView = noiseRT.view;

    // Repeating sampler for noise
    m_noiseSampler = createSampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT);

    // Keep staging buffer alive for deferred upload in first execute() call
    m_noiseStagingBuffer = stagingBuffer;
    m_noiseStagingMemory = stagingMemory;
    m_noiseUploaded = false;

    return true;
}

bool SSAOPass::createDescriptors() {
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

    safeDestroy(m_descriptorPool);

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo dsAllocInfo{};
    dsAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAllocInfo.descriptorPool = m_descriptorPool;
    dsAllocInfo.descriptorSetCount = 1;
    dsAllocInfo.pSetLayouts = &m_descriptorLayout;

    return vkAllocateDescriptorSets(m_device, &dsAllocInfo, &m_descriptorSet) == VK_SUCCESS;
}

bool SSAOPass::createPipeline() {
    return createComputePipeline("compute_ssao.comp.spv", m_descriptorLayout,
                                  sizeof(SSAOParams), m_pipeline, m_pipelineLayout);
}

} // namespace ohao
