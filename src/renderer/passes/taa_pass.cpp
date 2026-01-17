#include "taa_pass.hpp"
#include <array>

namespace ohao {

// Halton sequence for sub-pixel jitter
static float halton(int index, int base) {
    float f = 1.0f;
    float r = 0.0f;
    while (index > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
        index /= base;
    }
    return r;
}

TAAPass::~TAAPass() {
    cleanup();
}

bool TAAPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;

    m_width = 1920;
    m_height = 1080;

    // Generate Halton sequence for jitter
    for (uint32_t i = 0; i < JITTER_SAMPLES; ++i) {
        m_jitterSequence[i] = glm::vec2(
            halton(i + 1, 2) - 0.5f,
            halton(i + 1, 3) - 0.5f
        );
    }

    if (!createHistoryBuffers()) return false;
    if (!createRenderPass()) return false;
    if (!createDescriptors()) return false;
    if (!createPipeline()) return false;

    return true;
}

void TAAPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    if (m_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_pipeline, nullptr);
    if (m_pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    if (m_descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    if (m_descriptorLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, m_descriptorLayout, nullptr);
    if (m_renderPass != VK_NULL_HANDLE) vkDestroyRenderPass(m_device, m_renderPass, nullptr);
    if (m_sampler != VK_NULL_HANDLE) vkDestroySampler(m_device, m_sampler, nullptr);

    destroyHistoryBuffers();

    m_pipeline = VK_NULL_HANDLE;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_descriptorPool = VK_NULL_HANDLE;
    m_descriptorLayout = VK_NULL_HANDLE;
    m_renderPass = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
}

void TAAPass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (m_currentFrameView == VK_NULL_HANDLE) return;

    uint32_t outputIndex = m_currentHistoryIndex;
    uint32_t historyIndex = 1 - m_currentHistoryIndex;

    VkClearValue clearValue{};
    clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_renderPass;
    renderPassInfo.framebuffer = m_framebuffers[outputIndex];
    renderPassInfo.renderArea.extent = {m_width, m_height};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {m_width, m_height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                            0, 1, &m_descriptorSets[historyIndex], 0, nullptr);

    TAAParams params{};
    params.texelSize = glm::vec2(1.0f / m_width, 1.0f / m_height);
    params.blendFactor = m_blendFactor;
    params.motionScale = m_motionScale;
    params.flags = 0;
    if (m_velocityView != VK_NULL_HANDLE) params.flags |= 1;
    if (m_useVarianceClipping) params.flags |= 2;

    vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), &params);

    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    // Swap history buffers for next frame
    swapHistoryBuffers();
}

void TAAPass::onResize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;

    vkDeviceWaitIdle(m_device);

    destroyHistoryBuffers();
    createHistoryBuffers();
    createDescriptors();
}

glm::vec2 TAAPass::getJitterOffset(uint32_t frameIndex) const {
    uint32_t sampleIndex = frameIndex % JITTER_SAMPLES;
    return m_jitterSequence[sampleIndex] / glm::vec2(m_width, m_height);
}

void TAAPass::swapHistoryBuffers() {
    m_currentHistoryIndex = 1 - m_currentHistoryIndex;
}

void TAAPass::setCurrentFrame(VkImageView currentFrame) {
    m_currentFrameView = currentFrame;
    updateDescriptorSets();
}

void TAAPass::setVelocityBuffer(VkImageView velocity) {
    m_velocityView = velocity;
    updateDescriptorSets();
}

void TAAPass::setDepthBuffer(VkImageView depth) {
    m_depthView = depth;
    updateDescriptorSets();
}

void TAAPass::updateDescriptorSets() {
    if (m_sampler == VK_NULL_HANDLE) return;

    // Update both descriptor sets (for ping-pong history)
    for (uint32_t i = 0; i < HISTORY_COUNT; ++i) {
        if (m_descriptorSets[i] == VK_NULL_HANDLE) continue;

        std::vector<VkWriteDescriptorSet> writes;
        std::vector<VkDescriptorImageInfo> imageInfos;
        imageInfos.reserve(4);

        // Binding 0: Current frame
        if (m_currentFrameView != VK_NULL_HANDLE) {
            VkDescriptorImageInfo& info = imageInfos.emplace_back();
            info.sampler = m_sampler;
            info.imageView = m_currentFrameView;
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet& write = writes.emplace_back();
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = m_descriptorSets[i];
            write.dstBinding = 0;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.descriptorCount = 1;
            write.pImageInfo = &imageInfos.back();
        }

        // Binding 1: History frame (opposite buffer)
        uint32_t historyIdx = 1 - i;
        if (m_historyViews[historyIdx] != VK_NULL_HANDLE) {
            VkDescriptorImageInfo& info = imageInfos.emplace_back();
            info.sampler = m_sampler;
            info.imageView = m_historyViews[historyIdx];
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet& write = writes.emplace_back();
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = m_descriptorSets[i];
            write.dstBinding = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.descriptorCount = 1;
            write.pImageInfo = &imageInfos.back();
        }

        // Binding 2: Velocity
        if (m_velocityView != VK_NULL_HANDLE) {
            VkDescriptorImageInfo& info = imageInfos.emplace_back();
            info.sampler = m_sampler;
            info.imageView = m_velocityView;
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet& write = writes.emplace_back();
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = m_descriptorSets[i];
            write.dstBinding = 2;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.descriptorCount = 1;
            write.pImageInfo = &imageInfos.back();
        }

        // Binding 3: Depth
        if (m_depthView != VK_NULL_HANDLE) {
            VkDescriptorImageInfo& info = imageInfos.emplace_back();
            info.sampler = m_sampler;
            info.imageView = m_depthView;
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet& write = writes.emplace_back();
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = m_descriptorSets[i];
            write.dstBinding = 3;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.descriptorCount = 1;
            write.pImageInfo = &imageInfos.back();
        }

        if (!writes.empty()) {
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }
    }
}

bool TAAPass::createHistoryBuffers() {
    VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;

    for (uint32_t i = 0; i < HISTORY_COUNT; ++i) {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = {m_width, m_height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_historyImages[i]) != VK_SUCCESS) {
            return false;
        }

        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(m_device, m_historyImages[i], &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_historyMemory[i]) != VK_SUCCESS) {
            return false;
        }

        vkBindImageMemory(m_device, m_historyImages[i], m_historyMemory[i], 0);

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_historyImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_historyViews[i]) != VK_SUCCESS) {
            return false;
        }
    }

    // Create sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    if (m_sampler == VK_NULL_HANDLE) {
        if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
            return false;
        }
    }

    return true;
}

void TAAPass::destroyHistoryBuffers() {
    for (auto& fb : m_framebuffers) {
        if (fb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_device, fb, nullptr);
            fb = VK_NULL_HANDLE;
        }
    }
    for (uint32_t i = 0; i < HISTORY_COUNT; ++i) {
        if (m_historyViews[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, m_historyViews[i], nullptr);
            m_historyViews[i] = VK_NULL_HANDLE;
        }
        if (m_historyImages[i] != VK_NULL_HANDLE) {
            vkDestroyImage(m_device, m_historyImages[i], nullptr);
            m_historyImages[i] = VK_NULL_HANDLE;
        }
        if (m_historyMemory[i] != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_historyMemory[i], nullptr);
            m_historyMemory[i] = VK_NULL_HANDLE;
        }
    }
}

bool TAAPass::createRenderPass() {
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

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;

    if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass) != VK_SUCCESS) {
        return false;
    }

    // Create framebuffers
    for (uint32_t i = 0; i < HISTORY_COUNT; ++i) {
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &m_historyViews[i];
        framebufferInfo.width = m_width;
        framebufferInfo.height = m_height;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_framebuffers[i]) != VK_SUCCESS) {
            return false;
        }
    }

    return true;
}

bool TAAPass::createDescriptors() {
    // Bindings: 0 = current frame, 1 = history frame, 2 = velocity, 3 = depth
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    for (uint32_t i = 0; i < 4; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (m_descriptorLayout == VK_NULL_HANDLE) {
        if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorLayout) != VK_SUCCESS) {
            return false;
        }
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 4 * HISTORY_COUNT;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = HISTORY_COUNT;

    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    }

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        return false;
    }

    std::array<VkDescriptorSetLayout, HISTORY_COUNT> layouts;
    layouts.fill(m_descriptorLayout);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = HISTORY_COUNT;
    allocInfo.pSetLayouts = layouts.data();

    if (vkAllocateDescriptorSets(m_device, &allocInfo, m_descriptorSets.data()) != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool TAAPass::createPipeline() {
    VkShaderModule vertShader = loadShaderModule("postprocess_fullscreen.vert.spv");
    VkShaderModule fragShader = loadShaderModule("postprocess_taa_resolve.frag.spv");

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
    pushConstant.size = sizeof(TAAParams);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descriptorLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
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
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = m_renderPass;

    VkResult result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                                nullptr, &m_pipeline);

    vkDestroyShaderModule(m_device, vertShader, nullptr);
    vkDestroyShaderModule(m_device, fragShader, nullptr);

    return result == VK_SUCCESS;
}

} // namespace ohao
