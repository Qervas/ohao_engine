#include "cloud_shadow_pass.hpp"
#include <array>

namespace ohao {

CloudShadowPass::~CloudShadowPass() {
    cleanup();
}

bool CloudShadowPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;

    if (!createShadowOutput()) return false;
    if (!createDescriptors()) return false;
    if (!createPipelineResources()) return false;

    return true;
}

void CloudShadowPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);

    safeDestroy(m_pipeline);
    safeDestroy(m_pipelineLayout);
    safeDestroy(m_descriptorPool);
    safeDestroy(m_descriptorLayout);
    safeDestroy(m_shadowSampler);
    m_shadowOutput.destroy(m_device);
    m_descriptorSet = VK_NULL_HANDLE;
}

void CloudShadowPass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (m_pipeline == VK_NULL_HANDLE) return;
    if (m_noiseView == VK_NULL_HANDLE || m_weatherView == VK_NULL_HANDLE) return;

    // Transition shadow output UNDEFINED → GENERAL on first use
    if (!m_shadowOutputReady) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_shadowOutput.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        m_shadowOutputReady = true;
    }

    // Update descriptors if textures changed
    if (m_descriptorsDirty) {
        updateDescriptors();
        m_descriptorsDirty = false;
    }

    // Fill push constants
    ShadowParams pc{};
    pc.mapCenter = m_mapCenter;
    pc.mapExtent = m_mapExtent;
    pc.outputSize = glm::vec2(SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
    pc.cloudAltMin = m_altMin;
    pc.cloudAltMax = m_altMax;
    pc.cloudCoverage = m_coverage;
    pc.cloudDensity = m_density;
    pc.cloudAbsorption = m_absorption;
    pc.time = m_time;
    pc.cloudSpeed = m_speed;
    pc.sunDirection = m_sunDir;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(ShadowParams), &pc);

    uint32_t gx = (SHADOW_MAP_SIZE + 7) / 8;
    uint32_t gy = (SHADOW_MAP_SIZE + 7) / 8;
    vkCmdDispatch(cmd, gx, gy, 1);

    // Barrier: compute write → fragment read
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_shadowOutput.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void CloudShadowPass::setNoiseTexture(VkImageView view, VkSampler sampler) {
    if (m_noiseView != view || m_noiseSampler != sampler) {
        m_noiseView = view;
        m_noiseSampler = sampler;
        m_descriptorsDirty = true;
    }
}

void CloudShadowPass::setWeatherTexture(VkImageView view, VkSampler sampler) {
    if (m_weatherView != view || m_weatherSampler != sampler) {
        m_weatherView = view;
        m_weatherSampler = sampler;
        m_descriptorsDirty = true;
    }
}

void CloudShadowPass::setCameraPos(const glm::vec3& pos) {
    // Center shadow map on camera XZ position
    m_mapCenter = glm::vec2(pos.x, pos.z);
}

void CloudShadowPass::setCloudParams(float altMin, float altMax, float coverage,
                                      float density, float absorption, float speed) {
    m_altMin = altMin;
    m_altMax = altMax;
    m_coverage = coverage;
    m_density = density;
    m_absorption = absorption;
    m_speed = speed;
}

// ---------------------------------------------------------------------------
// GPU Resource Creation
// ---------------------------------------------------------------------------

bool CloudShadowPass::createShadowOutput() {
    m_shadowOutput = createRenderTarget(
        VK_FORMAT_R16_SFLOAT,
        SHADOW_MAP_SIZE, SHADOW_MAP_SIZE,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

    if (m_shadowOutput.image == VK_NULL_HANDLE) return false;

    // Create sampler for deferred lighting to read the shadow map
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    return vkCreateSampler(m_device, &samplerInfo, nullptr, &m_shadowSampler) == VK_SUCCESS;
}

bool CloudShadowPass::createDescriptors() {
    // Binding 0: storage image output
    // Binding 1: noise texture (sampler3D)
    // Binding 2: weather map (sampler2D)
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorLayout) != VK_SUCCESS) {
        return false;
    }

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = 2;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = 1;

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

bool CloudShadowPass::createPipelineResources() {
    return createComputePipeline("compute_cloud_shadow.comp.spv",
                                 m_descriptorLayout,
                                 sizeof(ShadowParams),
                                 m_pipeline, m_pipelineLayout);
}

void CloudShadowPass::updateDescriptors() {
    if (m_descriptorSet == VK_NULL_HANDLE) return;
    if (m_noiseView == VK_NULL_HANDLE || m_weatherView == VK_NULL_HANDLE) return;

    // Binding 0: output shadow image
    VkDescriptorImageInfo outputInfo{};
    outputInfo.imageView = m_shadowOutput.view;
    outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    // Binding 1: noise 3D
    VkDescriptorImageInfo noiseInfo{};
    noiseInfo.sampler = m_noiseSampler;
    noiseInfo.imageView = m_noiseView;
    noiseInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    // Binding 2: weather map
    VkDescriptorImageInfo weatherInfo{};
    weatherInfo.sampler = m_weatherSampler;
    weatherInfo.imageView = m_weatherView;
    weatherInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    std::array<VkWriteDescriptorSet, 3> writes{};

    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = m_descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &outputInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = m_descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &noiseInfo;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = m_descriptorSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].descriptorCount = 1;
    writes[2].pImageInfo = &weatherInfo;

    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

bool CloudShadowPass::reloadShader(const std::string& spvPath) {
    return reloadComputeShader(spvPath, m_descriptorLayout, sizeof(ShadowParams),
                               m_pipeline, m_pipelineLayout);
}

} // namespace ohao
