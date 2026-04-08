#include "caustics_pass.hpp"
#include <array>
#include <iostream>

namespace ohao {

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

CausticsPass::~CausticsPass() {
    cleanup();
}

bool CausticsPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device         = device;
    m_physicalDevice = physicalDevice;

    // Own depth sampler (NEAREST for depth reads)
    {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_NEAREST;
        si.minFilter    = VK_FILTER_NEAREST;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_device, &si, nullptr, &m_ownDepthSampler) != VK_SUCCESS) {
            std::cerr << "CausticsPass: depth sampler creation failed" << std::endl;
            return false;
        }
    }

    // Own linear-repeat sampler (for caustics texture)
    {
        VkSamplerCreateInfo si{};
        si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        si.maxLod       = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(m_device, &si, nullptr, &m_ownLinearSampler) != VK_SUCCESS) {
            std::cerr << "CausticsPass: linear sampler creation failed" << std::endl;
            return false;
        }
    }

    if (!createDescriptors()) {
        std::cerr << "CausticsPass: descriptor creation failed" << std::endl;
        return false;
    }
    if (!createPipeline()) {
        std::cerr << "CausticsPass: pipeline creation failed" << std::endl;
        return false;
    }

    std::cout << "CausticsPass: OK" << std::endl;
    return true;
}

void CausticsPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);

    safeDestroy(m_dummyView);
    safeDestroy(m_dummyImage);
    safeFree(m_dummyMem);

    safeDestroy(m_pipeline);
    safeDestroy(m_pipelineLayout);
    safeDestroy(m_descPool);
    safeDestroy(m_descLayout);
    m_descSet = VK_NULL_HANDLE;

    safeDestroy(m_ownDepthSampler);
    safeDestroy(m_ownLinearSampler);
}

// ---------------------------------------------------------------------------
// Execute
// ---------------------------------------------------------------------------

void CausticsPass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (!m_enabled) return;
    if (m_albedoImage == VK_NULL_HANDLE || m_albedoView == VK_NULL_HANDLE) return;
    if (m_depthView   == VK_NULL_HANDLE) return;
    if (m_pipeline    == VK_NULL_HANDLE) return;

    if (m_descDirty) {
        updateDescriptors();
        m_descDirty = false;
    }
    if (m_descSet == VK_NULL_HANDLE) return;

    // Transition albedo GBuffer: SHADER_READ_ONLY → GENERAL (compute needs STORAGE)
    VkImageMemoryBarrier toGeneral{};
    toGeneral.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGeneral.oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toGeneral.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.image               = m_albedoImage;
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

    CausticsPC pc{};
    pc.invViewProj       = m_invViewProj;
    pc.screenSize        = glm::vec2(static_cast<float>(m_width), static_cast<float>(m_height));
    pc.waterLevel        = m_waterLevel;
    pc.time              = m_time;
    pc.causticsScale     = m_causticsScale;
    pc.causticsIntensity = m_causticsIntensity;
    pc.pad0              = 0.0f;
    pc.pad1              = 0.0f;
    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(CausticsPC), &pc);

    uint32_t groupX = (m_width  + 7u) / 8u;
    uint32_t groupY = (m_height + 7u) / 8u;
    vkCmdDispatch(cmd, groupX, groupY, 1);

    // Transition albedo back to SHADER_READ_ONLY so SSAO/lighting can sample it
    VkImageMemoryBarrier toReadOnly{};
    toReadOnly.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toReadOnly.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    toReadOnly.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toReadOnly.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toReadOnly.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toReadOnly.image               = m_albedoImage;
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

void CausticsPass::onResize(uint32_t width, uint32_t height) {
    m_width  = width;
    m_height = height;
}

// ---------------------------------------------------------------------------
// Resource connections
// ---------------------------------------------------------------------------

void CausticsPass::setGBufferImages(VkImageView depthView,
                                     VkImage albedoImage, VkImageView albedoView) {
    m_depthView    = depthView;
    m_albedoImage  = albedoImage;
    m_albedoView   = albedoView;
    m_descDirty    = true;
}

void CausticsPass::setDepthSampler(VkSampler sampler) {
    m_depthSamplerExt = sampler;
    m_descDirty       = true;
}

void CausticsPass::setCausticsTexture(VkImageView view, VkSampler sampler) {
    m_causticsView    = view;
    m_causticsSampler = sampler;
    m_descDirty       = true;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool CausticsPass::createDummy() {
    if (m_dummyImage != VK_NULL_HANDLE) return true;

    VkImageCreateInfo imgInfo{};
    imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType     = VK_IMAGE_TYPE_2D;
    imgInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
    imgInfo.extent        = {1, 1, 1};
    imgInfo.mipLevels     = 1;
    imgInfo.arrayLayers   = 1;
    imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage         = VK_IMAGE_USAGE_SAMPLED_BIT
                          | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_device, &imgInfo, nullptr, &m_dummyImage) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(m_device, m_dummyImage, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = req.size;
    alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(m_device, &alloc, nullptr, &m_dummyMem) != VK_SUCCESS)
        return false;
    vkBindImageMemory(m_device, m_dummyImage, m_dummyMem, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image                           = m_dummyImage;
    viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format                          = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel   = 0;
    viewInfo.subresourceRange.levelCount     = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount     = 1;
    return vkCreateImageView(m_device, &viewInfo, nullptr, &m_dummyView) == VK_SUCCESS;
}

bool CausticsPass::createDescriptors() {
    // 3 bindings:
    //   0: sceneDepth (COMBINED_IMAGE_SAMPLER — depth read)
    //   1: gbufferAlbedo (STORAGE_IMAGE — R/W)
    //   2: causticsTex (COMBINED_IMAGE_SAMPLER)
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descLayout) != VK_SUCCESS)
        return false;

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 2;
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

bool CausticsPass::createPipeline() {
    return createComputePipeline("water_water_caustics.comp.spv",
                                 m_descLayout,
                                 sizeof(CausticsPC),
                                 m_pipeline, m_pipelineLayout);
}

void CausticsPass::updateDescriptors() {
    if (!m_descSet || !m_depthView || !m_albedoView) return;

    // Ensure dummy caustics texture exists
    createDummy();

    VkSampler depthSampler    = (m_depthSamplerExt != VK_NULL_HANDLE)
                                ? m_depthSamplerExt : m_ownDepthSampler;
    VkImageView causticsView  = (m_causticsView != VK_NULL_HANDLE)
                                ? m_causticsView  : m_dummyView;
    VkSampler causticsSampler = (m_causticsSampler != VK_NULL_HANDLE)
                                ? m_causticsSampler : m_ownLinearSampler;

    if (!depthSampler || !causticsSampler || !m_ownDepthSampler) return;

    VkDescriptorImageInfo depthInfo{};
    depthInfo.sampler     = depthSampler;
    depthInfo.imageView   = m_depthView;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo albedoInfo{};
    albedoInfo.imageView   = m_albedoView;
    albedoInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;  // storage image

    VkDescriptorImageInfo causticsInfo{};
    causticsInfo.sampler     = causticsSampler;
    causticsInfo.imageView   = causticsView ? causticsView : m_dummyView;
    causticsInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    std::array<VkWriteDescriptorSet, 3> writes{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = m_descSet;
    writes[0].dstBinding      = 0;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo      = &depthInfo;

    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = m_descSet;
    writes[1].dstBinding      = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo      = &albedoInfo;

    writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet          = m_descSet;
    writes[2].dstBinding      = 2;
    writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].descriptorCount = 1;
    writes[2].pImageInfo      = &causticsInfo;

    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
}

bool CausticsPass::reloadShader(const std::string& spvPath) {
    return reloadComputeShader(spvPath, m_descLayout, sizeof(CausticsPC),
                               m_pipeline, m_pipelineLayout);
}

} // namespace ohao
