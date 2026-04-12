#include "deferred_lighting_pass.hpp"
#include <array>
#include <cstring>

namespace ohao {

DeferredLightingPass::~DeferredLightingPass() {
    cleanup();
}

bool DeferredLightingPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;

    m_width = 1920;
    m_height = 1080;

    if (!createOutputImage()) return false;
    if (!createRenderPass()) return false;
    if (!createFramebuffer()) return false;
    if (!createDescriptors()) return false;
    if (!createPipeline()) return false;

    // Create G-Buffer sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_gbufferSampler) != VK_SUCCESS) {
        return false;
    }

    // Create dummy resources for fallback descriptor bindings
    if (!createDummyResources()) {
        return false;
    }

    return true;
}

void DeferredLightingPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_gbufferSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, m_gbufferSampler, nullptr);
        m_gbufferSampler = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_descriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_descriptorLayout, nullptr);
        m_descriptorLayout = VK_NULL_HANDLE;
    }
    if (m_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
        m_framebuffer = VK_NULL_HANDLE;
    }
    if (m_renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }

    m_hdrOutput.destroy(m_device);
    destroyDummyResources();
}

void DeferredLightingPass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (!m_gbufferPass) return;
    if (m_pipeline == VK_NULL_HANDLE || m_pipelineLayout == VK_NULL_HANDLE ||
        m_descriptorSet == VK_NULL_HANDLE) return;

    // Update descriptors every frame — light buffer, SSAO, and SSGI views are set
    // per-frame by the deferred renderer but the descriptor set is only written at
    // init. Without this, bindings 5/10/11 always point to dummy zero resources.
    // vkUpdateDescriptorSets is a cheap host-side call (~12 writes, microseconds).
    updateDescriptorSets();

    // Transition dummy image on first use (needed for fallback descriptor bindings)
    if (!m_dummyImageTransitioned && m_dummyImage != VK_NULL_HANDLE) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_dummyImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
        m_dummyImageTransitioned = true;
    }

    // Begin render pass
    VkClearValue clearValue{};
    clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = m_framebuffer;
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {m_width, m_height};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Set viewport and scissor
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(m_width);
    viewport.height = static_cast<float>(m_height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {m_width, m_height};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Bind pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    // Bind descriptor set (G-Buffer textures, lights, shadows, IBL)
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    // Push constants
    m_params.screenSize = glm::vec2(m_width, m_height);
    m_params.lightCount = m_lightCount;
    m_params.flags = 0;
    if (m_irradianceView != VK_NULL_HANDLE) m_params.flags |= 1; // IBL
    if (m_ssaoView != VK_NULL_HANDLE) m_params.flags |= 2;       // SSAO
    if (m_rtShadowView != VK_NULL_HANDLE)
        m_params.flags |= 32;  // RT shadows (bit 5)
    else if (m_shadowMapView != VK_NULL_HANDLE)
        m_params.flags |= 4;   // CSM fallback (bit 2)
    if (m_ssgiView != VK_NULL_HANDLE) m_params.flags |= 8;       // SSGI
    if (m_cloudShadowView != VK_NULL_HANDLE) m_params.flags |= 16; // Cloud shadows

    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(LightingParams), &m_params);

    // Draw fullscreen triangle
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd);
}

void DeferredLightingPass::onResize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;

    vkDeviceWaitIdle(m_device);

    if (m_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
    }

    m_hdrOutput.destroy(m_device);
    createOutputImage();
    createFramebuffer();
}

void DeferredLightingPass::setShadowMap(VkImageView shadowMap, VkSampler shadowSampler) {
    m_shadowMapView = shadowMap;
    m_shadowSampler = shadowSampler;
}

void DeferredLightingPass::setIBLTextures(VkImageView irradiance, VkImageView prefiltered,
                                          VkImageView brdfLUT, VkSampler iblSampler) {
    m_irradianceView = irradiance;
    m_prefilteredView = prefiltered;
    m_brdfLUTView = brdfLUT;
    m_iblSampler = iblSampler;
}

void DeferredLightingPass::setSSAOTexture(VkImageView ssao, VkSampler ssaoSampler) {
    m_ssaoView = ssao;
    m_ssaoSampler = ssaoSampler;
}

void DeferredLightingPass::setSSGITexture(VkImageView ssgi, VkSampler ssgiSampler) {
    m_ssgiView = ssgi;
    m_ssgiSampler = ssgiSampler;
}

void DeferredLightingPass::setCloudShadow(VkImageView view, VkSampler sampler,
                                           const glm::vec2& center, const glm::vec2& extent) {
    m_cloudShadowView = view;
    m_cloudShadowSampler = sampler;
    m_params.cloudShadowCenter = center;
    m_params.cloudShadowExtent = extent;
}

void DeferredLightingPass::setEnvMap(VkImageView view, VkSampler sampler) {
    m_envMapView = view;
    m_envMapSampler = sampler;
}

void DeferredLightingPass::setRTShadowMask(VkImageView view, VkSampler sampler) {
    m_rtShadowView = view;
    m_rtShadowSampler = sampler;
    // Set flag bit 5 to tell the shader to use RT shadows
    if (view != VK_NULL_HANDLE) {
        m_params.flags |= 32u;   // bit 5 = RT shadows
        m_params.flags &= ~4u;   // disable CSM (bit 2) when RT is active
    } else {
        m_params.flags &= ~32u;
    }
}

void DeferredLightingPass::setCameraData(const glm::vec3& position, const glm::mat4& view,
                                          const glm::mat4& invViewProj) {
    m_params.cameraPos = position;
    m_params.view = view;
    m_params.invViewProj = invViewProj;
}

void DeferredLightingPass::setGBufferPass(GBufferPass* gbufferPass) {
    m_gbufferPass = gbufferPass;
    updateDescriptorSets();
}

void DeferredLightingPass::updateDescriptorSets() {
    if (!m_gbufferPass || m_descriptorSet == VK_NULL_HANDLE || m_gbufferSampler == VK_NULL_HANDLE) return;

    // Always write ALL 14 bindings. Use dummy resources as fallback for unbound bindings.
    // Vulkan requires all declared bindings to be written before the descriptor set is used.
    VkImageView fallbackView = m_dummyView;
    VkSampler fallbackSampler = m_gbufferSampler;

    std::array<VkDescriptorImageInfo, 15> imageInfos{};
    std::array<VkWriteDescriptorSet, 16> writes{};
    VkDescriptorBufferInfo bufferInfo{};
    VkDescriptorBufferInfo cascadeBufferInfo{};

    // G-Buffer textures (bindings 0-4) — always available after GBuffer init
    std::array<VkImageView, 5> gbufferViews = {
        m_gbufferPass->getPositionView(),
        m_gbufferPass->getNormalView(),
        m_gbufferPass->getAlbedoView(),
        m_gbufferPass->getVelocityView(),
        m_gbufferPass->getDepthView()
    };

    for (uint32_t i = 0; i < 5; ++i) {
        VkImageView view = gbufferViews[i] != VK_NULL_HANDLE ? gbufferViews[i] : fallbackView;

        imageInfos[i].sampler = m_gbufferSampler;
        imageInfos[i].imageView = view;
        // Use depth layout only for actual depth views, not fallback color views
        imageInfos[i].imageLayout = (i == 4 && gbufferViews[i] != VK_NULL_HANDLE)
                                    ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                    : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_descriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo = &imageInfos[i];
    }

    // Light buffer (binding 5) — use dummy buffer as fallback
    bufferInfo.buffer = m_lightBuffer != VK_NULL_HANDLE ? m_lightBuffer : m_dummyBuffer;
    bufferInfo.offset = 0;
    bufferInfo.range = VK_WHOLE_SIZE;

    writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[5].dstSet = m_descriptorSet;
    writes[5].dstBinding = 5;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[5].descriptorCount = 1;
    writes[5].pBufferInfo = &bufferInfo;

    // Shadow map (binding 6) — use fallback if not set
    imageInfos[5].sampler = m_shadowSampler != VK_NULL_HANDLE ? m_shadowSampler : fallbackSampler;
    imageInfos[5].imageView = m_shadowMapView != VK_NULL_HANDLE ? m_shadowMapView : fallbackView;
    imageInfos[5].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[6].dstSet = m_descriptorSet;
    writes[6].dstBinding = 6;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[6].descriptorCount = 1;
    writes[6].pImageInfo = &imageInfos[5];

    // IBL textures (bindings 7-9) — use fallback if not set
    VkSampler iblSampler = m_iblSampler != VK_NULL_HANDLE ? m_iblSampler : fallbackSampler;

    imageInfos[6].sampler = iblSampler;
    imageInfos[6].imageView = m_irradianceView != VK_NULL_HANDLE ? m_irradianceView : fallbackView;
    imageInfos[6].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[7].dstSet = m_descriptorSet;
    writes[7].dstBinding = 7;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[7].descriptorCount = 1;
    writes[7].pImageInfo = &imageInfos[6];

    imageInfos[7].sampler = iblSampler;
    imageInfos[7].imageView = m_prefilteredView != VK_NULL_HANDLE ? m_prefilteredView : fallbackView;
    imageInfos[7].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[8].dstSet = m_descriptorSet;
    writes[8].dstBinding = 8;
    writes[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[8].descriptorCount = 1;
    writes[8].pImageInfo = &imageInfos[7];

    imageInfos[8].sampler = iblSampler;
    imageInfos[8].imageView = m_brdfLUTView != VK_NULL_HANDLE ? m_brdfLUTView : fallbackView;
    imageInfos[8].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[9].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[9].dstSet = m_descriptorSet;
    writes[9].dstBinding = 9;
    writes[9].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[9].descriptorCount = 1;
    writes[9].pImageInfo = &imageInfos[8];

    // SSAO (binding 10) — use fallback if not set or sampler is null
    imageInfos[9].sampler = m_ssaoSampler != VK_NULL_HANDLE ? m_ssaoSampler : fallbackSampler;
    imageInfos[9].imageView = m_ssaoView != VK_NULL_HANDLE ? m_ssaoView : fallbackView;
    imageInfos[9].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[10].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[10].dstSet = m_descriptorSet;
    writes[10].dstBinding = 10;
    writes[10].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[10].descriptorCount = 1;
    writes[10].pImageInfo = &imageInfos[9];

    // SSGI (binding 11) — use fallback if not set
    imageInfos[10].sampler = m_ssgiSampler != VK_NULL_HANDLE ? m_ssgiSampler : fallbackSampler;
    imageInfos[10].imageView = m_ssgiView != VK_NULL_HANDLE ? m_ssgiView : fallbackView;
    imageInfos[10].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[11].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[11].dstSet = m_descriptorSet;
    writes[11].dstBinding = 11;
    writes[11].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[11].descriptorCount = 1;
    writes[11].pImageInfo = &imageInfos[10];

    // CascadeData UBO (binding 12) — use dummy buffer if not yet set
    cascadeBufferInfo.buffer = m_cascadeBuffer != VK_NULL_HANDLE ? m_cascadeBuffer : m_dummyBuffer;
    cascadeBufferInfo.offset = 0;
    cascadeBufferInfo.range = VK_WHOLE_SIZE;

    writes[12].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[12].dstSet = m_descriptorSet;
    writes[12].dstBinding = 12;
    writes[12].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[12].descriptorCount = 1;
    writes[12].pBufferInfo = &cascadeBufferInfo;

    // Cloud shadow map (binding 13) — use fallback if not set
    imageInfos[11].sampler = m_cloudShadowSampler != VK_NULL_HANDLE ? m_cloudShadowSampler : fallbackSampler;
    imageInfos[11].imageView = m_cloudShadowView != VK_NULL_HANDLE ? m_cloudShadowView : fallbackView;
    imageInfos[11].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    writes[13].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[13].dstSet = m_descriptorSet;
    writes[13].dstBinding = 13;
    writes[13].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[13].descriptorCount = 1;
    writes[13].pImageInfo = &imageInfos[11];

    // RT shadow mask (binding 14)
    imageInfos[12].sampler = m_rtShadowSampler != VK_NULL_HANDLE ? m_rtShadowSampler : fallbackSampler;
    imageInfos[12].imageView = m_rtShadowView != VK_NULL_HANDLE ? m_rtShadowView : fallbackView;
    imageInfos[12].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[14].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[14].dstSet = m_descriptorSet;
    writes[14].dstBinding = 14;
    writes[14].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[14].descriptorCount = 1;
    writes[14].pImageInfo = &imageInfos[12];

    // Binding 15: HDR environment map
    imageInfos[13].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // Use env map if available, else RT shadow view (or any valid view) as dummy
    imageInfos[13].imageView = m_envMapView ? m_envMapView : (m_rtShadowView ? m_rtShadowView : imageInfos[0].imageView);
    imageInfos[13].sampler = m_envMapSampler ? m_envMapSampler : m_gbufferSampler;

    writes[15].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[15].dstSet = m_descriptorSet;
    writes[15].dstBinding = 15;
    writes[15].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[15].descriptorCount = 1;
    writes[15].pImageInfo = &imageInfos[13];

    vkUpdateDescriptorSets(m_device, 16, writes.data(), 0, nullptr);
}

bool DeferredLightingPass::createOutputImage() {
    m_hdrOutput.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    m_hdrOutput.width = m_width;
    m_hdrOutput.height = m_height;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = m_hdrOutput.format;
    imageInfo.extent = {m_width, m_height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_device, &imageInfo, nullptr, &m_hdrOutput.image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(m_device, m_hdrOutput.image, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_hdrOutput.memory) != VK_SUCCESS) {
        return false;
    }

    vkBindImageMemory(m_device, m_hdrOutput.image, m_hdrOutput.memory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_hdrOutput.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_hdrOutput.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    return vkCreateImageView(m_device, &viewInfo, nullptr, &m_hdrOutput.view) == VK_SUCCESS;
}

bool DeferredLightingPass::createRenderPass() {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    return vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass) == VK_SUCCESS;
}

bool DeferredLightingPass::createFramebuffer() {
    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = m_renderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = &m_hdrOutput.view;
    framebufferInfo.width = m_width;
    framebufferInfo.height = m_height;
    framebufferInfo.layers = 1;

    return vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_framebuffer) == VK_SUCCESS;
}

bool DeferredLightingPass::createDescriptors() {
    // Bindings:
    // 0-4: G-Buffer (position, normal, albedo, velocity, depth)
    // 5: Light buffer (SSBO)
    // 6: Shadow map array (sampler2DArray, 4 cascades)
    // 7: Irradiance cubemap
    // 8: Prefiltered cubemap
    // 9: BRDF LUT
    // 10: SSAO texture
    // 11: SSGI texture
    // 12: CascadeData UBO
    // 13: Cloud shadow map

    std::array<VkDescriptorSetLayoutBinding, 16> bindings{};

    // G-Buffer samplers (0-4)
    for (uint32_t i = 0; i < 5; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    // Light buffer (5) — matches shader's "uniform LightingUBO" declaration
    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Shadow map (6)
    bindings[6].binding = 6;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // IBL textures (7-9)
    for (uint32_t i = 7; i <= 9; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    // SSAO (10)
    bindings[10].binding = 10;
    bindings[10].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[10].descriptorCount = 1;
    bindings[10].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // SSGI (11)
    bindings[11].binding = 11;
    bindings[11].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[11].descriptorCount = 1;
    bindings[11].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // CascadeData UBO (12)
    bindings[12].binding = 12;
    bindings[12].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[12].descriptorCount = 1;
    bindings[12].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // Cloud shadow map (13)
    bindings[13].binding = 13;
    bindings[13].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[13].descriptorCount = 1;
    bindings[13].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // RT shadow mask (14)
    bindings[14].binding = 14;
    bindings[14].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[14].descriptorCount = 1;
    bindings[14].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // HDR environment map (15)
    bindings[15].binding = 15;
    bindings[15].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[15].descriptorCount = 1;
    bindings[15].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorLayout) != VK_SUCCESS) {
        return false;
    }

    // Descriptor pool — 12 image samplers + 2 UBOs (light buffer + cascade data)
    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 14;  // +1 for env map
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 2;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = 1;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        return false;
    }

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorLayout;

    return vkAllocateDescriptorSets(m_device, &allocInfo, &m_descriptorSet) == VK_SUCCESS;
}

bool DeferredLightingPass::createPipeline() {
    // Load shaders
    VkShaderModule vertShader = loadShaderModule("postprocess_fullscreen.vert.spv");
    VkShaderModule fragShader = loadShaderModule("core_deferred_lighting.frag.spv");

    std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages{};

    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = vertShader;
    shaderStages[0].pName = "main";

    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = fragShader;
    shaderStages[1].pName = "main";

    // No vertex input (fullscreen triangle generated in shader)
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    std::array<VkDynamicState, 2> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    // Push constants
    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(LightingParams);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_descriptorLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, vertShader, nullptr);
        vkDestroyShaderModule(m_device, fragShader, nullptr);
        return false;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = m_renderPass;
    pipelineInfo.subpass = 0;

    VkResult result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                nullptr, &m_pipeline);

    vkDestroyShaderModule(m_device, vertShader, nullptr);
    vkDestroyShaderModule(m_device, fragShader, nullptr);

    return result == VK_SUCCESS;
}

bool DeferredLightingPass::createDummyResources() {
    // 1x1 RGBA8 image for fallback texture bindings
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {1, 1, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_device, &imageInfo, nullptr, &m_dummyImage) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(m_device, m_dummyImage, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_dummyMemory) != VK_SUCCESS) {
        return false;
    }

    vkBindImageMemory(m_device, m_dummyImage, m_dummyMemory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_dummyImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_dummyView) != VK_SUCCESS) {
        return false;
    }

    // 512-byte buffer for fallback SSBO (binding 5) and UBO (binding 12) fallbacks.
    // CascadeData is 288 bytes, so 512 is sufficient for both uses.
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = 512;
    bufInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(m_device, &bufInfo, nullptr, &m_dummyBuffer) != VK_SUCCESS) {
        return false;
    }

    vkGetBufferMemoryRequirements(m_device, m_dummyBuffer, &memReqs);

    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_dummyBufferMemory) != VK_SUCCESS) {
        return false;
    }

    vkBindBufferMemory(m_device, m_dummyBuffer, m_dummyBufferMemory, 0);

    // Zero out the buffer
    void* data;
    vkMapMemory(m_device, m_dummyBufferMemory, 0, 512, 0, &data);
    memset(data, 0, 512);
    vkUnmapMemory(m_device, m_dummyBufferMemory);

    return true;
}

void DeferredLightingPass::destroyDummyResources() {
    if (m_dummyView != VK_NULL_HANDLE) vkDestroyImageView(m_device, m_dummyView, nullptr);
    if (m_dummyImage != VK_NULL_HANDLE) vkDestroyImage(m_device, m_dummyImage, nullptr);
    if (m_dummyMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_dummyMemory, nullptr);
    if (m_dummyBuffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, m_dummyBuffer, nullptr);
    if (m_dummyBufferMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_dummyBufferMemory, nullptr);
    m_dummyView = VK_NULL_HANDLE;
    m_dummyImage = VK_NULL_HANDLE;
    m_dummyMemory = VK_NULL_HANDLE;
    m_dummyBuffer = VK_NULL_HANDLE;
    m_dummyBufferMemory = VK_NULL_HANDLE;
    m_dummyImageTransitioned = false;
}

} // namespace ohao
