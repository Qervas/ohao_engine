#include "deferred_lighting_pass.hpp"
#include "render/shader_bindings/core_deferred_lighting.hpp"
#include "core/log.hpp"
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

    if (!createOutputImages()) return false;
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
    if (!m_openpbrTables.create(device, physicalDevice)) {
        OHAO_LOG(Error, render, "DeferredLightingPass: failed to upload OpenPBR's energy tables");
        return false;
    }

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

    destroyOutputImages();
    destroyDummyResources();
    m_openpbrTables.destroy();
}

void DeferredLightingPass::execute(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (!m_gbufferPass) return;
    if (m_pipeline == VK_NULL_HANDLE || m_pipelineLayout == VK_NULL_HANDLE ||
        frameIndex >= MAX_FRAMES_IN_FLIGHT || m_descriptorSets[frameIndex] == VK_NULL_HANDLE) return;

    // Inputs (light buffer, RT GI, RT direct light, env map) change between
    // frames, so this frame's set is rewritten from them here, at record time.
    writeDescriptorSet(frameIndex);

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

    // Begin render pass (the lit image)
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clearValues[1].color.uint32[0] = clearValues[1].color.uint32[1] = 0u;
    clearValues[1].color.uint32[2] = clearValues[1].color.uint32[3] = 0u;

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = m_framebuffer;
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {m_width, m_height};
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

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
                            m_pipelineLayout, 0, 1, &m_descriptorSets[frameIndex], 0, nullptr);

    // Push constants
    m_params.screenSize = glm::vec2(m_width, m_height);
    m_params.lightCount = m_lightCount;
    m_params.flags = 0;
    // IBL (bit 0): the prefiltered environment and its irradiance are both bound.
    if (m_prefilteredView != VK_NULL_HANDLE && m_irradianceView != VK_NULL_HANDLE)
        m_params.flags |= 1;
    if (m_directDiffuseView != VK_NULL_HANDLE)
        m_params.flags |= 32;  // RT direct light (bit 5)
    else if (m_shadowMapView != VK_NULL_HANDLE)
        m_params.flags |= 4;   // CSM fallback (bit 2)
    if (m_ssgiView != VK_NULL_HANDLE) m_params.flags |= 8;       // RT GI
    if (m_clusterBuffer == VK_NULL_HANDLE) m_params.flags |= 64; // every light (no froxel lists)

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

    destroyOutputImages();
    if (!createOutputImages() || !createFramebuffer())
        OHAO_LOG(Error, render, "DeferredLightingPass: failed to recreate its outputs at {}x{}", width, height);
}

void DeferredLightingPass::setShadowMap(VkImageView shadowMap, VkSampler shadowSampler) {
    m_shadowMapView = shadowMap;
    m_shadowSampler = shadowSampler;
}

void DeferredLightingPass::setEnvironment(VkImageView prefiltered, VkImageView irradiance, VkSampler sampler,
                                          float intensity) {
    m_prefilteredView = prefiltered;
    m_irradianceView = irradiance;
    m_iblSampler = sampler;
    m_params.envIntensity = intensity;
}

void DeferredLightingPass::setSSGITexture(VkImageView ssgi, VkImageView reflection, VkSampler ssgiSampler) {
    m_ssgiView = ssgi != VK_NULL_HANDLE && reflection != VK_NULL_HANDLE ? ssgi : VK_NULL_HANDLE;
    m_giReflectionView = m_ssgiView != VK_NULL_HANDLE ? reflection : VK_NULL_HANDLE;
    m_ssgiSampler = ssgiSampler;
}

void DeferredLightingPass::setDirectLight(VkImageView diffuse, VkImageView specular) {
    // execute() sets the flags from them
    m_directDiffuseView = diffuse != VK_NULL_HANDLE && specular != VK_NULL_HANDLE ? diffuse : VK_NULL_HANDLE;
    m_directSpecularView = m_directDiffuseView != VK_NULL_HANDLE ? specular : VK_NULL_HANDLE;
}

void DeferredLightingPass::setCameraData(const glm::vec3& position, const glm::mat4& view,
                                          const glm::mat4& invViewProj) {
    m_params.cameraPos = position;
    m_params.view = view;
    m_params.invViewProj = invViewProj;
}

void DeferredLightingPass::clearPerFrameInputs() {
    m_ssgiView = VK_NULL_HANDLE;
    m_giReflectionView = VK_NULL_HANDLE;
    m_ssgiSampler = VK_NULL_HANDLE;
    m_directDiffuseView = VK_NULL_HANDLE;
    m_directSpecularView = VK_NULL_HANDLE;
}

void DeferredLightingPass::setGBufferPass(GBufferPass* gbufferPass) {
    m_gbufferPass = gbufferPass;
}

void DeferredLightingPass::writeDescriptorSet(uint32_t slot) {
    const VkDescriptorSet set = m_descriptorSets[slot];
    if (!m_gbufferPass || set == VK_NULL_HANDLE || m_gbufferSampler == VK_NULL_HANDLE) return;

    // Always write every binding, dummy resources for the unbound ones: Vulkan requires every
    // declared binding written before the set is used.
    VkImageView fallbackView = m_dummyView;
    VkSampler fallbackSampler = m_gbufferSampler;

    namespace L = shader::core_deferred_lighting;
    std::array<VkDescriptorImageInfo, L::kSet0.size()> images{};
    std::array<VkDescriptorBufferInfo, L::kSet0.size()> buffers{};
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(L::kSet0.size());
    const auto image = [&](const shader::Binding& b, VkImageView view, VkSampler sampler,
                           VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        VkDescriptorImageInfo& i = images[writes.size()];
        i = {sampler, view != VK_NULL_HANDLE ? view : fallbackView, layout};
        VkWriteDescriptorSet w = shader::write(b, set);
        w.pImageInfo = &i;
        writes.push_back(w);
    };
    const auto buffer = [&](const shader::Binding& b, VkBuffer buf) {
        VkDescriptorBufferInfo& i = buffers[writes.size()];
        i = {buf != VK_NULL_HANDLE ? buf : m_dummyBuffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w = shader::write(b, set);
        w.pBufferInfo = &i;
        writes.push_back(w);
    };

    // The G-buffer, always there after its init.
    image(L::gBuffer0, m_gbufferPass->getPositionView(), m_gbufferSampler);
    image(L::gBuffer1, m_gbufferPass->getNormalView(), m_gbufferSampler);
    image(L::gBuffer2, m_gbufferPass->getAlbedoView(), m_gbufferSampler);
    image(L::gBufferEmissive, m_gbufferPass->getEmissiveView(), m_gbufferSampler);
    image(L::gBufferSpecular, m_gbufferPass->getSpecularView(), m_gbufferSampler);   // OpenPBR's inputs
    image(L::gBufferOpenPBR, m_gbufferPass->getOpenPBRView(), m_gbufferSampler);
    image(L::gBufferLayers, m_gbufferPass->getLayersView(), m_gbufferSampler);       // OpenPBR's layers (uint)
    // The light table and the froxel lists; the dummy buffer without lights (the count is 0 then).
    if (m_lightBuffer == VK_NULL_HANDLE) m_lightCount = 0;
    buffer(L::lights, m_lightBuffer);
    buffer(L::clusters, m_clusterBuffer);
    // The CSM fallback: its map and its cascades.
    image(L::shadowMap, m_shadowMapView, m_shadowSampler != VK_NULL_HANDLE ? m_shadowSampler : fallbackSampler);
    buffer(L::cascades, m_cascadeBuffer);
    // Image-based lighting: the environment's irradiance and GGX-prefiltered levels (flag bit 0 clear without).
    const VkSampler iblSampler = m_iblSampler != VK_NULL_HANDLE ? m_iblSampler : fallbackSampler;
    image(L::envIrradiance, m_irradianceView, iblSampler);
    image(L::envPrefiltered, m_prefilteredView, iblSampler);
    // RT GI's diffuse and reflection, RT direct light's diffuse and specular (or stand-ins: their flags clear).
    const VkSampler giSampler = m_ssgiSampler != VK_NULL_HANDLE ? m_ssgiSampler : fallbackSampler;
    image(L::ssgiTexture, m_ssgiView, giSampler);
    image(L::giReflection, m_giReflectionView, giSampler);
    image(L::directDiffuse, m_directDiffuseView, m_gbufferSampler);
    image(L::directSpecular, m_directSpecularView, m_gbufferSampler);
    // OpenPBR's energy tables and the material table the layers index.
    buffer(L::openpbrTables, m_openpbrTables.buffer());
    buffer(L::layerMaterials, m_materialBuffer);

    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

bool DeferredLightingPass::createTarget(RenderTarget& target, VkImageUsageFlags usage, VkFormat format) {
    target.format = format;
    target.width = m_width;
    target.height = m_height;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = target.format;
    imageInfo.extent = {m_width, m_height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_device, &imageInfo, nullptr, &target.image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(m_device, target.image, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &target.memory) != VK_SUCCESS) {
        return false;
    }

    vkBindImageMemory(m_device, target.image, target.memory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = target.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = target.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    return vkCreateImageView(m_device, &viewInfo, nullptr, &target.view) == VK_SUCCESS;
}

bool DeferredLightingPass::createOutputImages() {
    return createTarget(m_hdrOutput,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |   // final image when post-processing is off: readback copy / swapchain blit
                            VK_IMAGE_USAGE_STORAGE_BIT) &&      // SSS blurs skin tiles in place
           // The subsurface pixels' diffuse irradiance and albedo, which the SSS pass spreads alone.
           createTarget(m_subsurfaceDiffuse, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        kSubsurfaceDiffuseFormat);
}

void DeferredLightingPass::destroyOutputImages() {
    m_hdrOutput.destroy(m_device);
    m_subsurfaceDiffuse.destroy(m_device);
}

bool DeferredLightingPass::createRenderPass() {
    // The lit image (RGBA16F) and the subsurface pixels' diffuse irradiance and albedo (RGBA32_UINT):
    // cleared, stored and left for shader reads.
    std::array<VkAttachmentDescription, 2> attachments{};
    std::array<VkAttachmentReference, 2> colorRefs{};
    for (uint32_t i = 0; i < 2; ++i) {
        VkAttachmentDescription& colorAttachment = attachments[i];
        colorAttachment.format = i == 0 ? VK_FORMAT_R16G16B16A16_SFLOAT : kSubsurfaceDiffuseFormat;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        colorRefs[i].attachment = i;
        colorRefs[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<uint32_t>(colorRefs.size());
    subpass.pColorAttachments = colorRefs.data();

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
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
    const std::array<VkImageView, 2> views = {m_hdrOutput.view, m_subsurfaceDiffuse.view};
    framebufferInfo.attachmentCount = static_cast<uint32_t>(views.size());
    framebufferInfo.pAttachments = views.data();
    framebufferInfo.width = m_width;
    framebufferInfo.height = m_height;
    framebufferInfo.layers = 1;

    return vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_framebuffer) == VK_SUCCESS;
}

bool DeferredLightingPass::createDescriptors() {
    // Bindings (every one read by the fragment stage):
    // 0-4: G-Buffer (position, normal, albedo, velocity, depth)
    // 5: The light table (SSBO, LightGpu records)
    // 6: Shadow map array (sampler2DArray, 4 cascades)
    // 7: The environment's irradiance (EnvPrefilter)
    const auto bindings = shader::layoutBindings(shader::core_deferred_lighting::kSet0,
                                                 shader::core_deferred_lighting::kStages);
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorLayout) != VK_SUCCESS) {
        return false;
    }

    // Descriptor pool — 16 image samplers, 1 UBO (cascade data), 4 SSBOs (lights, froxels, OpenPBR's
    // tables, the materials)
    std::array<VkDescriptorPoolSize, 3> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 16 * MAX_FRAMES_IN_FLIGHT;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 1 * MAX_FRAMES_IN_FLIGHT;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[2].descriptorCount = 4 * MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        return false;
    }

    // One set per frame in flight, all with the same layout
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
    layouts.fill(m_descriptorLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    allocInfo.pSetLayouts = layouts.data();

    return vkAllocateDescriptorSets(m_device, &allocInfo, m_descriptorSets.data()) == VK_SUCCESS;
}

bool DeferredLightingPass::createPipeline() {
    // Load shaders
    VkShaderModule vertShader = loadShaderModule("postprocess_fullscreen.slang.spv");
    VkShaderModule fragShader = loadShaderModule("core_deferred_lighting.slang.spv");

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

    // Unblended writes: the lit image, the subsurface pixels' diffuse light
    std::array<VkPipelineColorBlendAttachmentState, 2> colorBlendAttachments{};
    for (VkPipelineColorBlendAttachmentState& colorBlendAttachment : colorBlendAttachments) {
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = VK_FALSE;
    }

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = static_cast<uint32_t>(colorBlendAttachments.size());
    colorBlending.pAttachments = colorBlendAttachments.data();

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
