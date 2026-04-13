#include "post_processing_pipeline.hpp"
#include <array>

namespace ohao {

PostProcessingPipeline::~PostProcessingPipeline() {
    cleanup();
}

bool PostProcessingPipeline::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;

    m_width = 1920;
    m_height = 1080;

    // Initialize sub-passes
    m_bloomPass = std::make_unique<BloomPass>();
    if (!m_bloomPass->initialize(device, physicalDevice)) {
        return false;
    }

    m_taaPass = std::make_unique<TAAPass>();
    if (!m_taaPass->initialize(device, physicalDevice)) {
        return false;
    }

    m_ssaoPass = std::make_unique<SSAOPass>();
    if (!m_ssaoPass->initialize(device, physicalDevice)) {
        return false;
    }

    // Create tonemapping pass
    if (!createFinalOutput()) return false;
    if (!createTonemappingPass()) return false;

    return true;
}

void PostProcessingPipeline::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    // Cleanup sub-passes
    auto resetPass = [](auto& pass) {
        if (pass) { pass->cleanup(); pass.reset(); }
    };
    resetPass(m_bloomPass);
    resetPass(m_taaPass);
    resetPass(m_ssaoPass);

    // Cleanup tonemapping
    safeDestroy(m_tonemapPipeline);
    safeDestroy(m_tonemapLayout);
    safeDestroy(m_tonemapDescPool);
    safeDestroy(m_tonemapDescLayout);
    safeDestroy(m_tonemapFramebuffer);
    safeDestroy(m_tonemapRenderPass);
    safeDestroy(m_sampler);

    destroyFinalOutput();
}

void PostProcessingPipeline::executeSSAO(VkCommandBuffer cmd, uint32_t frameIndex) {
    // SSAO runs before lighting so its output can modulate ambient occlusion
    if (m_ssaoEnabled && m_ssaoPass) {
        m_ssaoPass->execute(cmd, frameIndex);
    }
}

void PostProcessingPipeline::execute(VkCommandBuffer cmd, uint32_t frameIndex) {
    m_didExecute = false;

    if (m_hdrInputView == VK_NULL_HANDLE) return;

    VkImageView currentInput = m_hdrInputView;

    // ── 1. Bloom ──────────────────────────────────────────────────────
    float bloomStrength = 0.0f;
    if (m_bloomEnabled && m_bloomPass) {
        m_bloomPass->setInputImage(currentInput);
        m_bloomPass->execute(cmd, frameIndex);
        bloomStrength = 1.0f;
    }

    // ── 2. TAA (temporal anti-aliasing) ───────────────────────────────
    if (m_taaEnabled && m_taaPass) {
        m_taaPass->setCurrentFrame(currentInput);
        m_taaPass->execute(cmd, frameIndex);
        VkImageView taaOutput = m_taaPass->getOutputView();
        if (taaOutput != VK_NULL_HANDLE) currentInput = taaOutput;
    }

    // ── 3. Tonemapping: HDR → LDR ─────────────────────────────────────
    if (m_tonemappingEnabled && m_tonemapPipeline != VK_NULL_HANDLE &&
        m_tonemapFramebuffer != VK_NULL_HANDLE && m_tonemapDescSet != VK_NULL_HANDLE) {

        updateTonemapInput(currentInput);

        if (bloomStrength > 0.0f && m_bloomPass) {
            VkImageView bloomView = m_bloomPass->getBloomOutput();
            if (bloomView != VK_NULL_HANDLE) {
                VkDescriptorImageInfo bloomInfo{};
                bloomInfo.sampler = m_sampler;
                bloomInfo.imageView = bloomView;
                bloomInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                VkWriteDescriptorSet write{};
                write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = m_tonemapDescSet;
                write.dstBinding = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.descriptorCount = 1;
                write.pImageInfo = &bloomInfo;

                vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
            }
        }

        VkClearValue clearValue{};
        clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = m_tonemapRenderPass;
        renderPassInfo.framebuffer = m_tonemapFramebuffer;
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = {m_width, m_height};
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &clearValue;

        vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{};
        viewport.width = static_cast<float>(m_width);
        viewport.height = static_cast<float>(m_height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.extent = {m_width, m_height};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tonemapPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_tonemapLayout, 0, 1, &m_tonemapDescSet, 0, nullptr);

        TonemapParams params{};
        params.exposure       = m_exposure;
        params.gamma          = m_gamma;
        params.bloomStrength  = bloomStrength;
        params.tonemapOp      = static_cast<uint32_t>(m_tonemapOp);
        params.flashIntensity = m_flashIntensity;
        params.paddingF       = 0.0f;

        vkCmdPushConstants(cmd, m_tonemapLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(params), &params);

        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);

        m_didExecute = true;
    }
}

void PostProcessingPipeline::onResize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;

    vkDeviceWaitIdle(m_device);

    // Resize sub-passes
    if (m_bloomPass) m_bloomPass->onResize(width, height);
    if (m_taaPass) m_taaPass->onResize(width, height);
    if (m_ssaoPass) m_ssaoPass->onResize(width, height);

    // Recreate final output
    safeDestroy(m_tonemapFramebuffer);
    destroyFinalOutput();
    createFinalOutput();

    // Recreate framebuffer
    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = m_tonemapRenderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = &m_finalOutputView;
    framebufferInfo.width = m_width;
    framebufferInfo.height = m_height;
    framebufferInfo.layers = 1;

    vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_tonemapFramebuffer);
}

void PostProcessingPipeline::setDepthBuffer(VkImageView depth) {
    if (m_ssaoPass) m_ssaoPass->setDepthBuffer(depth);
    if (m_taaPass) m_taaPass->setDepthBuffer(depth);
}

void PostProcessingPipeline::setNormalBuffer(VkImageView normal) {
    if (m_ssaoPass) m_ssaoPass->setNormalBuffer(normal);
}

void PostProcessingPipeline::setVelocityBuffer(VkImageView velocity) {
    if (m_taaPass) m_taaPass->setVelocityBuffer(velocity);
}

void PostProcessingPipeline::setBloomThreshold(float threshold) {
    if (m_bloomPass) m_bloomPass->setThreshold(threshold);
}

void PostProcessingPipeline::setBloomIntensity(float intensity) {
    if (m_bloomPass) m_bloomPass->setIntensity(intensity);
}

void PostProcessingPipeline::setTAABlendFactor(float factor) {
    if (m_taaPass) m_taaPass->setBlendFactor(factor);
}

glm::vec2 PostProcessingPipeline::getJitterOffset(uint32_t frameIndex) const {
    if (m_taaPass && m_taaEnabled) {
        return m_taaPass->getJitterOffset(frameIndex);
    }
    return glm::vec2(0.0f);
}

void PostProcessingPipeline::setSSAORadius(float radius) {
    if (m_ssaoPass) m_ssaoPass->setRadius(radius);
}

void PostProcessingPipeline::setSSAOIntensity(float intensity) {
    if (m_ssaoPass) m_ssaoPass->setIntensity(intensity);
}

void PostProcessingPipeline::setProjectionMatrix(const glm::mat4& proj, const glm::mat4& invProj) {
    if (m_ssaoPass) m_ssaoPass->setProjectionMatrix(proj, invProj);
}

VkImageView PostProcessingPipeline::getSSAOOutput() const {
    if (m_ssaoPass) return m_ssaoPass->getOutputView();
    return VK_NULL_HANDLE;
}

VkImage PostProcessingPipeline::getSSAOImage() const {
    if (m_ssaoPass) return m_ssaoPass->getOutputImage();
    return VK_NULL_HANDLE;
}

VkSampler PostProcessingPipeline::getSSAOSampler() const {
    if (m_ssaoPass) return m_ssaoPass->getSampler();
    return VK_NULL_HANDLE;
}

void PostProcessingPipeline::setHDRInput(VkImageView hdrInput) {
    m_hdrInputView = hdrInput;
    updateTonemapDescriptors();
}

void PostProcessingPipeline::setHDRInputWithImage(VkImageView hdrInput, VkImage /*hdrImage*/) {
    m_hdrInputView  = hdrInput;
    updateTonemapDescriptors();
}

void PostProcessingPipeline::updateTonemapDescriptors() {
    if (m_tonemapDescSet == VK_NULL_HANDLE || m_sampler == VK_NULL_HANDLE) return;
    if (m_hdrInputView == VK_NULL_HANDLE) return;

    // Write all 3 bindings: HDR input, bloom, SSR
    std::array<VkDescriptorImageInfo, 3> imageInfos{};
    std::array<VkWriteDescriptorSet, 3> writes{};

    // Binding 0: HDR input
    imageInfos[0].sampler = m_sampler;
    imageInfos[0].imageView = m_hdrInputView;
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = m_tonemapDescSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &imageInfos[0];

    // Binding 1: Bloom texture — use HDR input as fallback
    VkImageView bloomView = m_bloomPass ? m_bloomPass->getBloomOutput() : VK_NULL_HANDLE;
    imageInfos[1].sampler = m_sampler;
    imageInfos[1].imageView = bloomView != VK_NULL_HANDLE ? bloomView : m_hdrInputView;
    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = m_tonemapDescSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &imageInfos[1];

    // Binding 2: SSR texture — use HDR input as fallback (zero reflections)
    imageInfos[2].sampler = m_sampler;
    imageInfos[2].imageView = m_ssrView != VK_NULL_HANDLE ? m_ssrView : m_hdrInputView;
    imageInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = m_tonemapDescSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].descriptorCount = 1;
    writes[2].pImageInfo = &imageInfos[2];

    vkUpdateDescriptorSets(m_device, 3, writes.data(), 0, nullptr);
}

bool PostProcessingPipeline::createFinalOutput() {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM; // LDR output — shader does manual gamma, no HW sRGB
    imageInfo.extent = {m_width, m_height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_device, &imageInfo, nullptr, &m_finalOutput) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(m_device, m_finalOutput, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_finalMemory) != VK_SUCCESS) {
        return false;
    }

    vkBindImageMemory(m_device, m_finalOutput, m_finalMemory, 0);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_finalOutput;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    return vkCreateImageView(m_device, &viewInfo, nullptr, &m_finalOutputView) == VK_SUCCESS;
}

void PostProcessingPipeline::destroyFinalOutput() {
    safeDestroy(m_finalOutputView);
    safeDestroy(m_finalOutput);
    safeFree(m_finalMemory);
}

bool PostProcessingPipeline::createTonemappingPass() {
    // Create sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
        return false;
    }

    // Render pass
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = VK_FORMAT_R8G8B8A8_UNORM;
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

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;

    if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_tonemapRenderPass) != VK_SUCCESS) {
        return false;
    }

    // Framebuffer
    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = m_tonemapRenderPass;
    framebufferInfo.attachmentCount = 1;
    framebufferInfo.pAttachments = &m_finalOutputView;
    framebufferInfo.width = m_width;
    framebufferInfo.height = m_height;
    framebufferInfo.layers = 1;

    if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_tonemapFramebuffer) != VK_SUCCESS) {
        return false;
    }

    // Descriptor layout: HDR input + bloom + SSR
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_tonemapDescLayout) != VK_SUCCESS) {
        return false;
    }

    // Descriptor pool
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 3;  // HDR + bloom + SSR

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 1;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_tonemapDescPool) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_tonemapDescPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_tonemapDescLayout;

    if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_tonemapDescSet) != VK_SUCCESS) {
        return false;
    }

    // Load shaders
    VkShaderModule vertShader = loadShaderModule("postprocess_fullscreen.vert.spv");
    VkShaderModule fragShader = loadShaderModule("postprocess_tonemapping.frag.spv");

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragShader;
    stages[1].pName = "main";

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

    std::array<VkDynamicState, 2> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstant.offset = 0;
    pushConstant.size = sizeof(TonemapParams);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_tonemapDescLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_tonemapLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, vertShader, nullptr);
        vkDestroyShaderModule(m_device, fragShader, nullptr);
        return false;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_tonemapLayout;
    pipelineInfo.renderPass = m_tonemapRenderPass;

    VkResult result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                nullptr, &m_tonemapPipeline);

    vkDestroyShaderModule(m_device, vertShader, nullptr);
    vkDestroyShaderModule(m_device, fragShader, nullptr);

    return result == VK_SUCCESS;
}


// ===================================================================
// Dynamic tonemapping input rebind
// ===================================================================

void PostProcessingPipeline::updateTonemapInput(VkImageView input) {
    if (m_tonemapDescSet == VK_NULL_HANDLE || m_sampler == VK_NULL_HANDLE) return;
    if (input == VK_NULL_HANDLE) return;

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = m_sampler;
    imageInfo.imageView = input;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_tonemapDescSet;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
}

} // namespace ohao
