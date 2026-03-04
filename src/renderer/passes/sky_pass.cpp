#include "sky_pass.hpp"
#include <array>

namespace ohao {

SkyPass::~SkyPass() {
    cleanup();
}

bool SkyPass::initialize(VkDevice device, VkPhysicalDevice physicalDevice) {
    m_device = device;
    m_physicalDevice = physicalDevice;
    m_width  = 1920;
    m_height = 1080;

    if (!createSampler())      return false;
    if (!createRenderPass())   return false;
    if (!createDescriptors())  return false;
    if (!createPipeline())     return false;

    return true;
}

void SkyPass::cleanup() {
    if (m_device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(m_device);

    destroyFramebuffer();

    if (m_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }
    if (m_pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        m_pipelineLayout = VK_NULL_HANDLE;
    }
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_descriptorLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_descriptorLayout, nullptr);
        m_descriptorLayout = VK_NULL_HANDLE;
    }
    if (m_renderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(m_device, m_renderPass, nullptr);
        m_renderPass = VK_NULL_HANDLE;
    }
    if (m_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, m_sampler, nullptr);
        m_sampler = VK_NULL_HANDLE;
    }
    if (m_linearSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, m_linearSampler, nullptr);
        m_linearSampler = VK_NULL_HANDLE;
    }
}

void SkyPass::execute(VkCommandBuffer cmd, uint32_t /*frameIndex*/) {
    if (!m_enabled) return;
    if (m_framebuffer == VK_NULL_HANDLE || m_pipeline == VK_NULL_HANDLE) return;
    if (m_descriptorSet == VK_NULL_HANDLE) return;

    // Only update descriptors when views actually changed (resize etc.)
    // Updating every frame on a shared descriptor set causes a race condition
    // with in-flight frames at high refresh rates (144Hz+).
    if (m_descriptorsDirty) {
        updateDescriptors();
        m_descriptorsDirty = false;
    }

    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass        = m_renderPass;
    rpInfo.framebuffer       = m_framebuffer;
    rpInfo.renderArea.offset = {0, 0};
    rpInfo.renderArea.extent = {m_width, m_height};
    rpInfo.clearValueCount   = 0;  // LOAD_OP_LOAD — no clear needed

    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{};
    vp.width    = static_cast<float>(m_width);
    vp.height   = static_cast<float>(m_height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D scissor{};
    scissor.extent = {m_width, m_height};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipelineLayout, 0, 1, &m_descriptorSet, 0, nullptr);

    SkyParams params{};
    params.invViewProj   = m_invViewProj;
    params.sunDirection  = m_sunDirection;
    params.turbidity     = m_turbidity;
    params.cameraPos     = m_cameraPos;
    params.sunIntensity  = m_sunIntensity;
    params.groundColor   = m_groundColor;
    params.nightFactor   = m_nightFactor;
    params.moonDirection = m_moonDirection;
    params.starSeed      = m_starSeed;

    vkCmdPushConstants(cmd, m_pipelineLayout,
                       VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SkyParams), &params);

    vkCmdDraw(cmd, 3, 1, 0, 0);  // fullscreen triangle

    vkCmdEndRenderPass(cmd);
}

void SkyPass::onResize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;
    m_width  = width;
    m_height = height;
    // Framebuffer is recreated by setHDROutput() called from DeferredRenderer::onResize()
    destroyFramebuffer();
}

void SkyPass::setCameraData(const glm::mat4& invViewProj, const glm::vec3& cameraPos) {
    m_invViewProj = invViewProj;
    m_cameraPos   = cameraPos;
}

void SkyPass::setDepthBuffer(VkImageView depth) {
    m_depthView = depth;
    m_descriptorsDirty = true;
}

void SkyPass::setCloudBuffer(VkImageView view) {
    m_cloudView = view;
    m_descriptorsDirty = true;
}

void SkyPass::setHDROutput(VkImageView view, VkImage image) {
    m_hdrView  = view;
    m_hdrImage = image;
    // Recreate framebuffer if render pass is ready
    if (m_renderPass != VK_NULL_HANDLE && m_hdrView != VK_NULL_HANDLE) {
        destroyFramebuffer();
        createFramebuffer();
    }
}

// ---------------------------------------------------------------------------

bool SkyPass::createSampler() {
    // Depth sampler: NEAREST, no compare
    VkSamplerCreateInfo si{};
    si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter    = VK_FILTER_NEAREST;
    si.minFilter    = VK_FILTER_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.compareEnable = VK_FALSE;
    si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (vkCreateSampler(m_device, &si, nullptr, &m_sampler) != VK_SUCCESS) return false;

    // Linear sampler: for cloud buffer (bilinear upscale from half-res)
    VkSamplerCreateInfo ls{};
    ls.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ls.magFilter    = VK_FILTER_LINEAR;
    ls.minFilter    = VK_FILTER_LINEAR;
    ls.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ls.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ls.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ls.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    return vkCreateSampler(m_device, &ls, nullptr, &m_linearSampler) == VK_SUCCESS;
}

bool SkyPass::createRenderPass() {
    // Color attachment: HDR lighting output.
    // LOAD_OP_LOAD — preserve existing lit pixels; sky only writes empty pixels.
    // initialLayout = SHADER_READ_ONLY_OPTIMAL (left there by DeferredLightingPass)
    // finalLayout   = SHADER_READ_ONLY_OPTIMAL (needed by post-processing)
    VkAttachmentDescription colorAtt{};
    colorAtt.format         = VK_FORMAT_R16G16B16A16_SFLOAT;
    colorAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAtt.initialLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    colorAtt.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    // Transition from SHADER_READ_ONLY → COLOR_ATTACHMENT on subpass entry
    std::array<VkSubpassDependency, 2> deps{};
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    // Transition back to SHADER_READ_ONLY on subpass exit
    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments    = &colorAtt;
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;
    rpInfo.dependencyCount = static_cast<uint32_t>(deps.size());
    rpInfo.pDependencies   = deps.data();

    return vkCreateRenderPass(m_device, &rpInfo, nullptr, &m_renderPass) == VK_SUCCESS;
}

bool SkyPass::createFramebuffer() {
    if (m_hdrView == VK_NULL_HANDLE) return false;

    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass      = m_renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments    = &m_hdrView;
    fbInfo.width           = m_width;
    fbInfo.height          = m_height;
    fbInfo.layers          = 1;

    return vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_framebuffer) == VK_SUCCESS;
}

void SkyPass::destroyFramebuffer() {
    if (m_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
        m_framebuffer = VK_NULL_HANDLE;
    }
}

bool SkyPass::createDescriptors() {
    // Binding 0: depth buffer (NEAREST, DEPTH_STENCIL_READ_ONLY)
    // Binding 1: cloud buffer (LINEAR, GENERAL — half-res RGBA16F)
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr,
                                    &m_descriptorLayout) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 2;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets       = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes    = &poolSize;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr,
                               &m_descriptorPool) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts        = &m_descriptorLayout;

    return vkAllocateDescriptorSets(m_device, &allocInfo, &m_descriptorSet) == VK_SUCCESS;
}

void SkyPass::updateDescriptors() {
    if (m_descriptorSet == VK_NULL_HANDLE || m_sampler == VK_NULL_HANDLE) return;

    std::vector<VkWriteDescriptorSet> writes;

    // Binding 0: depth buffer
    VkDescriptorImageInfo depthInfo{};
    depthInfo.sampler     = m_sampler;
    depthInfo.imageView   = m_depthView;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    if (m_depthView != VK_NULL_HANDLE) {
        VkWriteDescriptorSet w{};
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet          = m_descriptorSet;
        w.dstBinding      = 0;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.descriptorCount = 1;
        w.pImageInfo      = &depthInfo;
        writes.push_back(w);
    }

    // Binding 1: cloud buffer (GENERAL layout, linear sampler)
    VkDescriptorImageInfo cloudInfo{};
    cloudInfo.sampler     = m_linearSampler;
    cloudInfo.imageView   = m_cloudView;
    cloudInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    if (m_cloudView != VK_NULL_HANDLE && m_linearSampler != VK_NULL_HANDLE) {
        VkWriteDescriptorSet w{};
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet          = m_descriptorSet;
        w.dstBinding      = 1;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.descriptorCount = 1;
        w.pImageInfo      = &cloudInfo;
        writes.push_back(w);
    }

    if (!writes.empty()) {
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
}

bool SkyPass::createPipeline() {
    VkShaderModule vertShader = loadShaderModule("postprocess_fullscreen.vert.spv");
    VkShaderModule fragShader = loadShaderModule("postprocess_sky.frag.spv");
    if (vertShader == VK_NULL_HANDLE || fragShader == VK_NULL_HANDLE) {
        if (vertShader) vkDestroyShaderModule(m_device, vertShader, nullptr);
        if (fragShader) vkDestroyShaderModule(m_device, fragShader, nullptr);
        return false;
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertShader;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragShader;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType     = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth  = 1.0f;
    rasterizer.cullMode   = VK_CULL_MODE_NONE;
    rasterizer.frontFace  = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAtt.blendEnable    = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments    = &blendAtt;

    std::array<VkDynamicState, 2> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates    = dynamicStates.data();

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(SkyParams);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &m_descriptorLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;

    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_device, vertShader, nullptr);
        vkDestroyShaderModule(m_device, fragShader, nullptr);
        return false;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount          = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages             = stages.data();
    pipelineInfo.pVertexInputState   = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState   = &multisampling;
    pipelineInfo.pColorBlendState    = &colorBlending;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = m_pipelineLayout;
    pipelineInfo.renderPass          = m_renderPass;
    pipelineInfo.subpass             = 0;

    VkResult result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1,
                                                 &pipelineInfo, nullptr, &m_pipeline);
    vkDestroyShaderModule(m_device, vertShader, nullptr);
    vkDestroyShaderModule(m_device, fragShader, nullptr);
    return result == VK_SUCCESS;
}

} // namespace ohao
