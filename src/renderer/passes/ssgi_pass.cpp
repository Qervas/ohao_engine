#include "ssgi_pass.hpp"
#include <array>
#include <random>
#include <cmath>

namespace ohao {

SSGIPass::~SSGIPass() {
    cleanup();
}

bool SSGIPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;

    m_fullWidth = 1920;
    m_fullHeight = 1080;

    if (!createOutputImage()) return false;
    if (!createNoiseTexture()) return false;
    if (!createDescriptors()) return false;
    if (!createPipeline()) return false;

    return true;
}

void SSGIPass::cleanup() {
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

void SSGIPass::uploadNoiseTexture(VkCommandBuffer cmd) {
    if (m_noiseStagingBuffer == VK_NULL_HANDLE || m_noiseImage == VK_NULL_HANDLE) {
        m_noiseUploaded = true;
        return;
    }

    transitionImage(cmd, m_noiseImage,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, VK_ACCESS_TRANSFER_WRITE_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {4, 4, 1};

    vkCmdCopyBufferToImage(cmd, m_noiseStagingBuffer, m_noiseImage,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    transitionImage(cmd, m_noiseImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    m_noiseUploaded = true;
}

void SSGIPass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (m_depthView == VK_NULL_HANDLE || m_normalView == VK_NULL_HANDLE ||
        m_albedoView == VK_NULL_HANDLE || m_positionView == VK_NULL_HANDLE) return;
    if (m_pipeline == VK_NULL_HANDLE || m_pipelineLayout == VK_NULL_HANDLE ||
        m_descriptorSet == VK_NULL_HANDLE) return;

    if (!m_noiseUploaded) {
        uploadNoiseTexture(cmd);
    }

    uint32_t halfW = m_fullWidth / 2;
    uint32_t halfH = m_fullHeight / 2;

    // Transition GI output to general for compute write
    transitionImage(cmd, m_giOutput,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, VK_ACCESS_SHADER_WRITE_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipelineLayout,
                            0, 1, &m_descriptorSet, 0, nullptr);

    SSGIParams params{};
    params.view = m_view;
    params.projection = m_projection;
    params.invProjection = m_invProjection;
    params.screenParams = glm::vec4(
        static_cast<float>(m_fullWidth),
        static_cast<float>(m_fullHeight),
        static_cast<float>(halfW),
        static_cast<float>(halfH)
    );
    params.radius = m_radius;
    params.intensity = m_intensity;
    params.sampleCount = m_sampleCount;
    params.maxSteps = m_maxSteps;
    params.texelSize = glm::vec2(1.0f / m_fullWidth, 1.0f / m_fullHeight);
    params.thickness = m_thickness;
    params.falloff = m_falloff;

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);

    uint32_t groupsX = (halfW + 7) / 8;
    uint32_t groupsY = (halfH + 7) / 8;
    vkCmdDispatch(cmd, groupsX, groupsY, 1);

    // Transition GI output to shader read
    transitionImage(cmd, m_giOutput,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
}

void SSGIPass::onResize(uint32_t width, uint32_t height) {
    if (width == m_fullWidth && height == m_fullHeight) return;

    m_fullWidth = width;
    m_fullHeight = height;

    vkDeviceWaitIdle(m_device);

    destroyOutputImage();
    createOutputImage();
    createDescriptors();
}

void SSGIPass::setMatrices(const glm::mat4& view, const glm::mat4& proj, const glm::mat4& invProj) {
    m_view = view;
    m_projection = proj;
    m_invProjection = invProj;
}

void SSGIPass::setDepthBuffer(VkImageView depth) {
    m_depthView = depth;
    updateDescriptorSet();
}

void SSGIPass::setNormalBuffer(VkImageView normal) {
    m_normalView = normal;
    updateDescriptorSet();
}

void SSGIPass::setAlbedoBuffer(VkImageView albedo) {
    m_albedoView = albedo;
    updateDescriptorSet();
}

void SSGIPass::setPositionBuffer(VkImageView position) {
    m_positionView = position;
    updateDescriptorSet();
}

void SSGIPass::updateDescriptorSet() {
    if (m_descriptorSet == VK_NULL_HANDLE || m_sampler == VK_NULL_HANDLE) return;

    VkImageView fallbackView = m_noiseView != VK_NULL_HANDLE ? m_noiseView : m_giOutputView;
    VkSampler fallbackSampler = m_noiseSampler != VK_NULL_HANDLE ? m_noiseSampler : m_sampler;
    if (fallbackView == VK_NULL_HANDLE) return;

    std::array<VkDescriptorImageInfo, 6> imageInfos{};
    std::array<VkWriteDescriptorSet, 6> writes{};

    // Bindings 0-3: G-Buffer inputs (depth, normal, albedo, position)
    VkImageView gbufferViews[] = {m_depthView, m_normalView, m_albedoView, m_positionView};
    for (uint32_t i = 0; i < 4; ++i) {
        imageInfos[i].sampler = m_sampler;
        imageInfos[i].imageView = gbufferViews[i] != VK_NULL_HANDLE ? gbufferViews[i] : fallbackView;
        imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_descriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo = &imageInfos[i];
    }

    // Binding 4: GI output (storage image)
    imageInfos[4].sampler = VK_NULL_HANDLE;
    imageInfos[4].imageView = m_giOutputView;
    imageInfos[4].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = m_descriptorSet;
    writes[4].dstBinding = 4;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[4].descriptorCount = 1;
    writes[4].pImageInfo = &imageInfos[4];

    // Binding 5: Noise texture
    imageInfos[5].sampler = fallbackSampler;
    imageInfos[5].imageView = m_noiseView != VK_NULL_HANDLE ? m_noiseView : fallbackView;
    imageInfos[5].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[5].dstSet = m_descriptorSet;
    writes[5].dstBinding = 5;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[5].descriptorCount = 1;
    writes[5].pImageInfo = &imageInfos[5];

    vkUpdateDescriptorSets(m_device, 6, writes.data(), 0, nullptr);
}

bool SSGIPass::createOutputImage() {
    uint32_t halfW = m_fullWidth / 2;
    uint32_t halfH = m_fullHeight / 2;
    if (halfW == 0) halfW = 1;
    if (halfH == 0) halfH = 1;

    RenderTarget rt = createRenderTarget(VK_FORMAT_R16G16B16A16_SFLOAT, halfW, halfH,
                                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (rt.image == VK_NULL_HANDLE) return false;

    m_giOutput = rt.image;
    m_giMemory = rt.memory;
    m_giOutputView = rt.view;

    // Create linear sampler (only once)
    if (m_sampler == VK_NULL_HANDLE) {
        m_sampler = createSampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        if (m_sampler == VK_NULL_HANDLE) return false;
    }

    return true;
}

void SSGIPass::destroyOutputImage() {
    safeDestroy(m_giOutputView);
    safeDestroy(m_giOutput);
    safeFree(m_giMemory);
}

bool SSGIPass::createNoiseTexture() {
    // Generate 4x4 noise texture with cosine-weighted hemisphere directions
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    std::array<glm::vec4, 16> noiseData;
    for (auto& v : noiseData) {
        v = glm::vec4(dist(gen), dist(gen), dist(gen), dist(gen));
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

    m_noiseStagingBuffer = stagingBuffer;
    m_noiseStagingMemory = stagingMemory;
    m_noiseUploaded = false;

    return true;
}

bool SSGIPass::createDescriptors() {
    std::array<VkDescriptorSetLayoutBinding, 6> bindings{};

    for (uint32_t i = 0; i < 4; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

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
    poolSizes[0].descriptorCount = 5;
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

bool SSGIPass::createPipeline() {
    return createComputePipeline("compute_ssgi.comp.spv", m_descriptorLayout,
                                  sizeof(SSGIParams), m_pipeline, m_pipelineLayout);
}

} // namespace ohao
