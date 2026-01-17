#include "bloom_pass.hpp"
#include <algorithm>
#include <cmath>

namespace ohao {

BloomPass::~BloomPass() {
    cleanup();
}

bool BloomPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;

    m_width = 1920;
    m_height = 1080;

    if (!createMipChain()) return false;
    if (!createRenderPasses()) return false;
    if (!createDescriptors()) return false;
    if (!createPipelines()) return false;

    return true;
}

void BloomPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(m_device);

    // Pipelines
    if (m_thresholdPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_thresholdPipeline, nullptr);
    if (m_downsamplePipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_downsamplePipeline, nullptr);
    if (m_upsamplePipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_upsamplePipeline, nullptr);
    if (m_thresholdLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_thresholdLayout, nullptr);
    if (m_downsampleLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_downsampleLayout, nullptr);
    if (m_upsampleLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_upsampleLayout, nullptr);

    // Descriptors
    if (m_descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    if (m_inputLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, m_inputLayout, nullptr);

    // Render passes
    if (m_thresholdRenderPass != VK_NULL_HANDLE) vkDestroyRenderPass(m_device, m_thresholdRenderPass, nullptr);
    if (m_downsampleRenderPass != VK_NULL_HANDLE) vkDestroyRenderPass(m_device, m_downsampleRenderPass, nullptr);
    if (m_upsampleRenderPass != VK_NULL_HANDLE) vkDestroyRenderPass(m_device, m_upsampleRenderPass, nullptr);

    // Sampler
    if (m_sampler != VK_NULL_HANDLE) vkDestroySampler(m_device, m_sampler, nullptr);

    destroyMipChain();

    m_thresholdPipeline = VK_NULL_HANDLE;
    m_downsamplePipeline = VK_NULL_HANDLE;
    m_upsamplePipeline = VK_NULL_HANDLE;
    m_thresholdLayout = VK_NULL_HANDLE;
    m_downsampleLayout = VK_NULL_HANDLE;
    m_upsampleLayout = VK_NULL_HANDLE;
    m_descriptorPool = VK_NULL_HANDLE;
    m_inputLayout = VK_NULL_HANDLE;
    m_thresholdRenderPass = VK_NULL_HANDLE;
    m_downsampleRenderPass = VK_NULL_HANDLE;
    m_upsampleRenderPass = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE;
}

void BloomPass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    // Validate all resources before executing
    if (m_hdrInputView == VK_NULL_HANDLE) return;
    if (m_thresholdRenderPass == VK_NULL_HANDLE ||
        m_downsampleRenderPass == VK_NULL_HANDLE ||
        m_upsampleRenderPass == VK_NULL_HANDLE) return;
    if (m_thresholdPipeline == VK_NULL_HANDLE ||
        m_downsamplePipeline == VK_NULL_HANDLE ||
        m_upsamplePipeline == VK_NULL_HANDLE) return;
    if (m_mipLevels == 0 || m_framebuffers[0] == VK_NULL_HANDLE) return;

    executeThreshold(cmd);
    executeDownsample(cmd);
    executeUpsample(cmd);
}

void BloomPass::onResize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;

    vkDeviceWaitIdle(m_device);

    destroyMipChain();
    createMipChain();
    createDescriptors();
}

void BloomPass::setInputImage(VkImageView hdrInput) {
    m_hdrInputView = hdrInput;

    // Update descriptor set 0 with new HDR input
    if (m_hdrInputView != VK_NULL_HANDLE && !m_descriptorSets.empty() && m_sampler != VK_NULL_HANDLE) {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = m_sampler;
        imageInfo.imageView = m_hdrInputView;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_descriptorSets[0];
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }
}

bool BloomPass::createMipChain() {
    // Calculate mip levels
    m_mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(m_width, m_height)))) + 1;
    m_mipLevels = std::min(m_mipLevels, MAX_MIP_LEVELS);

    // Calculate mip sizes
    uint32_t w = m_width;
    uint32_t h = m_height;
    for (uint32_t i = 0; i < m_mipLevels; ++i) {
        m_mipSizes[i] = glm::uvec2(w, h);
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }

    // Create bloom image with mip chain
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imageInfo.extent = {m_width, m_height, 1};
    imageInfo.mipLevels = m_mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(m_device, &imageInfo, nullptr, &m_bloomImage) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(m_device, m_bloomImage, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_bloomMemory) != VK_SUCCESS) {
        return false;
    }

    vkBindImageMemory(m_device, m_bloomImage, m_bloomMemory, 0);

    // Create per-mip views
    for (uint32_t i = 0; i < m_mipLevels; ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_bloomImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = i;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_mipViews[i]) != VK_SUCCESS) {
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
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    if (m_sampler == VK_NULL_HANDLE) {
        if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
            return false;
        }
    }

    return true;
}

void BloomPass::destroyMipChain() {
    for (auto& fb : m_framebuffers) {
        if (fb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(m_device, fb, nullptr);
            fb = VK_NULL_HANDLE;
        }
    }
    for (auto& view : m_mipViews) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, view, nullptr);
            view = VK_NULL_HANDLE;
        }
    }
    if (m_bloomImage != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_bloomImage, nullptr);
        m_bloomImage = VK_NULL_HANDLE;
    }
    if (m_bloomMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_bloomMemory, nullptr);
        m_bloomMemory = VK_NULL_HANDLE;
    }
}

bool BloomPass::createRenderPasses() {
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
    dependency.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
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

    if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_thresholdRenderPass) != VK_SUCCESS) {
        return false;
    }

    // Same render pass for downsample/upsample
    if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_downsampleRenderPass) != VK_SUCCESS) {
        return false;
    }

    // Upsample needs to blend, so load instead of clear
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_upsampleRenderPass) != VK_SUCCESS) {
        return false;
    }

    // Create framebuffers for each mip level
    // Note: Framebuffer 0 is used by threshold pass, others by downsample
    // All render passes share compatible attachment format, so we can use any
    // However, MoltenVK may require matching render pass, so we use the appropriate one
    for (uint32_t i = 0; i < m_mipLevels; ++i) {
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        // Use threshold pass for mip 0, downsample for others
        framebufferInfo.renderPass = (i == 0) ? m_thresholdRenderPass : m_downsampleRenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &m_mipViews[i];
        framebufferInfo.width = m_mipSizes[i].x;
        framebufferInfo.height = m_mipSizes[i].y;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_framebuffers[i]) != VK_SUCCESS) {
            return false;
        }
    }

    return true;
}

bool BloomPass::createDescriptors() {
    // Simple input sampler layout
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;

    if (m_inputLayout == VK_NULL_HANDLE) {
        if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_inputLayout) != VK_SUCCESS) {
            return false;
        }
    }

    // Pool for mip chain descriptors + input descriptor
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = m_mipLevels + 1; // +1 for HDR input

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = m_mipLevels + 1;

    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    }

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        return false;
    }

    // Allocate descriptor sets
    m_descriptorSets.resize(m_mipLevels + 1);
    std::vector<VkDescriptorSetLayout> layouts(m_mipLevels + 1, m_inputLayout);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    allocInfo.pSetLayouts = layouts.data();

    if (vkAllocateDescriptorSets(m_device, &allocInfo, m_descriptorSets.data()) != VK_SUCCESS) {
        return false;
    }

    // Update descriptor sets
    // Set 0: HDR input (updated when setInputImage is called)
    // Sets 1-N: Mip levels for downsampling/upsampling
    for (uint32_t i = 0; i < m_mipLevels; ++i) {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = m_sampler;
        imageInfo.imageView = m_mipViews[i];
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = m_descriptorSets[i + 1];
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
    }

    return true;
}

bool BloomPass::createPipelines() {
    // Load shaders
    VkShaderModule fullscreenVert = loadShaderModule("postprocess_fullscreen.vert.spv");
    VkShaderModule thresholdFrag = loadShaderModule("postprocess_bloom_threshold.frag.spv");
    VkShaderModule downsampleFrag = loadShaderModule("postprocess_bloom_downsample.frag.spv");
    VkShaderModule upsampleFrag = loadShaderModule("postprocess_bloom_upsample.frag.spv");

    // Common pipeline state
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

    // Threshold pipeline
    {
        VkPushConstantRange pushConstant{};
        pushConstant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstant.offset = 0;
        pushConstant.size = sizeof(ThresholdParams);

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &m_inputLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushConstant;

        vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_thresholdLayout);

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = fullscreenVert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = thresholdFrag;
        stages[1].pName = "main";

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
        pipelineInfo.layout = m_thresholdLayout;
        pipelineInfo.renderPass = m_thresholdRenderPass;

        vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_thresholdPipeline);
    }

    // Downsample pipeline
    {
        VkPushConstantRange pushConstant{};
        pushConstant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstant.offset = 0;
        pushConstant.size = sizeof(SampleParams);

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &m_inputLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushConstant;

        vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_downsampleLayout);

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = fullscreenVert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = downsampleFrag;
        stages[1].pName = "main";

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
        pipelineInfo.layout = m_downsampleLayout;
        pipelineInfo.renderPass = m_downsampleRenderPass;

        vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_downsamplePipeline);
    }

    // Upsample pipeline (with additive blending)
    {
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &m_inputLayout;

        VkPushConstantRange pushConstant{};
        pushConstant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstant.offset = 0;
        pushConstant.size = sizeof(SampleParams);
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushConstant;

        vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_upsampleLayout);

        colorBlendAttachment.blendEnable = VK_TRUE;
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = fullscreenVert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = upsampleFrag;
        stages[1].pName = "main";

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
        pipelineInfo.layout = m_upsampleLayout;
        pipelineInfo.renderPass = m_upsampleRenderPass;

        vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_upsamplePipeline);
    }

    vkDestroyShaderModule(m_device, fullscreenVert, nullptr);
    vkDestroyShaderModule(m_device, thresholdFrag, nullptr);
    vkDestroyShaderModule(m_device, downsampleFrag, nullptr);
    vkDestroyShaderModule(m_device, upsampleFrag, nullptr);

    return true;
}

void BloomPass::executeThreshold(VkCommandBuffer cmd) {
    // Threshold pass: HDR input -> mip 0
    VkClearValue clearValue{};
    clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = m_thresholdRenderPass;
    renderPassInfo.framebuffer = m_framebuffers[0];
    renderPassInfo.renderArea.extent = {m_mipSizes[0].x, m_mipSizes[0].y};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearValue;

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{0.0f, 0.0f, static_cast<float>(m_mipSizes[0].x),
                        static_cast<float>(m_mipSizes[0].y), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {m_mipSizes[0].x, m_mipSizes[0].y}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_thresholdPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_thresholdLayout,
                            0, 1, &m_descriptorSets[0], 0, nullptr);

    ThresholdParams params{m_threshold, m_softThreshold, m_intensity, 0.0f};
    vkCmdPushConstants(cmd, m_thresholdLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), &params);

    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

void BloomPass::executeDownsample(VkCommandBuffer cmd) {
    // Downsample chain: mip N -> mip N+1
    for (uint32_t i = 1; i < m_mipLevels; ++i) {
        VkClearValue clearValue{};
        clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = m_downsampleRenderPass;
        renderPassInfo.framebuffer = m_framebuffers[i];
        renderPassInfo.renderArea.extent = {m_mipSizes[i].x, m_mipSizes[i].y};
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &clearValue;

        vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{0.0f, 0.0f, static_cast<float>(m_mipSizes[i].x),
                            static_cast<float>(m_mipSizes[i].y), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {m_mipSizes[i].x, m_mipSizes[i].y}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_downsamplePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_downsampleLayout,
                                0, 1, &m_descriptorSets[i], 0, nullptr); // Previous mip

        SampleParams params{
            glm::vec2(1.0f / m_mipSizes[i - 1].x, 1.0f / m_mipSizes[i - 1].y),
            m_filterRadius, 0.0f
        };
        vkCmdPushConstants(cmd, m_downsampleLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), &params);

        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }
}

void BloomPass::executeUpsample(VkCommandBuffer cmd) {
    // Upsample chain: mip N -> mip N-1 (additive)
    for (int32_t i = static_cast<int32_t>(m_mipLevels) - 2; i >= 0; --i) {
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = m_upsampleRenderPass;
        renderPassInfo.framebuffer = m_framebuffers[i];
        renderPassInfo.renderArea.extent = {m_mipSizes[i].x, m_mipSizes[i].y};
        renderPassInfo.clearValueCount = 0;

        vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{0.0f, 0.0f, static_cast<float>(m_mipSizes[i].x),
                            static_cast<float>(m_mipSizes[i].y), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {m_mipSizes[i].x, m_mipSizes[i].y}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_upsamplePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_upsampleLayout,
                                0, 1, &m_descriptorSets[i + 2], 0, nullptr); // Next mip (smaller)

        SampleParams params{
            glm::vec2(1.0f / m_mipSizes[i + 1].x, 1.0f / m_mipSizes[i + 1].y),
            m_filterRadius, 0.5f // Blend factor
        };
        vkCmdPushConstants(cmd, m_upsampleLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(params), &params);

        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }
}

} // namespace ohao
